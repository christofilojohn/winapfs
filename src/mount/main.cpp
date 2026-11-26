// winapfs - read-only APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// winapfs: mount APFS volumes through WinFsp; `winapfs service` auto-mounts.

#include "ApfsFs.h"
#include "Mount.h"
#include "Profiler.h"
#include "Service.h"

#include <winapfs/Names.h>

#include <ApfsLib/Util.h>

#include <Windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <io.h>
#include <string>
#include <vector>

using namespace winapfs;

static HANDLE g_stop_event;

static void Usage()
{
	fprintf(stderr,
		"winapfs - APFS volumes on Windows (WinFsp)\n"
		"\n"
		"usage:\n"
		"  winapfs list [<image|\\\\.\\PhysicalDriveN>...]\n"
		"  winapfs mount <image|\\\\.\\PhysicalDriveN> [X:|dir|*] [options]\n"
		"  winapfs mount-all [<image|\\\\.\\PhysicalDriveN>...] [options]\n"
		"  winapfs service install|uninstall|start|stop|status|config|debug   (see winapfs service)\n"
		"  winapfs ctl list|mount|unmount|eject|verify|set|rescan   control the service (see winapfs ctl)\n"
		"\n"
		"Without a source, list and mount-all scan every physical disk for APFS\n"
		"partitions (GPT type 7C3457EF-0000-11AA-AA11-00306543ECAC). Raw disk access\n"
		"needs an elevated (administrator) prompt.\n"
		"\n"
		"options:\n"
		"  -v, --volume <name|index>  volume to mount (mount; default: first)\n"
		"  --partition <n>            GPT entry index (mount; default: first APFS partition)\n"
		"  --dir <base>               mount-all: mount each volume at <base>\\<volume name>\n"
		"                             instead of a free drive letter\n"
		"  -p, --password <pw>        password or recovery key for encrypted volumes\n"
		"  --ask                      prompt for the password of encrypted volumes\n"
		"  --rw                       mount read-write (experimental). Refused when the disk\n"
		"                             was not ejected cleanly, and for system, Time Machine,\n"
		"                             sealed, encrypted and Fusion volumes\n"
		"  --allow-disk-writes        with --rw: also write to physical disks (have a backup)\n"
		"  --force                    with --rw: write although the disk was not ejected\n"
		"                             cleanly (only if no Mac is hibernated on it)\n"
		"  --engine auto|rw|lib       auto (default): linux-apfs-rw, or apfs-fuse for\n"
		"                             encrypted volumes; rw or lib forces one\n"
		"  --show-dotfiles            don't mark names starting with '.' hidden (they are by\n"
		"                             default, as on macOS)\n"
		"  --run <command>            mount, run command, unmount, and exit with its exit\n"
		"                             code. %%MOUNT%% is the mount point (mount); the\n"
		"                             environment gets WINAPFS_MOUNT_<volume>=<mount point>\n"
		"  -d, --debug                WinFsp debug log to stderr\n"
		"\n"
		"Without --run, stays mounted until Ctrl+C.\n"
		"\n"
		"winapfs is free software (GPL-2.0). It uses:\n"
		"%s\n", kWinFspNotice);
}

static BOOL WINAPI CtrlHandler(DWORD)
{
	SetEvent(g_stop_event);
	return TRUE;
}

struct Args
{
	std::vector<std::wstring> positional;
	std::string volume;
	std::string password;
	std::wstring run;
	std::wstring dir;
	bool ask = false;
	int partition = -1;
	std::string engine = "auto";
	bool rw = false;
	bool allow_disk_writes = false;
	bool force = false;
	MountOptions opts;
};

