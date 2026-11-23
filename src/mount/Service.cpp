// winapfs - APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The auto-mount service. Runs as LocalSystem, so its drive letters are
// global (Mount Manager) and visible to every user:
//   - mounts every APFS volume on every disk at start and when a disk arrives
//     (disk interface notifications), at a drive letter remembered per volume
//     UUID;
//   - unmounts a disk's volumes when Windows asks to remove it ("Safely
//     remove", eject: handle notifications on each disk), when it is gone,
//     and at shutdown (pre-shutdown, so read-write volumes unmount cleanly);
//   - read-only unless configured (and allowed) read-write; refused writes
//     fall back to read-only;
//   - serves the control pipe (Control.h) for `winapfs ctl` and the tray app:
//     status of every volume seen, mount (with a password for encrypted
//     volumes), unmount, eject, settings.
//
// Settings: HKLM\SOFTWARE\winapfs (tests: WINAPFS_CONFIG_HKCU=1 uses
// HKCU\Software\winapfs-test)
//   AllowDiskWrites  DWORD  1: read-write volumes on physical disks (W5 opt-in;
//                           only `winapfs service config`, i.e. administrators)
//   ReadWrite        DWORD  default for volumes without their own setting
//   HideDotfiles     DWORD  default 1: names starting with . are hidden (as on macOS)
//   Volumes\<UUID>   DriveLetter (REG_SZ "X:"), ReadWrite, HideDotfiles,
//                    Disabled (DWORD), Name (REG_SZ, informational)

#include "Service.h"

#include "ApfsFs.h"
#include "Control.h"
#include "Mount.h"
#include "messages.h"

#include <Windows.h>
#include <cfgmgr32.h>
#include <dbt.h>
#include <initguid.h>
#include <sddl.h>
#include <setupapi.h>
#include <winioctl.h>

#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace winapfs;

namespace {

void SecureClear(std::string &s)
{
	if (!s.empty())
	{
		SecureZeroMemory(&s[0], s.size());
		s.clear();
	}
}

struct SecretStringGuard
{
	std::string &s;
	~SecretStringGuard() { SecureClear(s); }
};

const wchar_t *kServiceName = L"winapfs";
const wchar_t *kDisplayName = L"winapfs (APFS for Windows)";
const wchar_t *kDescription = L"Mounts APFS (Mac) disks at drive letters when they are connected.";

// ---------------------------------------------------------------- settings

struct Config
{
	HKEY root = HKEY_LOCAL_MACHINE;
	std::wstring base = L"SOFTWARE\\winapfs";

	Config()
	{
		wchar_t v[8];
		if (GetEnvironmentVariableW(L"WINAPFS_CONFIG_HKCU", v, 8))
		{
			root = HKEY_CURRENT_USER;
			base = L"Software\\winapfs-test";
		}
	}

	std::wstring Key(const std::wstring &sub) const { return sub.empty() ? base : base + L"\\" + sub; }

