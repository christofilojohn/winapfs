// winapfs - read-only APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// winapfs-cli: inspect APFS images and disks without mounting them.

#include <winapfs/Apfs.h>

#include <ApfsLib/Util.h>

#include <Windows.h>
#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

using namespace winapfs;

static bool g_show_flags;  // list -l --flags: BSD and internal inode flags (debugging)

static void Usage()
{
	fprintf(stderr,
		"winapfs-cli - read-only APFS inspector\n"
		"\n"
		"usage:\n"
		"  winapfs-cli info  <image|\\\\.\\PhysicalDriveN>\n"
		"  winapfs-cli list  <image> [path] [-r] [-l]\n"
		"  winapfs-cli cat   <image> <path>\n"
		"  winapfs-cli xattr <image> <path> [name]\n"
		"\n"
		"options:\n"
		"  -v, --volume <name|index>  volume to open (default: first)\n"
		"  --partition <n>            GPT entry index (default: first APFS partition)\n"
		"  -p, --password <pw>        password or recovery key for an encrypted volume\n"
		"  --ask                      prompt for the password\n"
		"  -r                         list recursively\n"
		"  -l                         long listing (inode, mode, size, mtime)\n");
}

struct Options
{
	std::string cmd;
	std::vector<std::string> args;
	std::string volume;
	std::string password;
	bool ask = false;
	bool recursive = false;
	bool long_fmt = false;
	int partition = -1;
};

static void Out(const std::string &s)
{
	fwrite(s.data(), 1, s.size(), stdout);
}

static std::string FormatTime(int64_t ns)
{
	time_t secs = static_cast<time_t>(ns >= 0 ? ns / 1000000000 : -((-ns + 999999999) / 1000000000));
	struct tm tm;
	if (gmtime_s(&tm, &secs) != 0)
		return "?";
	char buf[32];
	strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
	return buf;
}

static std::string FormatEntry(Volume &vol, const std::string &display, const DirEntry &e, bool long_fmt)
{
	std::string line;
	if (long_fmt)
	{
		Stat st;
		char buf[160];
		if (vol.GetStat(e.inode, st))
		{
			snprintf(buf, sizeof(buf), "%10llu %06o %3llu %12llu %s ", static_cast<unsigned long long>(st.inode), st.mode,
				static_cast<unsigned long long>(st.nlink), static_cast<unsigned long long>(st.size), FormatTime(st.mod_ns).c_str());
			if (g_show_flags)
			{
				char fb[48];
				snprintf(fb, sizeof(fb), "bsd=%08x int=%08llx ", st.bsd_flags, static_cast<unsigned long long>(st.internal_flags));
				strcat_s(buf, fb);
			}
		}
		else
			snprintf(buf, sizeof(buf), "%10llu ?????? ? ? ? ", static_cast<unsigned long long>(e.inode));
		line += buf;
	}
	else
	{
		line += FileTypeName(e.type);
		line += '\t';
	}
	line += display;
	if (e.type == FileType::Dir)
		line += '/';
	if (long_fmt && e.type == FileType::Symlink)
	{
		std::string t;
		if (vol.ReadLink(e.inode, t))
			line += " -> " + t;
	}
	line += '\n';
	return line;
}

static bool ListRecursive(Volume &vol, uint64_t dir, const std::string &prefix, bool recursive, bool long_fmt, int depth = 0)
{
	std::vector<DirEntry> ents;
	if (!vol.ListDir(dir, ents))
	{
		fprintf(stderr, "cannot list directory inode %llu\n", static_cast<unsigned long long>(dir));
		return false;
	}
	bool ok = true;
	for (const auto &e : ents)
	{
		std::string path = prefix.empty() ? e.name : prefix + "/" + e.name;
		Out(FormatEntry(vol, recursive ? path : e.name, e, long_fmt));
		if (recursive && e.type == FileType::Dir && depth < 256)
			ok = ListRecursive(vol, e.inode, path, recursive, long_fmt, depth + 1) && ok;
	}
	return ok;
}