static bool ParseArgs(const std::vector<std::wstring> &argv, Args &a)
{
	for (size_t i = 0; i < argv.size(); i++)
	{
		const std::wstring &s = argv[i];
		auto next = [&](std::wstring &dst) {
			if (i + 1 >= argv.size())
				return false;
			dst = argv[++i];
			return true;
		};
		std::wstring v;
		if (s == L"-v" || s == L"--volume") { if (!next(v)) return false; a.volume = WideToUtf8(v); }
		else if (s == L"-p" || s == L"--password") { if (!next(v)) return false; a.password = WideToUtf8(v); }
		else if (s == L"--partition") { if (!next(v)) return false; a.partition = _wtoi(v.c_str()); }
		else if (s == L"--dir") { if (!next(a.dir)) return false; }
		else if (s == L"--run") { if (!next(a.run)) return false; }
		else if (s == L"--engine") { if (!next(v)) return false; a.engine = WideToUtf8(v); if (a.engine != "auto" && a.engine != "rw" && a.engine != "lib") return false; }
		else if (s == L"--rw") a.rw = true;
		else if (s == L"--allow-disk-writes") a.allow_disk_writes = true;
		else if (s == L"--force") a.force = true;
		else if (s == L"--ask") a.ask = true;
		else if (s == L"--hide-dotfiles") a.opts.hide_dotfiles = true;   // the default; kept for old scripts
		else if (s == L"--show-dotfiles") a.opts.hide_dotfiles = false;
		else if (s == L"-d" || s == L"--debug") a.opts.debug = true;
		else if (!s.empty() && s[0] == L'-' && s != L"*") return false;
		else a.positional.push_back(s);
	}
	return true;
}

// Mounted file systems plus the containers they read from.
struct Session
{
	DiskLocks locks;   // first member: released after everything that writes
	std::vector<std::unique_ptr<Container>> containers;
	std::vector<std::unique_ptr<ApfsFs>> mounts;

	~Session()
	{
		for (auto &m : mounts)
		{
			std::wstring mp = m->MountPoint();
			m->Stop();
			fprintf(stderr, "unmounted %ls\n", mp.c_str());
		}
		mounts.clear();
		containers.clear();
	}