	DWORD Dword(const std::wstring &sub, const wchar_t *name, DWORD def) const
	{
		DWORD v = 0, size = sizeof(v);
		if (RegGetValueW(root, Key(sub).c_str(), name, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS)
			return v;
		return def;
	}

	std::wstring String(const std::wstring &sub, const wchar_t *name) const
	{
		wchar_t buf[256];
		DWORD size = sizeof(buf);
		if (RegGetValueW(root, Key(sub).c_str(), name, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
			return buf;
		return L"";
	}

	bool Set(const std::wstring &sub, const wchar_t *name, DWORD type, const void *data, DWORD size) const
	{
		HKEY k;
		if (RegCreateKeyExW(root, Key(sub).c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
			return false;
		bool ok = RegSetValueExW(k, name, 0, type, static_cast<const BYTE *>(data), size) == ERROR_SUCCESS;
		RegCloseKey(k);
		return ok;
	}

	bool SetString(const std::wstring &sub, const wchar_t *name, const std::wstring &value) const
	{
		return Set(sub, name, REG_SZ, value.c_str(), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
	}

	bool SetDword(const std::wstring &sub, const wchar_t *name, DWORD value) const
	{
		return Set(sub, name, REG_DWORD, &value, sizeof(value));
	}
};

std::wstring VolKey(const std::string &uuid)
{
	return L"Volumes\\" + Utf8ToWide(uuid);
}

// ---------------------------------------------------------------- logging

HANDLE g_event_source = nullptr;

void EventLogSink(int level, const std::string &msg)
{
	std::wstring w = Utf8ToWide(msg);
	const wchar_t *strs[] = { w.c_str() };
	WORD type = level == 0 ? EVENTLOG_ERROR_TYPE : level == 1 ? EVENTLOG_WARNING_TYPE : EVENTLOG_INFORMATION_TYPE;
	if (g_event_source)
		ReportEventW(g_event_source, type, 0, MSG_TEXT, nullptr, 1, 0, strs, nullptr);
	OutputDebugStringW((L"winapfs: " + w + L"\n").c_str());
}

// ---------------------------------------------------------------- drive letters

bool LetterFree(wchar_t c)
{
	DWORD used = GetLogicalDrives();
	wchar_t root[] = { c, L':', 0 };
	wchar_t target[MAX_PATH];
	return !(used & (1u << (c - L'A'))) && !QueryDosDeviceW(root, target, MAX_PATH);
}

wchar_t PickLetter(const std::wstring &remembered)
{
	if (remembered.size() == 2 && iswalpha(remembered[0]) && remembered[1] == L':')
	{
		wchar_t c = static_cast<wchar_t>(towupper(remembered[0]));
		if (c >= L'D' && LetterFree(c))
			return c;
	}
	for (wchar_t c = L'Z'; c >= L'D'; c--)
		if (LetterFree(c))
			return c;
	return 0;
}

// ---------------------------------------------------------------- device eject

// Asks Windows to eject the device behind \\.\PhysicalDriveN (what "Safely
// Remove Hardware" does). Our handles must be closed first.
bool EjectDisk(const std::string &drive, std::string &err)
{
	const char *prefix = "\\\\.\\PhysicalDrive";
	if (_strnicmp(drive.c_str(), prefix, strlen(prefix)) != 0)
	{
		err = "only physical disks can be ejected";
		return false;
	}
	DWORD number = strtoul(drive.c_str() + strlen(prefix), nullptr, 10);
	HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_DISK, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (set == INVALID_HANDLE_VALUE)
	{
		err = "cannot list disks";
		return false;
	}
	DEVINST inst = 0;
	SP_DEVICE_INTERFACE_DATA ifd = { sizeof(ifd) };
	for (DWORD i = 0; !inst && SetupDiEnumDeviceInterfaces(set, nullptr, &GUID_DEVINTERFACE_DISK, i, &ifd); i++)
	{
		alignas(8) BYTE buf[1024];
		auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buf);
		detail->cbSize = sizeof(*detail);
		SP_DEVINFO_DATA dev = { sizeof(dev) };
		if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, sizeof(buf), nullptr, &dev))
			continue;
		HANDLE h = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			continue;
		STORAGE_DEVICE_NUMBER sdn = {};
		DWORD got = 0;
		if (DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &sdn, sizeof(sdn), &got, nullptr) && sdn.DeviceNumber == number)
			inst = dev.DevInst;
		CloseHandle(h);
	}
	SetupDiDestroyDeviceInfoList(set);
	if (!inst)
	{
		err = "the disk is gone";
		return false;
	}
	// the disk itself, else its parent (a USB storage device, a card reader slot)
	for (int level = 0; level < 2 && inst; level++)
	{
		PNP_VETO_TYPE veto = PNP_VetoTypeUnknown;
		wchar_t name[MAX_PATH] = {};
		CONFIGRET cr = CM_Request_Device_EjectW(inst, &veto, name, MAX_PATH, 0);
		if (cr == CR_SUCCESS && veto == PNP_VetoTypeUnknown)
			return true;
		if (veto != PNP_VetoTypeUnknown)
			err = "Windows refused to eject it (veto " + std::to_string(static_cast<int>(veto)) + (name[0] ? ", " + WideToUtf8(name) : std::string()) + ")";
		else
			err = "eject failed (CONFIGRET " + std::to_string(cr) + ")";
		DEVINST parent = 0;
		if (CM_Get_Parent(&parent, inst, 0) != CR_SUCCESS)
			break;
		inst = parent;
	}
	return false;
}

// ---------------------------------------------------------------- auto-mounter

class AutoMounter
{
public:
	AutoMounter(const Config &cfg, std::vector<std::string> images, SERVICE_STATUS_HANDLE status)
		: m_cfg(cfg), m_images(std::move(images)), m_status(status) {}

	~AutoMounter() { UnmountAll(); }

	// Mounts what is new, unmounts what is gone.
	void Scan()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		std::vector<std::string> sources = m_images;
		if (sources.empty())
			for (const auto &d : EnumeratePhysicalDrives())
				if (!d.access_denied && !d.apfs.empty())
					sources.push_back(d.path);

		// disks that disappeared (an ejected disk may come back, and is new then)
		for (size_t i = 0; i < m_disks.size();)
		{
			bool present = false;
			for (const auto &s : sources)
				present = present || s == m_disks[i]->path;
			if (present)
				i++;
			else
			{
				Log(2, "%s is gone", m_disks[i]->path.c_str());
				Drop(i);
			}
		}
		for (auto it = m_ejected.begin(); it != m_ejected.end();)
		{
			bool present = false;
			for (const auto &s : sources)
				present = present || s == *it;
			it = present ? std::next(it) : m_ejected.erase(it);
		}
		for (const auto &src : sources)
			if (!m_ejected.count(src))
				ScanDisk(src);
	}

	// A disk Windows wants to remove: release everything on it (query-remove).
	bool ReleaseByNotifyHandle(HANDLE h)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		for (size_t i = 0; i < m_disks.size(); i++)
			if (m_disks[i]->notify_handle == h)
			{
				Log(2, "%s: removal requested, unmounting", m_disks[i]->path.c_str());
				Drop(i);
				return true;
			}
		return false;
	}

	void UnmountAll()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		while (!m_disks.empty())
			Drop(m_disks.size() - 1);
	}

