// winapfs - APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// winapfs-tray: a notification-area app for each signed-in user. It shows the
// APFS volumes the service sees and acts on them through the service's control
// pipe (Control.h), so it never needs administrator rights itself:
//   - per volume: Open, Mount / Unmount, Unlock... (encrypted, asks for the
//     password), Eject disk (safely remove), Read-write, Mount automatically
//   - Allow writing to disks (experimental; runs `winapfs service config`
//     elevated), Rescan, Start/install the service, About (WinFsp notice)
//   - notifications when volumes are mounted or unmounted, and when an
//     encrypted volume waits for its password (click to unlock)
//
//   winapfs-tray [--pipe <name>]   (default: the service; WINAPFS_PIPE too)

#include "Control.h"
#include "Dashboard.h"
#include "Updater.h"
#include "resource.h"

#include <Windows.h>
#include <commctrl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace winapfs;

namespace {

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_ACTION_DONE = WM_APP + 2;          // lParam: new std::wstring (message) or null
constexpr UINT WM_POLL_DONE = WM_APP + 3;            // lParam: new PollUiUpdate
constexpr UINT WM_UPDATE_RESULT = WM_APP + 4;        // lParam: new UpdateResultMsg
constexpr UINT WM_UPDATE_DOWNLOAD_DONE = WM_APP + 5; // lParam: new UpdateDownloadMsg
constexpr UINT_PTR TIMER_POLL = 1;
constexpr UINT_PTR TIMER_STARTUP_UPDATE = 2;
constexpr UINT kPollMs = 2500;

enum MenuId : UINT
{
	ID_OPEN_DASHBOARD = 99,
	ID_ALLOW_WRITES = 100,
	ID_RESCAN,
	ID_SERVICE_START,
	ID_SERVICE_INSTALL,
	ID_CHECK_UPDATES,
	ID_ABOUT,
	ID_EXIT,
	ID_VOLUME_BASE = 1000,   // + 10 * volume index + action
};

enum VolumeAction : UINT
{
	VA_OPEN,
	VA_MOUNT,
	VA_UNMOUNT,
	VA_UNLOCK,
	VA_EJECT,
	VA_READWRITE,
	VA_AUTOMOUNT,
};

HINSTANCE g_inst;
HWND g_wnd;
NOTIFYICONDATAW g_nid;
UINT g_taskbar_created;
UINT g_show_dashboard_msg;
std::wstring g_pipe;

std::mutex g_mutex;                    // the state below (the poller thread writes it)
std::vector<VolumeStatus> g_vols;
bool g_allow_writes = false;
bool g_service_up = false;
bool g_service_installed = false;
std::map<std::string, std::string> g_seen;   // uuid -> "state|letter|mode" at the last poll
bool g_first_poll = true;
std::string g_balloon_uuid;            // a locked volume the last notification was about
std::atomic<bool> g_polling{ false };
std::atomic<bool> g_exiting{ false };
std::atomic<bool> g_checking_update{ false };
std::atomic<bool> g_downloading_update{ false };
UpdateInfo g_update;
std::string g_update_status_text;
DashboardState g_last_dash;
bool g_last_balloon_is_update = false;

struct PollUiUpdate
{
	std::wstring tip;
	std::wstring notify_title;
	std::wstring notify_text;
	bool has_locked_note = false;
	DashboardState dash;
};

struct UpdateResultMsg
{
	UpdateInfo info;
	bool interactive = false;
};

struct UpdateDownloadMsg
{
	bool ok = false;
	std::wstring path;
	std::string err;
};

void SyncDashboardState(DashboardState s)
{
	s.update_checking = g_checking_update.load();
	s.update_downloading = g_downloading_update.load();
	s.update_available = g_update.ok && g_update.update_available;
	s.update_version = g_update.latest_version;
	s.update_status = g_update_status_text;
	g_last_dash = s;
	DashboardUpdate(g_last_dash);
}

HWND DialogOwner()
{
	HWND dash = DashboardHwnd();
	return (dash && IsWindowVisible(dash)) ? dash : g_wnd;
}

std::wstring W(const std::string &s)
{
	if (s.empty())
		return L"";
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
	return w;
}

std::string U(const std::wstring &w)
{
	if (w.empty())
		return "";
	int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
	return s;
}

std::wstring WinapfsExe()
{
	wchar_t buf[MAX_PATH];
	GetModuleFileNameW(nullptr, buf, MAX_PATH);
	std::wstring p = buf;
	return p.substr(0, p.rfind(L'\\') + 1) + L"winapfs.exe";
}

void Notify(const std::wstring &title, const std::wstring &text, DWORD flags = NIIF_INFO)
{
	NOTIFYICONDATAW n = g_nid;
	n.uFlags = NIF_INFO;
	wcsncpy_s(n.szInfoTitle, title.c_str(), _TRUNCATE);
	wcsncpy_s(n.szInfo, text.c_str(), _TRUNCATE);
	n.dwInfoFlags = flags | NIIF_RESPECT_QUIET_TIME;
	Shell_NotifyIconW(NIM_MODIFY, &n);
}

void SetTip(const std::wstring &tip)
{
	g_nid.uFlags = NIF_TIP | NIF_SHOWTIP;
	wcsncpy_s(g_nid.szTip, tip.c_str(), _TRUNCATE);
	Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

bool IsPhysical(const VolumeStatus &v)
{
	return v.disk.compare(0, 4, "\\\\.\\") == 0;
}

std::wstring StateText(const VolumeStatus &v)
{
	if (v.state == "mounted")
		return v.mode == "rw" ? L"read-write" : L"read-only";
	if (v.state == "locked")
		return L"locked";
	if (v.state == "unmounted")
		return L"unmounted";
	if (v.state == "disabled")
		return L"not mounted automatically";
	return L"not mounted";
}

// ---------------------------------------------------------------- polling

bool ServiceInstalled();

void StartPollAsync()
{
	if (g_exiting.load() || g_polling.exchange(true))
		return;
	std::thread([] {
		std::vector<VolumeStatus> vols;
		bool allow = false;
		std::string err;
		bool up = ControlList(g_pipe, vols, allow, err);
		bool installed = up ? true : ServiceInstalled();

		std::vector<std::pair<std::wstring, std::wstring>> notes;   // title, text
		std::string locked_uuid;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if (!g_first_poll && up)
				for (const auto &v : vols)
				{
					std::string now = v.state + "|" + v.letter + "|" + v.mode;
					auto it = g_seen.find(v.uuid);
					std::string before = it == g_seen.end() ? "" : it->second;
					if (before == now)
						continue;
					if (v.state == "mounted")
						notes.emplace_back(W(v.name) + L" is ready", L"Mounted at " + W(v.letter) + L" (" + StateText(v) + L")");
					else if (v.state == "locked" && before.compare(0, 6, "locked") != 0)
					{
						notes.emplace_back(W(v.name) + L" is encrypted", L"Click here to unlock it with its password.");
						locked_uuid = v.uuid;
					}
					else if (before.compare(0, 7, "mounted") == 0)
						notes.emplace_back(W(v.name) + L" unmounted", v.note.empty() ? L"It is no longer mounted." : W(v.note));
				}
			if (up && !g_first_poll)
				for (const auto &kv : g_seen)
				{
					bool still = false;
					for (const auto &v : vols)
						still = still || v.uuid == kv.first;
					if (!still && kv.second.compare(0, 7, "mounted") == 0)
						notes.emplace_back(L"APFS disk removed", L"Its volumes were unmounted.");
				}
			g_seen.clear();
			for (const auto &v : vols)
				g_seen[v.uuid] = v.state + "|" + v.letter + "|" + v.mode;
			if (!locked_uuid.empty())
				g_balloon_uuid = locked_uuid;
			g_first_poll = !up;
			g_vols = vols;
			g_allow_writes = allow;
			g_service_up = up;
			g_service_installed = installed;
		}

		size_t mounted = 0;
		for (const auto &v : vols)
			mounted += v.state == "mounted";
		auto *upd = new PollUiUpdate;
		upd->dash.service_up = up;
		upd->dash.service_installed = installed;
		upd->dash.allow_writes = allow;
		for (const auto &v : vols)
		{
			VolumeCardData card;
			static_cast<VolumeStatus &>(card) = v;
			if (v.state == "mounted" && v.letter.size() >= 2 && v.letter[1] == ':')
			{
				std::wstring root = W(v.letter) + L"\\";
				ULARGE_INTEGER avail{}, total{}, free_b{};
				if (GetDiskFreeSpaceExW(root.c_str(), &avail, &total, &free_b))
				{
					card.has_space = true;
					card.total_bytes = total.QuadPart;
					card.free_bytes = avail.QuadPart;
				}
			}
			upd->dash.vols.push_back(std::move(card));
		}

		if (!up)
			upd->tip = L"winapfs: the service is not running";
		else if (vols.empty())
			upd->tip = L"winapfs: no APFS disks";
		else
			upd->tip = L"winapfs: " + std::to_wstring(mounted) + L" of " + std::to_wstring(vols.size()) + L" APFS volumes mounted";
		if (!notes.empty())
		{
			upd->notify_title = notes.back().first;
			upd->notify_text = notes.size() == 1 ? notes.back().second : notes.back().second + L" (and more)";
			upd->has_locked_note = !locked_uuid.empty();
		}
		g_polling.store(false);
		if (g_exiting.load() || !PostMessageW(g_wnd, WM_POLL_DONE, 0, reinterpret_cast<LPARAM>(upd)))
			delete upd;
	}).detach();
}

// Runs a request off the UI thread; failures come back as a notification.
void Request(std::string req, const std::wstring &what)
{
	std::thread([req = std::move(req), what]() mutable {
		std::vector<std::string> lines;
		std::string err;
		int rc = ControlRequest(g_pipe, req, lines, err);
		if (!req.empty())
		{
			SecureZeroMemory(&req[0], req.size());
			req.clear();
		}
		std::wstring *msg = rc ? new std::wstring(what + L": " + W(err)) : nullptr;
		if (!PostMessageW(g_wnd, WM_ACTION_DONE, 0, reinterpret_cast<LPARAM>(msg)))
			delete msg;
	}).detach();
}

// winapfs.exe with administrator rights (one UAC prompt); true if it ran and succeeded
bool RunElevated(const std::wstring &args, HWND owner)
{
	std::wstring exe = WinapfsExe();
	SHELLEXECUTEINFOW sei = { sizeof(sei) };
	sei.fMask = SEE_MASK_NOCLOSEPROCESS;
	sei.hwnd = owner;
	sei.lpVerb = L"runas";
	sei.lpFile = exe.c_str();
	sei.lpParameters = args.c_str();
	sei.nShow = SW_HIDE;
	if (!ShellExecuteExW(&sei) || !sei.hProcess)
		return false;
	WaitForSingleObject(sei.hProcess, 120000);
	DWORD code = 1;
	GetExitCodeProcess(sei.hProcess, &code);
	CloseHandle(sei.hProcess);
	return code == 0;
}

bool ServiceInstalled()
{
	SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
	SC_HANDLE svc = scm ? OpenServiceW(scm, L"winapfs", SERVICE_QUERY_STATUS) : nullptr;
	bool ok = svc != nullptr;
	if (svc)
		CloseServiceHandle(svc);
	if (scm)
		CloseServiceHandle(scm);
	return ok;
}

// ---------------------------------------------------------------- unlock dialog

struct UnlockData
{
	std::wstring name;
	std::wstring password;
};

INT_PTR CALLBACK UnlockProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
	case WM_INITDIALOG:
	{
		auto *d = reinterpret_cast<UnlockData *>(lp);
		SetWindowLongPtrW(dlg, DWLP_USER, lp);
		SetDlgItemTextW(dlg, IDC_UNLOCK_TEXT, (L"\"" + d->name + L"\" is encrypted. Enter its password or recovery key:").c_str());
		SetDlgItemTextW(dlg, IDC_HINT, L"The password is sent to the winapfs service on this PC only.");
		static HICON small_icon = nullptr, big_icon = nullptr;   // loaded once, kept
		if (!small_icon)
		{
			LoadIconMetric(g_inst, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &small_icon);
			LoadIconMetric(g_inst, MAKEINTRESOURCEW(IDI_APP), LIM_LARGE, &big_icon);
		}
		SendMessageW(dlg, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));
		SendMessageW(dlg, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big_icon));
		SetForegroundWindow(dlg);
		SetFocus(GetDlgItem(dlg, IDC_PASSWORD));
		return FALSE;
	}
	case WM_COMMAND:
		if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)
		{
			if (LOWORD(wp) == IDOK)
			{
				auto *d = reinterpret_cast<UnlockData *>(GetWindowLongPtrW(dlg, DWLP_USER));
				wchar_t buf[512];
				GetDlgItemTextW(dlg, IDC_PASSWORD, buf, 512);
				d->password = buf;
				SecureZeroMemory(buf, sizeof(buf));
			}
			EndDialog(dlg, LOWORD(wp));
			return TRUE;
		}
		break;
	}
	return FALSE;
}

