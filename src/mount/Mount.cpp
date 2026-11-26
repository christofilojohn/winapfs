// winapfs - APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Mount.h"

#include <ApfsLib/Util.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdarg>
#include <cstdio>

namespace winapfs {

static void SecureClear(std::string &s)
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

const char *const kWinFspNotice =
	"WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos\n"
	"https://github.com/winfsp/winfsp";

static LogSink g_sink = nullptr;
static thread_local std::string t_last_problem;

void SetLogSink(LogSink sink)
{
	g_sink = sink;
}

const std::string &LastLoggedProblem()
{
	return t_last_problem;
}

void ClearLoggedProblem()
{
	t_last_problem.clear();
}

void Log(int level, const char *fmt, ...)
{
	char buf[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (level <= 1)
		t_last_problem = buf;
	if (g_sink)
		g_sink(level, buf);
	else
		fprintf(stderr, "%s\n", buf);
}

std::string MountEnvName(const std::string &vol)
{
	std::string n = "WINAPFS_MOUNT_";
	for (char c : vol)
		n.push_back(isalnum(static_cast<unsigned char>(c)) ? c : '_');
	return n;
}

std::unique_ptr<Volume> OpenVolume(Container &c, const VolumeInfo &vi, const OpenOptions &o)
{
	std::string pw = o.password;
	SecretStringGuard pw_guard{ pw };
	std::string err;

	if (o.rw)
	{
		// physical disks: opt-in until macOS fsck_apfs has passed on images written here
		bool raw = c.Path().compare(0, 4, "\\\\.\\") == 0;
		if (raw && !o.allow_disk_writes)
		{
			Log(0, "%s: writing to physical disks is experimental; add --allow-disk-writes "
				"(and have a backup), or mount without --rw", vi.name.c_str());
			return nullptr;
		}
		if (o.engine == "lib")
		{
			Log(0, "%s: --rw needs the linux-apfs-rw engine", vi.name.c_str());
			return nullptr;
		}
		std::string why = c.WriteRefusal(vi, o.force);
		if (!why.empty())
		{
			Log(0, "%s: not writable: %s", vi.name.c_str(), why.c_str());
			return nullptr;
		}
		if (!c.CleanlyUnmounted())
			Log(1, "%s: warning: writing to a disk that was not ejected cleanly (--force)", vi.name.c_str());
		if (raw)
		{
			// Windows' RAW volume on the partition must be locked (once per partition)
			if (!o.locks)
			{
				Log(0, "%s: internal error: no lock owner for a raw-disk write", vi.name.c_str());
				return nullptr;
			}
			std::string key = c.Path() + "@" + std::to_string(c.PartitionOffset());
			bool have = false;
			for (const auto &l : *o.locks)
				have = have || l.first == key;
			if (!have)
			{
				std::unique_ptr<DiskWriteLock> lock = DiskWriteLock::Acquire(c.Path(), c.PartitionOffset(), c.PartitionSize(), err);
				if (!lock)
				{
					Log(0, "%s: %s", vi.name.c_str(), err.c_str());
					return nullptr;
				}
				o.locks->emplace_back(key, std::move(lock));
			}
		}
		std::unique_ptr<Volume> v = OpenRwVolume(c, vi.fsid, true, err);
		if (!v)
		{
			Log(0, "%s: %s", vi.name.c_str(), err.c_str());
			return nullptr;
		}
		if (!v->Writable())
		{
			// the engine falls back to read-only on its own refusals (see its messages)
			Log(0, "%s: the engine opened it read-only; not mounting with --rw", vi.name.c_str());
			return nullptr;
		}
		return v;
	}

	// linux-apfs-rw for everything it supports; ApfsLib for encrypted volumes
	if (o.engine == "rw" || (o.engine == "auto" && !vi.encrypted))
	{
		std::unique_ptr<Volume> v = OpenRwVolume(c, vi.fsid, false, err);
		if (v || o.engine == "rw")
		{
			if (!v)
				Log(0, "%s: %s", vi.name.c_str(), err.c_str());
			return v;
		}
		Log(1, "%s: %s; falling back to apfs-fuse", vi.name.c_str(), err.c_str());
	}

	for (int attempt = 0; attempt < 3; attempt++)
	{
		if (vi.encrypted && pw.empty() && o.ask)
		{
			if (!vi.password_hint.empty())
				fprintf(stderr, "Hint: %s\n", vi.password_hint.c_str());
			fprintf(stderr, "Password (or recovery key) for %s: ", vi.name.c_str());
			GetPassword(pw);
		}
		if (vi.encrypted && pw.empty())
		{
			Log(1, "skipping encrypted volume %s (use --ask or --password)", vi.name.c_str());
			return nullptr;
		}
		std::unique_ptr<Volume> v = c.OpenVolume(vi.fsid, pw, err);
		if (v)
			return v;
		if (!vi.encrypted || !o.ask)
			break;
		fprintf(stderr, "%s\n", err.c_str());
		SecureClear(pw);
	}
	Log(0, "%s: %s", vi.name.c_str(), err.c_str());
	return nullptr;
}

} // namespace winapfs