	// name -> mount point, for --run in debug mode
	std::vector<std::pair<std::string, std::wstring>> Mounts()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		std::vector<std::pair<std::string, std::wstring>> out;
		for (const auto &d : m_disks)
			for (const auto &m : d->mounts)
				out.emplace_back(m.name, m.fs->MountPoint());
		return out;
	}

	// ---- control requests (Control.h); each returns "" or an error

	std::vector<std::string> List()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		std::vector<std::string> out;
		for (const auto &d : m_disks)
			for (const auto &kv : d->vols)
			{
				const VolStatus &s = kv.second;
				std::string letter = "-", mode = "-";
				for (const auto &m : d->mounts)
					if (m.uuid == kv.first)
					{
						letter = WideToUtf8(m.fs->MountPoint());
						mode = m.fs->Vol().Writable() ? "rw" : "ro";
					}
				out.push_back("vol\t" + kv.first + "\t" + Clean(s.name) + "\t" + s.state + "\t" + letter + "\t" + mode + "\t" + d->path + "\t" + Clean(s.note));
			}
		out.push_back("cfg\tAllowDiskWrites\t" + std::to_string(m_cfg.Dword(L"", L"AllowDiskWrites", 0)));
		return out;
	}

	std::string Mount(const std::string &uuid, const std::string &password)
	{
		bool siblings = false;
		std::string r = MountLocked(uuid, password, siblings);
		if (siblings)
			Scan();   // remount the volumes stopped to make the device writable
		return r;
	}

	std::string MountLocked(const std::string &uuid, const std::string &password, bool &siblings)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		Disk *d = nullptr;
		VolStatus *s = Find(uuid, &d);
		if (!s)
			return "no such volume";
		if (IsMounted(*d, uuid))
			return "";
		// a read-write volume can't share a device its read-only siblings opened read-only
		if (WantsReadWrite(uuid))
			for (const auto &kv : d->vols)
				if (kv.first != uuid && kv.second.part == s->part && IsMounted(*d, kv.first))
					siblings = true;
		if (siblings)
			StopPartition(*d, s->part);
		std::string err;
		std::unique_ptr<Container> c = Container::Open(d->path, err, s->part);
		if (!c)
			return err;
		for (const auto &vi : c->Volumes())
			if (vi.uuid == uuid)
			{
				if (vi.encrypted && password.empty())
					return "the volume is encrypted: a password (or recovery key) is needed";
				if (!TryMount(*d, *c, vi, password, true))
					return s->note.empty() ? "cannot mount it" : s->note;
				d->containers.push_back(std::move(c));
				if (d->notify == nullptr && m_status && d->path.compare(0, 4, "\\\\.\\") == 0)
					WatchRemoval(*d);
				return "";
			}
		return "the volume is gone";
	}

	std::string Unmount(const std::string &uuid)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		Disk *d = nullptr;
		VolStatus *s = Find(uuid, &d);
		if (!s)
			return "no such volume";
		Stop(*d, uuid);
		s->state = "unmounted";
		s->note = "unmounted; mount it again from the menu, or reconnect the disk";
		return "";
	}

	std::string Eject(const std::string &uuid)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		Disk *d = nullptr;
		if (!Find(uuid, &d))
			return "no such volume";
		std::string path = d->path;
		for (size_t i = 0; i < m_disks.size(); i++)
			if (m_disks[i].get() == d)
			{
				Drop(i);   // unmounts everything on the disk and closes our handles
				break;
			}
		m_ejected.insert(path);   // no remounting while it is still there
		if (path.compare(0, 4, "\\\\.\\") != 0)
			return "";             // an image (debug mode): unmounting is all
		std::string err;
		if (!EjectDisk(path, err))
			return "unmounted, but " + err;
		Log(2, "%s ejected", path.c_str());
		return "";
	}

	std::string Set(const std::string &uuid, const std::string &key, const std::string &value)
	{
		std::wstring wkey = Utf8ToWide(key);
		if (key != "ReadWrite" && key != "Disabled" && key != "HideDotfiles")
			return "unknown setting " + key;
		if (value != "0" && value != "1")
			return "the value must be 0 or 1";
		bool remount = false;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			Disk *d = nullptr;
			VolStatus *s = Find(uuid, &d);
			if (!s)
				return "no such volume";
			if (!m_cfg.SetDword(VolKey(uuid), wkey.c_str(), value == "1"))
				return "cannot store the setting";
			bool mounted = IsMounted(*d, uuid);
			if (key == "Disabled" && value == "1")
			{
				Stop(*d, uuid);
				s->state = "disabled";
				s->note = "not mounted automatically";
			}
			else if (key == "Disabled")
			{
				s->tried = false;   // mount it at the next scan
				remount = !mounted;
			}
			else if (key == "ReadWrite")
			{
				// the container's device may have to become writable: remount
				// every volume of the partition (rw ones first, see ScanDisk)
				StopPartition(*d, s->part);
				remount = true;
			}
			else if (mounted)
			{
				Stop(*d, uuid);     // applies HideDotfiles
				s->tried = false;
				remount = true;
			}
		}
		if (remount)
			Scan();
		return "";
	}