static std::unique_ptr<Volume> OpenVolume(Container &c, const Options &o)
{
	if (c.Volumes().empty())
	{
		fprintf(stderr, "container has no volumes\n");
		return nullptr;
	}
	const VolumeInfo *vi = o.volume.empty() ? &c.Volumes().front() : c.FindVolume(o.volume);
	if (!vi)
	{
		fprintf(stderr, "no volume '%s'\n", o.volume.c_str());
		return nullptr;
	}

	std::string pw = o.password;
	std::string err;
	for (int attempt = 0; attempt < 3; attempt++)
	{
		if (vi->encrypted && pw.empty() && o.ask)
		{
			if (!vi->password_hint.empty())
				fprintf(stderr, "Hint: %s\n", vi->password_hint.c_str());
			fprintf(stderr, "Password for %s: ", vi->name.c_str());
			GetPassword(pw);
		}
		std::unique_ptr<Volume> v = c.OpenVolume(vi->fsid, pw, err);
		if (v)
			return v;
		if (!vi->encrypted || !o.ask)
			break;
		fprintf(stderr, "%s\n", err.c_str());
		pw.clear();
	}
	fprintf(stderr, "%s\n", err.c_str());
	return nullptr;
}

static int CmdInfo(const Options &o)
{
	std::string err;
	std::vector<PartitionInfo> parts;
	if (!Container::FindApfsPartitions(o.args[0], parts, err))
	{
		fprintf(stderr, "%s\n", err.c_str());
		return 1;
	}
	if (parts.empty())
		Out("no GPT; treating the whole image as one APFS container\n");
	for (const auto &p : parts)
	{
		char buf[256];
		snprintf(buf, sizeof(buf), "partition %d: offset %llu, size %llu, name \"%s\"\n", p.index,
			static_cast<unsigned long long>(p.offset), static_cast<unsigned long long>(p.size), p.name.c_str());
		Out(buf);
	}

	std::vector<int> which;
	if (parts.empty())
		which.push_back(-1);
	for (const auto &p : parts)
		if (o.partition < 0 || o.partition == p.index)
			which.push_back(p.index);

	int rc = 0;
	for (int part : which)
	{
		std::unique_ptr<Container> c = Container::Open(o.args[0], err, part);
		if (!c)
		{
			fprintf(stderr, "%s\n", err.c_str());
			rc = 1;
			continue;
		}
		char buf[512];
		snprintf(buf, sizeof(buf), "container: block size %u, %llu blocks, %llu free\n", c->BlockSize(),
			static_cast<unsigned long long>(c->BlockCount()), static_cast<unsigned long long>(c->FreeBlocks()));
		Out(buf);
		for (const auto &v : c->Volumes())
		{
			snprintf(buf, sizeof(buf), "  volume %u: \"%s\" uuid %s role 0x%x%s%s%s used %llu bytes\n", v.fsid, v.name.c_str(),
				v.uuid.c_str(), v.role, v.case_sensitive ? " case-sensitive" : " case-insensitive",
				v.encrypted ? " encrypted" : "", v.sealed ? " sealed" : "", static_cast<unsigned long long>(v.bytes_used));
			Out(buf);
			if (!v.password_hint.empty())
				Out("    hint: " + v.password_hint + "\n");
		}
	}
	return rc;
}

static int CmdList(const Options &o)
{
	std::string err;
	std::unique_ptr<Container> c = Container::Open(o.args[0], err, o.partition);
	if (!c)
	{
		fprintf(stderr, "%s\n", err.c_str());
		return 1;
	}
	std::unique_ptr<Volume> vol = OpenVolume(*c, o);
	if (!vol)
		return 1;

	std::string path = o.args.size() > 1 ? o.args[1] : "/";
	uint64_t ino;
	if (!vol->ResolvePath(path, ino))
	{
		fprintf(stderr, "not found: %s\n", path.c_str());
		return 1;
	}
	Stat st;
	if (!vol->GetStat(ino, st))
	{
		fprintf(stderr, "cannot stat %s\n", path.c_str());
		return 1;
	}
	if (st.type != FileType::Dir)
	{
		DirEntry e;
		e.name = path;
		e.inode = ino;
		e.type = st.type;
		Out(FormatEntry(*vol, path, e, o.long_fmt));
		return 0;
	}
	std::string prefix = path;
	while (!prefix.empty() && prefix.back() == '/')
		prefix.pop_back();
	return ListRecursive(*vol, ino, prefix, o.recursive, o.long_fmt) ? 0 : 1;
}