void ActionUnlock(VolumeStatus v)
{
	UnlockData d;
	d.name = W(v.name);
	if (DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_UNLOCK), DialogOwner(), UnlockProc, reinterpret_cast<LPARAM>(&d)) != IDOK || d.password.empty())
		return;
	std::string pw = U(d.password);
	SecureZeroMemory(&d.password[0], d.password.size() * sizeof(wchar_t));
	d.password.clear();
	std::string req = "mount\t" + v.uuid + "\t" + pw;
	if (!pw.empty())
	{
		SecureZeroMemory(&pw[0], pw.size());
		pw.clear();
	}
	Request(std::move(req), L"Cannot unlock " + d.name);
}

void ActionOpen(const VolumeStatus &v)
{
	ShellExecuteW(nullptr, L"open", (W(v.letter) + L"\\").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void ActionMount(const VolumeStatus &v)
{
	Request("mount\t" + v.uuid, L"Cannot mount " + W(v.name));
}

void ActionUnmount(const VolumeStatus &v)
{
	Request("unmount\t" + v.uuid, L"Cannot unmount " + W(v.name));
}

void ActionEject(const VolumeStatus &v)
{
	Request("eject\t" + v.uuid, L"Cannot eject " + W(v.name));
	Notify(L"Ejecting the disk", L"Wait for the next message before unplugging it.");
}

void ActionToggleReadWrite(VolumeStatus v)
{
	std::wstring name = W(v.name);
	if (v.mode != "rw" &&
		MessageBoxW(DialogOwner(), (L"Mount \"" + name + L"\" read-write?\n\nWriting to APFS is experimental: keep a backup. "
			L"Always unmount or eject before unplugging the disk.").c_str(), L"winapfs", MB_OKCANCEL | MB_ICONWARNING) != IDOK)
		return;
	Request("set\t" + v.uuid + "\tReadWrite\t" + (v.mode == "rw" ? "0" : "1"), L"Cannot change " + name);
}

void ActionToggleAutoMount(const VolumeStatus &v)
{
	Request("set\t" + v.uuid + "\tDisabled\t" + (v.state == "disabled" ? "0" : "1"), L"Cannot change " + W(v.name));
}

void ActionToggleAllowWrites(bool allow)
{
	HWND owner = DialogOwner();
	if (!allow &&
		MessageBoxW(owner, L"Allow winapfs to write to APFS disks?\n\nWrite support is experimental and still being validated "
			L"against macOS. Keep a backup of anything on the disk.\n\nYou will then choose \"Read-write\" per volume. "
			L"Windows asks for administrator rights once.", L"winapfs", MB_OKCANCEL | MB_ICONWARNING) != IDOK)
		return;
	std::thread([allow, owner] {
		bool ok = RunElevated(allow ? L"service config AllowDiskWrites 0" : L"service config AllowDiskWrites 1", owner);
		std::wstring *msg = ok ? nullptr : new std::wstring(L"The setting was not changed");
		if (!PostMessageW(g_wnd, WM_ACTION_DONE, 0, reinterpret_cast<LPARAM>(msg)))
			delete msg;
	}).detach();
}

void ActionRescan()
{
	Request("rescan", L"Rescan failed");
}

void ActionStartService()
{
	HWND owner = DialogOwner();
	std::thread([owner] {
		bool ok = RunElevated(L"service start", owner);
		std::wstring *msg = ok ? nullptr : new std::wstring(L"The winapfs service could not be started");
		if (!PostMessageW(g_wnd, WM_ACTION_DONE, 0, reinterpret_cast<LPARAM>(msg)))
			delete msg;
	}).detach();
}

void ActionInstallService()
{
	HWND owner = DialogOwner();
	std::thread([owner] {
		bool ok = RunElevated(L"service install", owner);
		std::wstring *msg = ok ? nullptr : new std::wstring(L"The winapfs service could not be installed");
		if (!PostMessageW(g_wnd, WM_ACTION_DONE, 0, reinterpret_cast<LPARAM>(msg)))
			delete msg;
	}).detach();
}

void ActionOpenReleasePage()
{
	std::string url = !g_update.release_url.empty() ? g_update.release_url : "https://github.com/FOLKGG/winapfs/releases";
	ShellExecuteW(nullptr, L"open", W(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void ActionCheckUpdates(bool interactive)
{
	if (g_exiting.load() || g_checking_update.exchange(true))
		return;
	SyncDashboardState(g_last_dash);
	std::thread([interactive] {
		auto *res = new UpdateResultMsg;
		res->info = CheckForUpdates(WINAPFS_VERSION_STR);
		res->interactive = interactive;
		g_checking_update.store(false);
		if (g_exiting.load() || !PostMessageW(g_wnd, WM_UPDATE_RESULT, 0, reinterpret_cast<LPARAM>(res)))
			delete res;
	}).detach();
}

void ActionInstallUpdate()
{
	if (g_exiting.load() || g_downloading_update.load())
		return;
	if (g_update.download_url.empty())
	{
		ActionOpenReleasePage();
		return;
	}
	if (g_downloading_update.exchange(true))
		return;
	std::string url = g_update.download_url;
	std::string asset = g_update.asset_name;
	SyncDashboardState(g_last_dash);
	std::thread([url, asset] {
		auto *dl = new UpdateDownloadMsg;
		dl->ok = DownloadUpdateInstaller(url, asset, dl->path, dl->err);
		g_downloading_update.store(false);
		if (g_exiting.load() || !PostMessageW(g_wnd, WM_UPDATE_DOWNLOAD_DONE, 0, reinterpret_cast<LPARAM>(dl)))
			delete dl;
	}).detach();
}

// ---------------------------------------------------------------- menu

void About()
{
	std::wstring text =
		L"winapfs " WINAPFS_VERSION_W L"\nAPFS (Mac) disks on Windows\n\n"
		L"Free software under the GNU General Public License, version 2.\n"
		L"Includes linux-apfs-rw, apfs-fuse and apfsprogs (GPL), LZFSE, zlib, bzip2.\n\n"
		L"Uses WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos\n"
		L"https://github.com/winfsp/winfsp\n\n"
		L"Writing to APFS disks is experimental: keep a backup.";
	MSGBOXPARAMSW mb = { sizeof(mb) };
	mb.hwndOwner = DialogOwner();
	mb.hInstance = g_inst;
	mb.lpszText = text.c_str();
	mb.lpszCaption = L"About winapfs";
	mb.dwStyle = MB_OK | MB_USERICON;
	mb.lpszIcon = MAKEINTRESOURCEW(IDI_APP);
	MessageBoxIndirectW(&mb);
}

void ShowMenu()
{
	StartPollAsync();
	std::vector<VolumeStatus> vols;
	bool allow, up, installed;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		vols = g_vols;
		allow = g_allow_writes;
		up = g_service_up;
		installed = g_service_installed;
	}

	HMENU m = CreatePopupMenu();
	AppendMenuW(m, MF_STRING, ID_OPEN_DASHBOARD, L"Open winapfs Dashboard");
	SetMenuDefaultItem(m, ID_OPEN_DASHBOARD, FALSE);
	AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(m, MF_STRING | MF_GRAYED, 0, up ? L"winapfs \x2014 APFS disks" : L"winapfs: the service is not running");
	if (up && vols.empty())
		AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"No APFS disks connected");
	for (size_t i = 0; i < vols.size() && i < 90; i++)
	{
		const VolumeStatus &v = vols[i];
		UINT base = ID_VOLUME_BASE + static_cast<UINT>(i) * 10;
		HMENU sub = CreatePopupMenu();
		bool mounted = v.state == "mounted";
		if (mounted)
		{
			AppendMenuW(sub, MF_STRING, base + VA_OPEN, (L"Open " + W(v.letter)).c_str());
			SetMenuDefaultItem(sub, base + VA_OPEN, FALSE);
			AppendMenuW(sub, MF_STRING, base + VA_UNMOUNT, L"Unmount");
		}
		else if (v.state == "locked")
			AppendMenuW(sub, MF_STRING, base + VA_UNLOCK, L"Unlock\x2026");
		else
			AppendMenuW(sub, MF_STRING, base + VA_MOUNT, L"Mount");
		if (IsPhysical(v))
			AppendMenuW(sub, MF_STRING, base + VA_EJECT, L"Eject disk (safely remove)");
		AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
		bool rw_possible = !IsPhysical(v) || allow;
		AppendMenuW(sub, MF_STRING | (v.mode == "rw" ? MF_CHECKED : 0) | (rw_possible && v.state != "locked" ? 0 : MF_GRAYED), base + VA_READWRITE,
			rw_possible ? L"Read-write" : L"Read-write (allow writing to disks first)");
		AppendMenuW(sub, MF_STRING | (v.state != "disabled" ? MF_CHECKED : 0), base + VA_AUTOMOUNT, L"Mount automatically");
		if (!v.note.empty())
		{
			AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
			std::wstring note = W(v.note);
			if (note.size() > 90)
				note = note.substr(0, 87) + L"...";
			AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, note.c_str());
		}
		std::wstring label = W(v.name) + (v.letter != "-" ? L" (" + W(v.letter) + L")" : L"") + L"\t" + StateText(v);
		AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), label.c_str());
	}
	AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
	if (up)
	{
		AppendMenuW(m, MF_STRING | (allow ? MF_CHECKED : 0), ID_ALLOW_WRITES, L"Allow writing to disks (experimental)\x2026");
		AppendMenuW(m, MF_STRING, ID_RESCAN, L"Rescan disks");
	}
	else if (installed)
		AppendMenuW(m, MF_STRING, ID_SERVICE_START, L"Start the winapfs service");
	else
		AppendMenuW(m, MF_STRING, ID_SERVICE_INSTALL, L"Install the winapfs service\x2026");
	AppendMenuW(m, MF_STRING, ID_CHECK_UPDATES, L"Check for updates\x2026");
	AppendMenuW(m, MF_STRING, ID_ABOUT, L"About winapfs");
	AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");

	POINT pt;
	GetCursorPos(&pt);
	SetForegroundWindow(g_wnd);
	UINT cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, g_wnd, nullptr);
	PostMessageW(g_wnd, WM_NULL, 0, 0);
	DestroyMenu(m);

	if (cmd >= ID_VOLUME_BASE)
	{
		size_t i = (cmd - ID_VOLUME_BASE) / 10;
		UINT action = (cmd - ID_VOLUME_BASE) % 10;
		if (i >= vols.size())
			return;
		const VolumeStatus &v = vols[i];
		switch (action)
		{
		case VA_OPEN: ActionOpen(v); break;
		case VA_MOUNT: ActionMount(v); break;
		case VA_UNMOUNT: ActionUnmount(v); break;
		case VA_UNLOCK: ActionUnlock(v); break;
		case VA_EJECT: ActionEject(v); break;
		case VA_READWRITE: ActionToggleReadWrite(v); break;
		case VA_AUTOMOUNT: ActionToggleAutoMount(v); break;
		}
		return;
	}
	switch (cmd)
	{
	case ID_OPEN_DASHBOARD:
		DashboardShow();
		StartPollAsync();
		break;
	case ID_ALLOW_WRITES:
		ActionToggleAllowWrites(allow);
		break;
	case ID_RESCAN:
		ActionRescan();
		break;
	case ID_SERVICE_START:
		ActionStartService();
		break;
	case ID_SERVICE_INSTALL:
		ActionInstallService();
		break;
	case ID_CHECK_UPDATES:
		ActionCheckUpdates(true);
		break;
	case ID_ABOUT:
		About();
		break;
	case ID_EXIT:
		DestroyWindow(g_wnd);
		break;
	}
}