private:
	struct Mounted
	{
		std::string uuid, name;
		std::unique_ptr<ApfsFs> fs;
	};
	struct VolStatus
	{
		std::string name;
		int part = -1;
		std::string state = "failed";
		std::string note;
		bool tried = false;   // automatic mounting was attempted (not retried)
	};
	struct Disk
	{
		std::string path;
		DiskLocks locks;   // released after the mounts and containers below
		std::vector<std::unique_ptr<Container>> containers;
		std::vector<Mounted> mounts;
		std::map<std::string, VolStatus> vols;   // every volume seen, by UUID
		HANDLE notify_handle = INVALID_HANDLE_VALUE;
		HDEVNOTIFY notify = nullptr;
	};

	static std::string Clean(std::string s)
	{
		for (auto &c : s)
			if (c == '\t' || c == '\n' || c == '\r')
				c = ' ';
		return s;
	}

	VolStatus *Find(const std::string &uuid, Disk **disk)
	{
		for (auto &d : m_disks)
		{
			auto it = d->vols.find(uuid);
			if (it != d->vols.end())
			{
				*disk = d.get();
				return &it->second;
			}
		}
		return nullptr;
	}

	static bool IsMounted(const Disk &d, const std::string &uuid)
	{
		for (const auto &m : d.mounts)
			if (m.uuid == uuid)
				return true;
		return false;
	}

	void Stop(Disk &d, const std::string &uuid)
	{
		for (size_t i = 0; i < d.mounts.size(); i++)
			if (d.mounts[i].uuid == uuid)
			{
				std::wstring mp = d.mounts[i].fs->MountPoint();
				d.mounts[i].fs->Stop();
				Log(2, "unmounted %s (%ls)", d.mounts[i].name.c_str(), mp.c_str());
				d.mounts.erase(d.mounts.begin() + static_cast<ptrdiff_t>(i));
				if (d.mounts.empty())
				{
					d.containers.clear();
					d.locks.clear();
				}
				return;
			}
	}

	// Unmounts every volume of one container (they share its device), to be
	// mounted again by the next scan.
	void StopPartition(Disk &d, int part)
	{
		for (auto &kv : d.vols)
			if (kv.second.part == part)
			{
				Stop(d, kv.first);
				if (kv.second.state == "mounted")
					kv.second.tried = false;
			}
		if (d.mounts.empty())
		{
			d.containers.clear();
			d.locks.clear();
		}
	}

	void Drop(size_t i)
	{
		Disk &d = *m_disks[i];
		for (auto &m : d.mounts)
		{
			std::wstring mp = m.fs->MountPoint();
			m.fs->Stop();
			Log(2, "unmounted %s (%ls)", m.name.c_str(), mp.c_str());
		}
		d.mounts.clear();
		d.containers.clear();
		d.locks.clear();
		if (d.notify)
			UnregisterDeviceNotification(d.notify);
		if (d.notify_handle != INVALID_HANDLE_VALUE)
			CloseHandle(d.notify_handle);
		m_disks.erase(m_disks.begin() + static_cast<ptrdiff_t>(i));
	}

	void ScanDisk(const std::string &src)
	{
		Disk *disk = nullptr;
		for (auto &d : m_disks)
			if (d->path == src)
				disk = d.get();
		if (!disk)
		{
			m_disks.emplace_back(new Disk());
			disk = m_disks.back().get();
			disk->path = src;
		}

		std::string err;
		std::vector<PartitionInfo> parts;
		Container::FindApfsPartitions(src, parts, err);
		std::vector<int> which;
		for (const auto &p : parts)
			which.push_back(p.index);
		if (which.empty())
			which.push_back(-1);   // a bare container image

		std::set<std::string> seen;
		for (int part : which)
		{
			std::unique_ptr<Container> c = Container::Open(src, err, part);
			if (!c)
			{
				Log(1, "%s: %s", src.c_str(), err.c_str());
				continue;
			}
			// read-write volumes first: the engine opens the container's
			// device writable for them, and read-only volumes can share it
			// (not the other way round)
			std::vector<const VolumeInfo *> order;
			for (const auto &vi : c->Volumes())
				if (WantsReadWrite(vi.uuid))
					order.push_back(&vi);
			for (const auto &vi : c->Volumes())
				if (!WantsReadWrite(vi.uuid))
					order.push_back(&vi);
			bool used = false;
			for (const VolumeInfo *vi : order)
			{
				seen.insert(vi->uuid);
				VolStatus &s = disk->vols[vi->uuid];
				s.name = vi->name;
				s.part = part;
				if (IsMounted(*disk, vi->uuid) || s.tried)
					continue;
				s.tried = true;
				if (TryMount(*disk, *c, *vi, "", false))
					used = true;
			}
			if (used)
				disk->containers.push_back(std::move(c));
		}

		// volumes that left the disk (another disk took its number)
		for (auto it = disk->vols.begin(); it != disk->vols.end();)
		{
			if (seen.count(it->first))
			{
				++it;
				continue;
			}
			Stop(*disk, it->first);
			Log(2, "%s left %s", it->second.name.c_str(), src.c_str());
			it = disk->vols.erase(it);
		}

		if (!disk->mounts.empty() && disk->notify == nullptr && m_status && src.compare(0, 4, "\\\\.\\") == 0)
			WatchRemoval(*disk);
	}

	bool WantsReadWrite(const std::string &uuid) const
	{
		return m_cfg.Dword(VolKey(uuid), L"ReadWrite", m_cfg.Dword(L"", L"ReadWrite", 0)) != 0;
	}

	// Mounts one volume; records the outcome in its VolStatus. explicit_request:
	// asked for through the control pipe (mounts disabled volumes too).
	bool TryMount(Disk &disk, Container &c, const VolumeInfo &vi, const std::string &password, bool explicit_request)
	{
		VolStatus &s = disk.vols[vi.uuid];
		s.name = vi.name;
		std::wstring sub = VolKey(vi.uuid);
		if (!explicit_request && m_cfg.Dword(sub, L"Disabled", 0))
		{
			s.state = "disabled";
			s.note = "not mounted automatically";
			return false;
		}
		if (vi.encrypted && password.empty())
		{
			s.state = "locked";
			s.note = "encrypted: unlock it with its password";
			Log(2, "%s: encrypted, waiting for a password", vi.name.c_str());
			return false;
		}
		ClearLoggedProblem();
		bool rw = WantsReadWrite(vi.uuid);
		OpenOptions o;
		o.allow_disk_writes = m_cfg.Dword(L"", L"AllowDiskWrites", 0) != 0;
		o.locks = &disk.locks;
		o.password = password;
		SecretStringGuard pw_guard{ o.password };
		std::unique_ptr<Volume> vol;
		std::string rw_problem;
		if (rw && !vi.encrypted)
		{
			o.rw = true;
			vol = OpenVolume(c, vi, o);
			if (!vol)
			{
				rw_problem = LastLoggedProblem();
				Log(1, "%s: mounting read-only instead", vi.name.c_str());
			}
		}
		if (!vol)
		{
			o.rw = false;
			vol = OpenVolume(c, vi, o);
		}
		if (!vol)
		{
			s.state = vi.encrypted ? "locked" : "failed";
			s.note = LastLoggedProblem();
			return false;
		}

		wchar_t letter = PickLetter(m_cfg.String(sub, L"DriveLetter"));
		if (!letter)
		{
			s.state = "failed";
			s.note = "no free drive letter";
			Log(0, "%s: no free drive letter", vi.name.c_str());
			return false;
		}
		MountOptions mo;
		mo.hide_dotfiles = m_cfg.Dword(sub, L"HideDotfiles", m_cfg.Dword(L"", L"HideDotfiles", 1)) != 0;
		std::unique_ptr<ApfsFs> fs(new ApfsFs(std::move(vol), mo));
		wchar_t mp[] = { letter, L':', 0 };
		NTSTATUS r = fs->Start(mp);
		if (!NT_SUCCESS(r))
		{
			char buf[64];
			snprintf(buf, sizeof(buf), "cannot mount at %lc: (NTSTATUS 0x%08lx)", letter, static_cast<unsigned long>(r));
			s.state = "failed";
			s.note = buf;
			Log(0, "%s: %s", vi.name.c_str(), buf);
			return false;
		}
		m_cfg.SetString(sub, L"DriveLetter", mp);
		m_cfg.SetString(sub, L"Name", Utf8ToWide(vi.name));
		bool writable = fs->Vol().Writable();
		Log(2, "mounted %s at %ls (%s) from %s", vi.name.c_str(), fs->MountPoint().c_str(), writable ? "read-write" : "read-only", disk.path.c_str());
		s.state = "mounted";
		s.note = rw && !writable ? "read-only: " + rw_problem : "";
		disk.mounts.push_back(Mounted{ vi.uuid, vi.name, std::move(fs) });
		return true;
	}

	// Handle notifications on the disk: Windows asks before removing it.
	void WatchRemoval(Disk &disk)
	{
		std::wstring p = Utf8ToWide(disk.path);
		HANDLE h = CreateFileW(p.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return;
		DEV_BROADCAST_HANDLE f = {};
		f.dbch_size = sizeof(f);
		f.dbch_devicetype = DBT_DEVTYP_HANDLE;
		f.dbch_handle = h;
		disk.notify = RegisterDeviceNotificationW(m_status, &f, DEVICE_NOTIFY_SERVICE_HANDLE);
		if (!disk.notify)
		{
			CloseHandle(h);
			return;
		}
		disk.notify_handle = h;
	}

	const Config &m_cfg;
	std::vector<std::string> m_images;
	SERVICE_STATUS_HANDLE m_status;
	std::mutex m_mutex;
	std::vector<std::unique_ptr<Disk>> m_disks;
	std::set<std::string> m_ejected;   // disks ejected through the control pipe, until they go
};