	int RunOrWait(std::wstring cmd)
	{
		if (cmd.empty())
		{
			g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			SetConsoleCtrlHandler(CtrlHandler, TRUE);
			fprintf(stderr, "press Ctrl+C to unmount\n");
			WaitForSingleObject(g_stop_event, INFINITE);
			return 0;
		}

		size_t pos;
		std::wstring first = mounts.empty() ? L"" : mounts.front()->MountPoint();
		while ((pos = cmd.find(L"%MOUNT%")) != std::wstring::npos)
			cmd.replace(pos, 7, first);

		STARTUPINFOW si = { sizeof(si) };
		PROCESS_INFORMATION pi = {};
		if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
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
};

static OpenOptions Opts(const Args &a, Session &s)
{
	OpenOptions o;
	o.rw = a.rw;
	o.allow_disk_writes = a.allow_disk_writes;
	o.force = a.force;
	o.engine = a.engine;
	o.password = a.password;
	o.ask = a.ask;
	o.locks = &s.locks;
	return o;
}

static bool MountOne(Session &s, std::unique_ptr<Volume> vol, const std::wstring &mount_point, const MountOptions &opts)
{
	std::string name = vol->Info().name;
	std::unique_ptr<ApfsFs> fs(new ApfsFs(std::move(vol), opts));
	NTSTATUS r = fs->Start(mount_point.empty() || mount_point == L"*" ? nullptr : mount_point.c_str());
	if (!NT_SUCCESS(r))
	{
		fprintf(stderr, "cannot mount %s: NTSTATUS 0x%08lx\n", name.c_str(), static_cast<unsigned long>(r));
		return false;
	}
	std::wstring mp = fs->MountPoint();
	fprintf(stderr, "mounted %s at %ls (%s, %s)\n", name.c_str(), mp.c_str(),
		fs->Vol().Writable() ? "read-write, experimental" : "read-only", fs->Vol().EngineName());
	SetEnvironmentVariableW(Utf8ToWide(MountEnvName(name)).c_str(), mp.c_str());
	s.mounts.push_back(std::move(fs));
	return true;
}

static std::vector<std::string> DefaultSources(bool verbose)
{
	std::vector<std::string> out;
	bool denied = false;
	for (const auto &d : EnumeratePhysicalDrives())
	{
		if (d.access_denied)
			denied = true;
		else if (!d.apfs.empty())
			out.push_back(d.path);
	}
	if (denied && verbose)
		fprintf(stderr, "some physical disks could not be opened; run from an elevated prompt\n");
	return out;
}

static int CmdList(const Args &a)
{
	std::vector<std::string> sources;
	if (a.positional.empty())
	{
		for (const auto &d : EnumeratePhysicalDrives())
		{
			if (d.access_denied)
				printf("%s: access denied (run as administrator)\n", d.path.c_str());
			else
				printf("%s: %.1f GB, %s, %zu APFS partition(s)\n", d.path.c_str(), d.size / 1e9, d.has_gpt ? "GPT" : "no GPT", d.apfs.size());
			if (!d.apfs.empty())
				sources.push_back(d.path);
		}
	}
	else
	{
		for (const auto &p : a.positional)
			sources.push_back(WideToUtf8(p));
	}

	for (const auto &src : sources)
	{
		std::string err;
		std::vector<PartitionInfo> parts;
		Container::FindApfsPartitions(src, parts, err);
		std::vector<int> which;
		for (const auto &p : parts)
			which.push_back(p.index);
		if (which.empty())
			which.push_back(-1);
		for (int part : which)
		{
			std::unique_ptr<Container> c = Container::Open(src, err, part);
			if (!c)
			{
				printf("%s: %s\n", src.c_str(), err.c_str());
				continue;
			}
			printf("%s partition %d (offset %llu): APFS container, %llu MB\n", src.c_str(), part,
				static_cast<unsigned long long>(c->PartitionOffset()),
				static_cast<unsigned long long>(c->BlockCount() * c->BlockSize() >> 20));
			for (const auto &v : c->Volumes())
				printf("  volume %u: %-24s %s%s\n", v.fsid, v.name.c_str(), v.case_sensitive ? "case-sensitive" : "case-insensitive",
					v.encrypted ? ", encrypted" : "");
		}
	}
	return 0;
}

static int CmdMount(const Args &a)
{
	if (a.positional.empty() || a.positional.size() > 2)
	{
		Usage();
		return 2;
	}
	std::string err;
	std::unique_ptr<Container> c = Container::Open(WideToUtf8(a.positional[0]), err, a.partition);
	if (!c)
	{
		fprintf(stderr, "%s\n", err.c_str());
		return 1;
	}
	if (c->Volumes().empty())
	{
		fprintf(stderr, "container has no volumes\n");
		return 1;
	}
	const VolumeInfo *vi = a.volume.empty() ? &c->Volumes().front() : c->FindVolume(a.volume);
	if (!vi)
	{
		fprintf(stderr, "no volume '%s'\n", a.volume.c_str());
		return 1;
	}
	Args aa = a;
	// a single explicit volume: offer the prompt when there is someone to answer it
	aa.ask = a.ask || (a.password.empty() && _isatty(_fileno(stdin)));
	Session s;
	std::unique_ptr<Volume> vol = OpenVolume(*c, *vi, Opts(aa, s));
	if (!vol)
		return 1;

	s.containers.push_back(std::move(c));
	if (!MountOne(s, std::move(vol), a.positional.size() > 1 ? a.positional[1] : L"*", a.opts))
		return 1;
	return s.RunOrWait(a.run);
}

static int CmdMountAll(const Args &a)
{
	std::vector<std::string> sources;
	for (const auto &p : a.positional)
		sources.push_back(WideToUtf8(p));
	if (sources.empty())
		sources = DefaultSources(true);
	if (sources.empty())
	{
		fprintf(stderr, "no APFS partitions found\n");
		return 1;
	}

	Session s;
	int failures = 0;
	for (const auto &src : sources)
	{
		std::string err;
		std::vector<PartitionInfo> parts;
		Container::FindApfsPartitions(src, parts, err);
		std::vector<int> which;
		for (const auto &p : parts)
			which.push_back(p.index);
		if (which.empty())
			which.push_back(-1);  // bare container image

		for (int part : which)
		{
			std::unique_ptr<Container> c = Container::Open(src, err, part);
			if (!c)
			{
				fprintf(stderr, "%s: %s\n", src.c_str(), err.c_str());
				failures++;
				continue;
			}
			for (const auto &vi : c->Volumes())
			{
				std::unique_ptr<Volume> vol = OpenVolume(*c, vi, Opts(a, s));
				if (!vol && a.rw)
				{
					// a volume refused for writing (macOS system volume, not ejected
					// cleanly, ...) is still mounted, read-only, as the service does
					Args ro = a;
					ro.rw = false;
					vol = OpenVolume(*c, vi, Opts(ro, s));
					if (vol)
						fprintf(stderr, "%s: mounting read-only instead\n", vi.name.c_str());
				}
				if (!vol)
				{
					failures++;
					continue;
				}
				std::wstring mp;
				if (!a.dir.empty())
				{
					CreateDirectoryW(a.dir.c_str(), nullptr);
					mp = a.dir + L"\\" + ApfsNameToWindows(vi.name);
				}
				if (!MountOne(s, std::move(vol), mp, a.opts))
					failures++;
			}
			s.containers.push_back(std::move(c));
		}
	}

	if (s.mounts.empty())
		return 1;
	int rc = s.RunOrWait(a.run);
	if (rc == 0 && failures && !a.run.empty())
		rc = 3;  // command passed, but not every volume could be mounted
	return rc;
}

// On a crash: the exception and a symbolized stack of the faulting thread on
// stderr (for bug reports; the PDB next to the exe gives names and lines).
static LONG WINAPI CrashHandler(EXCEPTION_POINTERS *ep)
{
	HANDLE proc = GetCurrentProcess(), thread = GetCurrentThread();
	CONTEXT ctx = *ep->ContextRecord;
	STACKFRAME64 frame = {};
	fprintf(stderr, "\n*** winapfs crashed: exception 0x%08lx at %p\n", ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
	SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
	SymInitialize(proc, nullptr, TRUE);
	frame.AddrPC.Offset = ctx.Rip;
	frame.AddrFrame.Offset = ctx.Rbp;
	frame.AddrStack.Offset = ctx.Rsp;
	frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
	for (int i = 0; i < 40 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &frame, &ctx, nullptr,
			SymFunctionTableAccess64, SymGetModuleBase64, nullptr); i++)
	{
		char buf[sizeof(SYMBOL_INFO) + 256];
		SYMBOL_INFO *sym = reinterpret_cast<SYMBOL_INFO *>(buf);
		sym->SizeOfStruct = sizeof(SYMBOL_INFO);
		sym->MaxNameLen = 255;
		DWORD64 disp = 0;
		DWORD ldisp = 0;
		IMAGEHLP_LINE64 line = { sizeof(line) };
		const char *name = SymFromAddr(proc, frame.AddrPC.Offset, &disp, sym) ? sym->Name : "?";
		if (SymGetLineFromAddr64(proc, frame.AddrPC.Offset, &ldisp, &line))
			fprintf(stderr, "  %2d %s  %s:%lu\n", i, name, line.FileName, line.LineNumber);
		else
			fprintf(stderr, "  %2d %s\n", i, name);
	}
	fflush(stderr);
	return EXCEPTION_EXECUTE_HANDLER;
}

int wmain(int argc, wchar_t **wargv)
{
	SetConsoleOutputCP(CP_UTF8);
	SetUnhandledExceptionFilter(CrashHandler);
	StartProfilerFromEnv();
	atexit([] { StopProfilerAndReport(); ReportDirectoryListings(); });

	if (argc < 2)
	{
		Usage();
		return 2;
	}
	std::wstring cmd = wargv[1];
	if (cmd == L"ctl")
		return CmdCtl(std::vector<std::wstring>(wargv + 2, wargv + argc));
	if (cmd == L"service")
		return CmdService(std::vector<std::wstring>(wargv + 2, wargv + argc));
	Args a;
	if (!ParseArgs(std::vector<std::wstring>(wargv + 2, wargv + argc), a))
	{
		Usage();
		return 2;
	}

	if (cmd == L"list")
		return CmdList(a);

	if (cmd != L"mount" && cmd != L"mount-all")
	{
		Usage();
		return 2;
	}
	if (!LoadWinFsp())
	{
		fprintf(stderr, "WinFsp is not installed (https://winfsp.dev)\n");
		return 1;
	}
	return cmd == L"mount" ? CmdMount(a) : CmdMountAll(a);
}