// ---------------------------------------------------------------- window

void AddIcon()
{
	Shell_NotifyIconW(NIM_ADD, &g_nid);
	g_nid.uVersion = NOTIFYICON_VERSION_4;
	Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}

LRESULT CALLBACK WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == g_taskbar_created && g_taskbar_created)
	{
		AddIcon();   // Explorer restarted
		return 0;
	}
	if (msg == g_show_dashboard_msg && g_show_dashboard_msg)
	{
		DashboardShow();
		StartPollAsync();
		return 0;
	}
	switch (msg)
	{
	case WM_TRAY:
		switch (LOWORD(lp))
		{
		case WM_CONTEXTMENU:
			ShowMenu();
			break;
		case NIN_SELECT:
		case NIN_KEYSELECT:
			DashboardShow();
			StartPollAsync();
			break;
		case NIN_BALLOONUSERCLICK:
		{
			if (g_last_balloon_is_update)
			{
				DashboardShow();
				StartPollAsync();
				break;
			}
			VolumeStatus v;
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				for (const auto &x : g_vols)
					if (x.uuid == g_balloon_uuid && x.state == "locked")
						v = x;
			}
			if (!v.uuid.empty())
				ActionUnlock(v);
			break;
		}
		}
		return 0;
	case WM_TIMER:
		if (wp == TIMER_POLL)
			StartPollAsync();
		else if (wp == TIMER_STARTUP_UPDATE)
		{
			KillTimer(wnd, TIMER_STARTUP_UPDATE);
			ActionCheckUpdates(false);
		}
		return 0;
	case WM_POLL_DONE:
	{
		auto *upd = reinterpret_cast<PollUiUpdate *>(lp);
		if (upd)
		{
			SetTip(upd->tip);
			if (!upd->notify_title.empty())
			{
				if (upd->has_locked_note)
					g_last_balloon_is_update = false;
				Notify(upd->notify_title, upd->notify_text);
			}
			SyncDashboardState(upd->dash);
			delete upd;
		}
		return 0;
	}
	case WM_UPDATE_RESULT:
	{
		auto *res = reinterpret_cast<UpdateResultMsg *>(lp);
		if (res)
		{
			if (res->info.ok)
			{
				g_update = res->info;
				g_update_status_text = res->info.update_available ? "" : "Up to date";
				SyncDashboardState(g_last_dash);
				if (res->info.update_available)
				{
					std::wstring ver = W(res->info.latest_version);
					g_last_balloon_is_update = true;
					Notify(L"winapfs update available", L"winapfs v" + ver + L" is ready to install.");
					if (res->interactive && (!DashboardHwnd() || !IsWindowVisible(DashboardHwnd())))
					{
						if (MessageBoxW(DialogOwner(),
							(L"winapfs v" + ver + L" is available (you have v" WINAPFS_VERSION_W L").\n\nDownload and install it now?").c_str(),
							L"winapfs Update", MB_YESNO | MB_ICONINFORMATION) == IDYES)
						{
							ActionInstallUpdate();
						}
					}
				}
				else if (res->interactive)
				{
					MessageBoxW(DialogOwner(), L"winapfs v" WINAPFS_VERSION_W L" is up to date.", L"winapfs Update", MB_OK | MB_ICONINFORMATION);
				}
			}
			else
			{
				g_update_status_text = "";
				SyncDashboardState(g_last_dash);
				if (res->interactive)
					MessageBoxW(DialogOwner(), (L"Could not check for updates:\n\n" + W(res->info.error)).c_str(), L"winapfs Update", MB_OK | MB_ICONWARNING);
			}
			delete res;
		}
		return 0;
	}
	case WM_UPDATE_DOWNLOAD_DONE:
	{
		auto *dl = reinterpret_cast<UpdateDownloadMsg *>(lp);
		if (dl)
		{
			SyncDashboardState(g_last_dash);
			if (dl->ok)
			{
				HINSTANCE r = ShellExecuteW(DialogOwner(), L"open", dl->path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
				if (reinterpret_cast<INT_PTR>(r) <= 32)
					Notify(L"winapfs update", L"Downloaded to " + dl->path + L" (could not launch installer automatically).", NIIF_WARNING);
			}
			else
			{
				Notify(L"winapfs update failed", W(dl->err), NIIF_WARNING);
			}
			delete dl;
		}
		return 0;
	}
	case WM_ACTION_DONE:
	{
		auto *m = reinterpret_cast<std::wstring *>(lp);
		if (m)
		{
			Notify(L"winapfs", *m, NIIF_WARNING);
			delete m;
		}
		StartPollAsync();
		return 0;
	}
	case WM_DESTROY:
		g_exiting.store(true);
		DashboardDestroy();
		KillTimer(wnd, TIMER_POLL);
		KillTimer(wnd, TIMER_STARTUP_UPDATE);
		Shell_NotifyIconW(NIM_DELETE, &g_nid);
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(wnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int)
{
	g_inst = inst;
	g_show_dashboard_msg = RegisterWindowMessageW(L"winapfs-show-dashboard");
	HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\winapfs-tray");
	if (GetLastError() == ERROR_ALREADY_EXISTS)
	{
		HWND existing = FindWindowExW(HWND_MESSAGE, nullptr, L"winapfs-tray", L"winapfs");
		if (existing && g_show_dashboard_msg)
		{
			DWORD pid = 0;
			GetWindowThreadProcessId(existing, &pid);
			if (pid)
				AllowSetForegroundWindow(pid);
			PostMessageW(existing, g_show_dashboard_msg, 0, 0);
		}
		CloseHandle(single);
		return 0;   // already running for this user
	}

	g_pipe = DefaultControlPipe();
	bool open_dashboard = false;
	int argc = 0;
	wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	for (int i = 1; argv && i < argc; i++)
	{
		if (wcscmp(argv[i], L"--pipe") == 0 && i + 1 < argc)
			g_pipe = argv[++i];
		else if (wcscmp(argv[i], L"--dashboard") == 0)
			open_dashboard = true;
	}
	if (argv)
		LocalFree(argv);

	INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
	InitCommonControlsEx(&icc);

	Gdiplus::GdiplusStartupInput gdip_in;
	ULONG_PTR gdip_token = 0;
	Gdiplus::GdiplusStartup(&gdip_token, &gdip_in, nullptr);

	WNDCLASSEXW wc = { sizeof(wc) };
	wc.lpfnWndProc = WndProc;
	wc.hInstance = inst;
	wc.lpszClassName = L"winapfs-tray";
	RegisterClassExW(&wc);
	g_wnd = CreateWindowExW(0, wc.lpszClassName, L"winapfs", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
	g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
	ChangeWindowMessageFilterEx(g_wnd, g_taskbar_created, MSGFLT_ALLOW, nullptr);
	if (g_show_dashboard_msg)
		ChangeWindowMessageFilterEx(g_wnd, g_show_dashboard_msg, MSGFLT_ALLOW, nullptr);

	DashboardCallbacks dash_cbs;
	dash_cbs.on_open = ActionOpen;
	dash_cbs.on_mount = ActionMount;
	dash_cbs.on_unmount = ActionUnmount;
	dash_cbs.on_unlock = ActionUnlock;
	dash_cbs.on_eject = ActionEject;
	dash_cbs.on_toggle_rw = ActionToggleReadWrite;
	dash_cbs.on_toggle_auto = ActionToggleAutoMount;
	dash_cbs.on_toggle_allow_writes = ActionToggleAllowWrites;
	dash_cbs.on_rescan = ActionRescan;
	dash_cbs.on_start_service = ActionStartService;
	dash_cbs.on_install_service = ActionInstallService;
	dash_cbs.on_about = About;
	dash_cbs.on_check_updates = [] { ActionCheckUpdates(true); };
	dash_cbs.on_install_update = ActionInstallUpdate;
	dash_cbs.on_open_release_page = ActionOpenReleasePage;
	DashboardInit(inst, g_wnd, std::move(dash_cbs));

	// the logo, at the notification area's size for this DPI
	HICON tray_icon = nullptr;
	if (FAILED(LoadIconMetric(inst, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &tray_icon)))
		tray_icon = LoadIconW(nullptr, IDI_APPLICATION);
	g_nid.cbSize = sizeof(g_nid);
	g_nid.hWnd = g_wnd;
	g_nid.uID = 1;
	g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
	g_nid.uCallbackMessage = WM_TRAY;
	g_nid.hIcon = tray_icon;
	wcscpy_s(g_nid.szTip, L"winapfs");
	AddIcon();

	if (open_dashboard)
		DashboardShow();
	StartPollAsync();
	SetTimer(g_wnd, TIMER_POLL, kPollMs, nullptr);
	SetTimer(g_wnd, TIMER_STARTUP_UPDATE, 10000, nullptr);

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0) > 0)
	{
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	if (tray_icon)
		DestroyIcon(tray_icon);
	Gdiplus::GdiplusShutdown(gdip_token);
	CloseHandle(single);
	return 0;
}