// ---------------------------------------------------------------- control pipe

class ControlServer
{
public:
	// sddl: who may connect (null: the default, i.e. the owner)
	ControlServer(AutoMounter &m, const wchar_t *pipe, const wchar_t *sddl) : m_mounter(m), m_pipe(pipe)
	{
		m_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (sddl)
			ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &m_sd, nullptr);
		m_thread = std::thread([this] { Run(); });
	}

	~ControlServer()
	{
		SetEvent(m_stop);
		if (m_thread.joinable())
			m_thread.join();
		CloseHandle(m_stop);
		if (m_sd)
			LocalFree(m_sd);
	}

private:
	HANDLE NewInstance()
	{
		SECURITY_ATTRIBUTES sa = { sizeof(sa), m_sd, FALSE };
		HANDLE h = CreateNamedPipeW(m_pipe.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
			PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, PIPE_UNLIMITED_INSTANCES,
			65536, 4096, 0, m_sd ? &sa : nullptr);
		if (h == INVALID_HANDLE_VALUE)
			Log(0, "cannot create the control pipe (error %lu)", GetLastError());
		return h;
	}

	// One instance always listens: the next is created before a client is
	// served, so the pipe name never disappears between requests.
	void Run()
	{
		HANDLE h = NewInstance();
		while (h != INVALID_HANDLE_VALUE && WaitForSingleObject(m_stop, 0) == WAIT_TIMEOUT)
		{
			OVERLAPPED ov = {};
			ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			bool connected = ConnectNamedPipe(h, &ov) != 0 || GetLastError() == ERROR_PIPE_CONNECTED;
			if (!connected && GetLastError() == ERROR_IO_PENDING)
			{
				HANDLE waits[] = { m_stop, ov.hEvent };
				if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1)
				{
					DWORD got;
					connected = GetOverlappedResult(h, &ov, &got, FALSE) != 0;
				}
				else
					CancelIo(h);
			}
			CloseHandle(ov.hEvent);
			HANDLE next = connected ? NewInstance() : INVALID_HANDLE_VALUE;
			if (connected)
				Serve(h);
			DisconnectNamedPipe(h);
			CloseHandle(h);
			h = next;
		}
		if (h != INVALID_HANDLE_VALUE)
			CloseHandle(h);
	}

	void Serve(HANDLE h)
	{
		constexpr size_t kMaxRequestBytes = 65536;
		std::string req;
		char buf[4096];
		OVERLAPPED ov = {};
		ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!ov.hEvent)
			return;
		bool too_large = false;
		for (;;)
		{
			DWORD got = 0;
			BOOL ok = ReadFile(h, buf, sizeof(buf), nullptr, &ov);
			DWORD e = ok ? 0 : GetLastError();
			if (ok || e == ERROR_IO_PENDING || e == ERROR_MORE_DATA)
			{
				ok = GetOverlappedResult(h, &ov, &got, TRUE);
				e = ok ? 0 : GetLastError();
			}
			if (got > kMaxRequestBytes || req.size() > kMaxRequestBytes - got)
			{
				too_large = true;
				break;
			}
			req.append(buf, got);
			if (ok)
				break;
			if (e != ERROR_MORE_DATA)   // the rest of a longer message follows
			{
				SecureZeroMemory(buf, sizeof(buf));
				SecureClear(req);
				CloseHandle(ov.hEvent);
				return;
			}
		}
		SecureZeroMemory(buf, sizeof(buf));
		std::string resp;
		if (too_large)
		{
			SecureClear(req);
			resp = "ERR request too large";
		}
		else
		{
			while (!req.empty() && (req.back() == '\n' || req.back() == '\r'))
				req.pop_back();
			resp = Handle(req);
			SecureClear(req);
		}
		DWORD done = 0;
		if (!WriteFile(h, resp.data(), static_cast<DWORD>(resp.size()), nullptr, &ov) && GetLastError() == ERROR_IO_PENDING)
			GetOverlappedResult(h, &ov, &done, TRUE);
		FlushFileBuffers(h);
		CloseHandle(ov.hEvent);
	}

	std::string Handle(const std::string &req)
	{
		std::vector<std::string> f = SplitTabs(req);
		const std::string &cmd = f[0];
		std::string err, out;
		if (cmd == "list")
		{
			for (const auto &l : m_mounter.List())
				out += l + "\n";
		}
		else if (cmd == "mount" && f.size() >= 2)
			err = m_mounter.Mount(f[1], f.size() >= 3 ? f[2] : "");
		else if (cmd == "unmount" && f.size() >= 2)
			err = m_mounter.Unmount(f[1]);
		else if (cmd == "eject" && f.size() >= 2)
			err = m_mounter.Eject(f[1]);
		else if (cmd == "set" && f.size() >= 4)
			err = m_mounter.Set(f[1], f[2], f[3]);
		else if (cmd == "rescan")
			m_mounter.Scan();
		else
			err = "unknown request";
		for (auto &part : f)
			SecureClear(part);
		return out + (err.empty() ? "OK" : "ERR " + err);
	}

	AutoMounter &m_mounter;
	std::wstring m_pipe;
	PSECURITY_DESCRIPTOR m_sd = nullptr;
	HANDLE m_stop = nullptr;
	std::thread m_thread;
};

// interactive users may use the pipe; SYSTEM and administrators fully
const wchar_t *kPipeSddl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)";

// ---------------------------------------------------------------- the service

SERVICE_STATUS_HANDLE g_status = nullptr;
SERVICE_STATUS g_ss = {};
HANDLE g_stop = nullptr, g_rescan = nullptr;
AutoMounter *g_mounter = nullptr;

void ReportStatus(DWORD state, DWORD wait_hint = 0)
{
	g_ss.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
	g_ss.dwCurrentState = state;
	g_ss.dwControlsAccepted = state == SERVICE_RUNNING ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_PRESHUTDOWN) : 0;
	g_ss.dwWin32ExitCode = NO_ERROR;
	g_ss.dwWaitHint = wait_hint;
	if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING)
		g_ss.dwCheckPoint++;
	else
		g_ss.dwCheckPoint = 0;
	SetServiceStatus(g_status, &g_ss);
}