static int CmdCat(const Options &o)
{
	std::string err;
	if (o.args.size() < 2)
	{
		Usage();
		return 2;
	}
	std::unique_ptr<Container> c = Container::Open(o.args[0], err, o.partition);
	if (!c)
	{
		fprintf(stderr, "%s\n", err.c_str());
		return 1;
	}
	std::unique_ptr<Volume> vol = OpenVolume(*c, o);
	if (!vol)
		return 1;

	uint64_t ino;
	Stat st;
	if (!vol->ResolvePath(o.args[1], ino) || !vol->GetStat(ino, st))
	{
		fprintf(stderr, "not found: %s\n", o.args[1].c_str());
		return 1;
	}
	std::unique_ptr<FileReader> r = vol->OpenFile(st, err);
	if (!r)
	{
		fprintf(stderr, "%s: %s\n", o.args[1].c_str(), err.c_str());
		return 1;
	}

	_setmode(_fileno(stdout), _O_BINARY);
	std::vector<uint8_t> buf(1 << 20);
	uint64_t off = 0;
	while (off < r->Size())
	{
		size_t got = 0;
		if (!r->Read(off, buf.data(), buf.size(), got))
		{
			fprintf(stderr, "read error at offset %llu\n", static_cast<unsigned long long>(off));
			return 1;
		}
		if (got == 0)
			break;
		fwrite(buf.data(), 1, got, stdout);
		off += got;
	}
	fflush(stdout);
	return 0;
}

static int CmdXattr(const Options &o)
{
	std::string err;
	if (o.args.size() < 2)
	{
		Usage();
		return 2;
	}
	std::unique_ptr<Container> c = Container::Open(o.args[0], err, o.partition);
	if (!c)
	{
		fprintf(stderr, "%s\n", err.c_str());
		return 1;
	}
	std::unique_ptr<Volume> vol = OpenVolume(*c, o);
	if (!vol)
		return 1;
	uint64_t ino;
	Stat st;
	if (!vol->ResolvePath(o.args[1], ino) || !vol->GetStat(ino, st))
	{
		fprintf(stderr, "not found: %s\n", o.args[1].c_str());
		return 1;
	}
	if (o.args.size() > 2)
	{
		std::vector<uint8_t> data;
		if (!vol->GetXattr(ino, o.args[2], data))
		{
			fprintf(stderr, "no xattr %s\n", o.args[2].c_str());
			return 1;
		}
		_setmode(_fileno(stdout), _O_BINARY);
		fwrite(data.data(), 1, data.size(), stdout);
		return 0;
	}
	std::vector<std::string> names;
	vol->ListXattrs(ino, st, names);
	for (const auto &n : names)
	{
		std::vector<uint8_t> data;
		vol->GetXattr(ino, n, data);
		Out(n + "\t" + std::to_string(data.size()) + "\n");
	}
	return 0;
}

int wmain(int argc, wchar_t **wargv)
{
	SetConsoleOutputCP(CP_UTF8);

	Options o;
	std::vector<std::string> argv;
	for (int i = 1; i < argc; i++)
		argv.push_back(WideToUtf8(wargv[i]));

	for (size_t i = 0; i < argv.size(); i++)
	{
		const std::string &a = argv[i];
		auto next = [&](std::string &dst) {
			if (i + 1 >= argv.size())
				return false;
			dst = argv[++i];
			return true;
		};
		if (a == "-v" || a == "--volume")
		{
			if (!next(o.volume))
				return Usage(), 2;
		}
		else if (a == "-p" || a == "--password")
		{
			if (!next(o.password))
				return Usage(), 2;
		}
		else if (a == "--partition")
		{
			std::string s;
			if (!next(s))
				return Usage(), 2;
			o.partition = atoi(s.c_str());
		}
		else if (a == "--ask")
			o.ask = true;
		else if (a == "-r")
			o.recursive = true;
		else if (a == "-l")
			o.long_fmt = true;
		else if (a == "--flags")
			g_show_flags = o.long_fmt = true;
		else if (a == "-h" || a == "--help")
			return Usage(), 0;
		else if (o.cmd.empty())
			o.cmd = a;
		else
			o.args.push_back(a);
	}

	if (o.cmd.empty() || o.args.empty())
		return Usage(), 2;

	if (o.cmd == "info")
		return CmdInfo(o);
	if (o.cmd == "list" || o.cmd == "ls")
		return CmdList(o);
	if (o.cmd == "cat")
		return CmdCat(o);
	if (o.cmd == "xattr")
		return CmdXattr(o);
	Usage();
	return 2;
}