DWORD WINAPI Handler(DWORD control, DWORD type, LPVOID data, LPVOID)
{
	switch (control)
	{
	case SERVICE_CONTROL_STOP:
	case SERVICE_CONTROL_PRESHUTDOWN:
		ReportStatus(SERVICE_STOP_PENDING, 30000);
		SetEvent(g_stop);
		return NO_ERROR;
	case SERVICE_CONTROL_INTERROGATE:
		return NO_ERROR;
	case SERVICE_CONTROL_DEVICEEVENT:
	{
		auto *hdr = static_cast<DEV_BROADCAST_HDR *>(data);
		if (hdr && hdr->dbch_devicetype == DBT_DEVTYP_HANDLE)
		{
			auto *h = static_cast<DEV_BROADCAST_HANDLE *>(data);
			// unmount before answering, so that removal can go ahead
			if ((type == DBT_DEVICEQUERYREMOVE || type == DBT_DEVICEREMOVEPENDING || type == DBT_DEVICEREMOVECOMPLETE) && g_mounter)
				g_mounter->ReleaseByNotifyHandle(h->dbch_handle);
			else if (type == DBT_DEVICEQUERYREMOVEFAILED)
				SetEvent(g_rescan);
		}
		else if (type == DBT_DEVICEARRIVAL || type == DBT_DEVICEREMOVECOMPLETE)
			SetEvent(g_rescan);
		return NO_ERROR;
	}
	default:
		return ERROR_CALL_NOT_IMPLEMENTED;
	}
}

void WINAPI ServiceMain(DWORD, LPWSTR *)
{
	g_status = RegisterServiceCtrlHandlerExW(kServiceName, Handler, nullptr);
	if (!g_status)
		return;
	ReportStatus(SERVICE_START_PENDING, 10000);
	g_event_source = RegisterEventSourceW(nullptr, kServiceName);
	SetLogSink(EventLogSink);
	g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	g_rescan = CreateEventW(nullptr, FALSE, FALSE, nullptr);

	if (!LoadWinFsp())
	{
		Log(0, "WinFsp is not installed (https://winfsp.dev); the service cannot mount anything");
		g_ss.dwWin32ExitCode = ERROR_SERVICE_DEPENDENCY_FAIL;
		ReportStatus(SERVICE_STOPPED);
		return;
	}

	// disk arrival and removal
	DEV_BROADCAST_DEVICEINTERFACE_W f = {};
	f.dbcc_size = sizeof(f);
	f.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
	f.dbcc_classguid = GUID_DEVINTERFACE_DISK;
	HDEVNOTIFY dn = RegisterDeviceNotificationW(g_status, &f, DEVICE_NOTIFY_SERVICE_HANDLE);

	Config cfg;
	{
		AutoMounter m(cfg, {}, g_status);
		g_mounter = &m;
		{
			ControlServer control(m, kServicePipe, kPipeSddl);
			ReportStatus(SERVICE_RUNNING);
			Log(2, "started");
			m.Scan();
			HANDLE waits[] = { g_stop, g_rescan };
			for (;;)
			{
				DWORD w = WaitForMultipleObjects(2, waits, FALSE, 60000);   // and a rescan every minute
				if (w == WAIT_OBJECT_0)
					break;
				if (w == WAIT_OBJECT_0 + 1)
				{
					// a new disk takes a moment to show its partitions
					Sleep(1500);
					m.Scan();
					Sleep(3000);
				}
				m.Scan();
			}
		}
		g_mounter = nullptr;
		m.UnmountAll();
	}
	if (dn)
		UnregisterDeviceNotification(dn);
	Log(2, "stopped");
	if (g_event_source)
		DeregisterEventSource(g_event_source);
	ReportStatus(SERVICE_STOPPED);
}

// ---------------------------------------------------------------- debug mode

HANDLE g_debug_stop = nullptr;

BOOL WINAPI DebugCtrl(DWORD)
{
	SetEvent(g_debug_stop);
	return TRUE;
}

int Debug(const std::vector<std::wstring> &args)
{
	std::vector<std::string> images;
	std::wstring run;
	int seconds = 0;
	for (size_t i = 0; i < args.size(); i++)
	{
		if (args[i] == L"--image" && i + 1 < args.size())
			images.push_back(WideToUtf8(args[++i]));
		else if (args[i] == L"--run" && i + 1 < args.size())
			run = args[++i];
		else if (args[i] == L"--seconds" && i + 1 < args.size())
			seconds = _wtoi(args[++i].c_str());
		else
		{
			fprintf(stderr, "usage: winapfs service debug [--image <img>]... [--run <command>] [--seconds <n>]\n");
			return 2;
		}
	}
	if (!LoadWinFsp())
	{
		fprintf(stderr, "WinFsp is not installed (https://winfsp.dev)\n");
		return 1;
	}
	Config cfg;
	AutoMounter m(cfg, images, nullptr);
	ControlServer control(m, kDebugPipe, nullptr);
	m.Scan();
	if (!run.empty())
	{
		for (const auto &mp : m.Mounts())
			SetEnvironmentVariableW(Utf8ToWide(MountEnvName(mp.first)).c_str(), mp.second.c_str());
		SetEnvironmentVariableW(L"WINAPFS_PIPE", kDebugPipe);
		STARTUPINFOW si = { sizeof(si) };
		PROCESS_INFORMATION pi = {};
		if (!CreateProcessW(nullptr, &run[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
		{
			fprintf(stderr, "cannot run command (error %lu)\n", GetLastError());
			return 1;
		}
		WaitForSingleObject(pi.hProcess, INFINITE);
		DWORD code = 1;
		GetExitCodeProcess(pi.hProcess, &code);
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		return static_cast<int>(code);
	}
	g_debug_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	SetConsoleCtrlHandler(DebugCtrl, TRUE);
	fprintf(stderr, "auto-mounting (rescan every 3 s, control pipe %ls); press Ctrl+C to stop\n", kDebugPipe);
	ULONGLONG end = seconds ? GetTickCount64() + seconds * 1000ull : 0;
	while (WaitForSingleObject(g_debug_stop, 3000) == WAIT_TIMEOUT && (!end || GetTickCount64() < end))
		m.Scan();
	return 0;
}

// ---------------------------------------------------------------- install & control

std::wstring ExePath()
{
	wchar_t buf[MAX_PATH];
	GetModuleFileNameW(nullptr, buf, MAX_PATH);
	return buf;
}

int Fail(const char *what)
{
	DWORD e = GetLastError();
	if (e == ERROR_ACCESS_DENIED)
		fprintf(stderr, "%s: access denied (run as administrator)\n", what);
	else
		fprintf(stderr, "%s failed (error %lu)\n", what, e);
	return 1;
}

bool WaitState(SC_HANDLE svc, DWORD want, DWORD timeout_ms)
{
	SERVICE_STATUS st;
	ULONGLONG end = GetTickCount64() + timeout_ms;
	while (QueryServiceStatus(svc, &st))
	{
		if (st.dwCurrentState == want)
			return true;
		if (GetTickCount64() > end)
			return false;
		Sleep(250);
	}
	return false;
}

int Install()
{
	SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
	if (!scm)
		return Fail("opening the service manager");
	std::wstring cmd = L"\"" + ExePath() + L"\" service run";
	SC_HANDLE svc = CreateServiceW(scm, kServiceName, kDisplayName, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
		SERVICE_ERROR_NORMAL, cmd.c_str(), nullptr, nullptr, nullptr, nullptr /* LocalSystem */, nullptr);
	if (!svc && GetLastError() == ERROR_SERVICE_EXISTS)
	{
		svc = OpenServiceW(scm, kServiceName, SERVICE_ALL_ACCESS);
		if (svc)
			ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE, cmd.c_str(), nullptr, nullptr, nullptr,
				nullptr, nullptr, kDisplayName);
	}
	if (!svc)
	{
		int rc = Fail("creating the service");
		CloseServiceHandle(scm);
		return rc;
	}
	SERVICE_DESCRIPTIONW d = { const_cast<LPWSTR>(kDescription) };
	ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &d);
	SC_ACTION actions[] = { { SC_ACTION_RESTART, 5000 }, { SC_ACTION_RESTART, 30000 }, { SC_ACTION_NONE, 0 } };
	SERVICE_FAILURE_ACTIONSW fa = {};
	fa.dwResetPeriod = 86400;
	fa.cActions = 3;
	fa.lpsaActions = actions;
	ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);
	SERVICE_PRESHUTDOWN_INFO pi = { 60000 };
	ChangeServiceConfig2W(svc, SERVICE_CONFIG_PRESHUTDOWN_INFO, &pi);

	// event log source: the message table is in winapfs.exe
	HKEY k;
	if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\EventLog\\Application\\winapfs", 0, nullptr, 0,
			KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS)
	{
		std::wstring exe = ExePath();
		DWORD types = EVENTLOG_ERROR_TYPE | EVENTLOG_WARNING_TYPE | EVENTLOG_INFORMATION_TYPE;
		RegSetValueExW(k, L"EventMessageFile", 0, REG_SZ, reinterpret_cast<const BYTE *>(exe.c_str()), static_cast<DWORD>((exe.size() + 1) * 2));
		RegSetValueExW(k, L"TypesSupported", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&types), sizeof(types));
		RegCloseKey(k);
	}

	bool started = StartServiceW(svc, 0, nullptr) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
	printf("service installed (%ls)%s\n", cmd.c_str(), started ? " and started" : "");
	CloseServiceHandle(svc);
	CloseServiceHandle(scm);
	return started ? 0 : Fail("starting the service");
}

int Uninstall()
{
	SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
	if (!scm)
		return Fail("opening the service manager");
	SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
	if (!svc)
	{
		int rc = GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST ? (printf("service not installed\n"), 0) : Fail("opening the service");
		CloseServiceHandle(scm);
		return rc;
	}
	SERVICE_STATUS st;
	if (ControlService(svc, SERVICE_CONTROL_STOP, &st))
		WaitState(svc, SERVICE_STOPPED, 60000);
	bool ok = DeleteService(svc) != 0;
	int rc = ok ? 0 : Fail("deleting the service");
	RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\EventLog\\Application\\winapfs");
	if (ok)
		printf("service removed (settings in HKLM\\SOFTWARE\\winapfs are kept)\n");
	CloseServiceHandle(svc);
	CloseServiceHandle(scm);
	return rc;
}

int Control(bool start)
{
	SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
	if (!scm)
		return Fail("opening the service manager");
	SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS);
	int rc = 0;
	if (!svc)
		rc = Fail("opening the service");
	else if (start)
		rc = (StartServiceW(svc, 0, nullptr) && WaitState(svc, SERVICE_RUNNING, 30000)) ? 0 : Fail("starting the service");
	else
	{
		SERVICE_STATUS st;
		rc = (ControlService(svc, SERVICE_CONTROL_STOP, &st) && WaitState(svc, SERVICE_STOPPED, 60000)) ? 0 : Fail("stopping the service");
	}
	if (svc)
		CloseServiceHandle(svc);
	CloseServiceHandle(scm);
	return rc;
}

int Status()
{
	SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
	SC_HANDLE svc = scm ? OpenServiceW(scm, kServiceName, SERVICE_QUERY_STATUS) : nullptr;
	SERVICE_STATUS st = {};
	const char *s = "not installed";
	if (svc && QueryServiceStatus(svc, &st))
		s = st.dwCurrentState == SERVICE_RUNNING ? "running" : st.dwCurrentState == SERVICE_STOPPED ? "stopped" : "changing state";
	printf("winapfs service: %s\n", s);
	if (svc)
		CloseServiceHandle(svc);
	if (scm)
		CloseServiceHandle(scm);
	return svc ? 0 : 1;
}

// `service config [<name> <0|1>]`: the global switches (administrators)
int ConfigCmd(const std::vector<std::wstring> &args)
{
	Config cfg;
	const wchar_t *names[] = { L"AllowDiskWrites", L"ReadWrite", L"HideDotfiles" };
	if (args.empty())
	{
		for (const wchar_t *n : names)
			printf("%ls = %lu\n", n, cfg.Dword(L"", n, wcscmp(n, L"HideDotfiles") == 0 ? 1 : 0));
		return 0;
	}
	if (args.size() != 2 || (args[1] != L"0" && args[1] != L"1"))
	{
		fprintf(stderr, "usage: winapfs service config [AllowDiskWrites|ReadWrite|HideDotfiles 0|1]\n");
		return 2;
	}
	bool known = false;
	for (const wchar_t *n : names)
		known = known || args[0] == n;
	if (!known)
	{
		fprintf(stderr, "unknown setting %ls\n", args[0].c_str());
		return 2;
	}
	if (!cfg.SetDword(L"", args[0].c_str(), args[1] == L"1"))
		return Fail("storing the setting");
	printf("%ls = %ls\n", args[0].c_str(), args[1].c_str());
	// let a running service apply it
	std::vector<std::string> lines;
	std::string err;
	ControlRequest(DefaultControlPipe(), "rescan", lines, err);
	return 0;
}

void ServiceUsage()
{
	fprintf(stderr,
		"usage: winapfs service install     install and start the auto-mount service (administrator)\n"
		"       winapfs service uninstall   stop and remove it\n"
		"       winapfs service start|stop|status\n"
		"       winapfs service config [<name> <0|1>]   global settings (administrator)\n"
		"       winapfs service debug [--image <img>]... [--run <cmd>] [--seconds <n>]\n"
		"                                   the same in this console (images instead of disks)\n"
		"\n"
		"The service mounts every APFS volume read-only at a drive letter it remembers\n"
		"per volume. Settings (HKLM\\SOFTWARE\\winapfs): AllowDiskWrites and ReadWrite\n"
		"(DWORD, both 1 for read-write disks), HideDotfiles (default 1); per volume under\n"
		"Volumes\\<UUID>: DriveLetter (\"X:\"), ReadWrite, HideDotfiles, Disabled.\n"
		"winapfs ctl and the tray app (winapfs-tray) control it.\n");
}

} // namespace

int CmdService(const std::vector<std::wstring> &args)
{
	std::wstring sub = args.empty() ? L"" : args[0];
	std::vector<std::wstring> rest(args.begin() + (args.empty() ? 0 : 1), args.end());
	if (sub == L"run")
	{
		SERVICE_TABLE_ENTRYW table[] = { { const_cast<LPWSTR>(kServiceName), ServiceMain }, { nullptr, nullptr } };
		if (!StartServiceCtrlDispatcherW(table))
		{
			fprintf(stderr, "winapfs service run is started by the service manager; use winapfs service debug\n");
			return 1;
		}
		return 0;
	}
	if (sub == L"debug")
		return Debug(rest);
	if (sub == L"install")
		return Install();
	if (sub == L"uninstall")
		return Uninstall();
	if (sub == L"start" || sub == L"stop")
		return Control(sub == L"start");
	if (sub == L"status")
		return Status();
	if (sub == L"config")
		return ConfigCmd(rest);
	ServiceUsage();
	return 2;
}

// ---------------------------------------------------------------- winapfs ctl

int CmdCtl(const std::vector<std::wstring> &args)
{
	std::wstring pipe = DefaultControlPipe();
	auto usage = [] {
		fprintf(stderr,
			"usage: winapfs ctl list\n"
			"       winapfs ctl mount <volume> [--password <pw>]\n"
			"       winapfs ctl unmount <volume>\n"
			"       winapfs ctl eject <volume>        unmount the disk's volumes and eject it\n"
			"       winapfs ctl set <volume> <ReadWrite|Disabled|HideDotfiles> <0|1>\n"
			"       winapfs ctl rescan\n"
			"<volume>: a name or UUID from `winapfs ctl list`. Talks to the service\n"
			"(WINAPFS_PIPE selects another pipe, e.g. service debug's).\n");
		return 2;
	};
	if (args.empty())
		return usage();
	std::string cmd = WideToUtf8(args[0]);
	std::string err;
	if (cmd == "list")
	{
		std::vector<VolumeStatus> vols;
		bool adw = false;
		if (!ControlList(pipe, vols, adw, err))
		{
			fprintf(stderr, "%s\n", err.c_str());
			return 1;
		}
		for (const auto &v : vols)
			printf("%-24s %-3s %-9s %-2s  %s  %s%s%s\n", v.name.c_str(), v.letter.c_str(), v.state.c_str(), v.mode.c_str(), v.uuid.c_str(),
				v.disk.c_str(), v.note.empty() ? "" : "  # ", v.note.c_str());
		printf("disk writes %s\n", adw ? "allowed (AllowDiskWrites=1)" : "not allowed (winapfs service config AllowDiskWrites 1)");
		return 0;
	}
	if (cmd == "rescan")
	{
		std::vector<std::string> lines;
		int rc = ControlRequest(pipe, "rescan", lines, err);
		if (rc)
			fprintf(stderr, "%s\n", err.c_str());
		return rc ? 1 : 0;
	}
	if (args.size() < 2)
		return usage();
	// a name or UUID
	std::vector<VolumeStatus> vols;
	bool adw = false;
	if (!ControlList(pipe, vols, adw, err))
	{
		fprintf(stderr, "%s\n", err.c_str());
		return 1;
	}
	std::string want = WideToUtf8(args[1]), uuid;
	for (const auto &v : vols)
		if (v.uuid == want || v.name == want)
			uuid = v.uuid;
	if (uuid.empty())
	{
		fprintf(stderr, "no volume '%s' (see winapfs ctl list)\n", want.c_str());
		return 1;
	}
	std::string req;
	if (cmd == "mount")
	{
		req = "mount\t" + uuid;
		if (args.size() >= 4 && args[2] == L"--password")
			req += "\t" + WideToUtf8(args[3]);
	}
	else if (cmd == "unmount" || cmd == "eject")
		req = cmd + "\t" + uuid;
	else if (cmd == "set" && args.size() >= 4)
		req = "set\t" + uuid + "\t" + WideToUtf8(args[2]) + "\t" + WideToUtf8(args[3]);
	else
		return usage();
	std::vector<std::string> lines;
	int rc = ControlRequest(pipe, req, lines, err);
	if (rc)
	{
		fprintf(stderr, "%s\n", err.c_str());
		return 1;
	}
	printf("ok\n");
	return 0;
}
