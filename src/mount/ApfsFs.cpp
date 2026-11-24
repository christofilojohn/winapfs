// winapfs - read-only APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "ApfsFs.h"

#include <winapfs/Names.h>

#include <sddl.h>

#include <cstdio>
#include <map>

namespace winapfs {

// Fixed security descriptors. macOS owners don't correspond to Windows
// accounts, so like FAT/exFAT there are no per-file ACLs: read-only mounts
// are readable by everyone, writable mounts writable by everyone.
static const wchar_t *kSddlReadOnly = L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FRFX;;;WD)";
static const wchar_t *kSddlWritable = L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;WD)";

constexpr size_t kDirCacheMax = 65536;

// WSL's reparse tags for special files (not in the SDK headers)
constexpr ULONG kTagLxFifo = 0x80000024;
constexpr ULONG kTagLxChr = 0x80000025;
constexpr ULONG kTagLxBlk = 0x80000026;
constexpr ULONG kTagAfUnix = 0x80000023;

struct ApfsFs::Node
{
	Stat st;
	std::string name;           // last path component (APFS bytes)
	std::string stream;         // named stream = this xattr; empty = the file's data
	std::mutex mutex;
	std::shared_ptr<FileReader> reader;   // shared: Read uses it outside the lock
	uint64_t reader_gen = 0;
	PVOID dir_buffer = nullptr;
};

// "\dir\file:stream:$DATA" -> ("\dir\file", "stream"). false if the type isn't $DATA.
static bool SplitStream(const std::wstring &path, std::wstring &base, std::wstring &stream)
{
	size_t slash = path.rfind(L'\\');
	size_t colon = path.find(L':', slash == std::wstring::npos ? 0 : slash);
	base = path;
	stream.clear();
	if (colon == std::wstring::npos)
		return true;
	base = path.substr(0, colon);
	stream = path.substr(colon + 1);
	size_t type = stream.find(L':');
	if (type != std::wstring::npos)
	{
		if (_wcsicmp(stream.c_str() + type + 1, L"$DATA") != 0)
			return false;
		stream.resize(type);
	}
	return true;
}

// FILETIME values that mean "leave unchanged" in SetBasicInfo
static bool TimeGiven(UINT64 ft)
{
	return ft != 0 && ft < 0xFFFFFFFFFFFFFFFEull;
}

static ApfsFs *Self(FSP_FILE_SYSTEM *fs)
{
	return static_cast<ApfsFs *>(fs->UserContext);
}

static uint64_t NsToFileTime(int64_t ns)
{
	// 100ns ticks since 1601-01-01; floor division keeps pre-1970 times right
	int64_t ticks = ns >= 0 ? ns / 100 : -((-ns + 99) / 100);
	int64_t ft = ticks + 116444736000000000LL;
	return ft < 0 ? 0 : static_cast<uint64_t>(ft);
}

// ---------------------------------------------------------------- ApfsFs

ApfsFs::ApfsFs(std::unique_ptr<Volume> vol, const MountOptions &opts) : m_vol(std::move(vol)), m_opts(opts)
{
	ConvertStringSecurityDescriptorToSecurityDescriptorW(m_vol->Writable() ? kSddlWritable : kSddlReadOnly, SDDL_REVISION_1, &m_sd, &m_sd_size);
	VolumeSpace space;
	if (m_vol->GetSpace(space) && space.block_size)
		m_block_size = space.block_size;
}

ApfsFs::~ApfsFs()
{
	Stop();
	if (m_sd)
		LocalFree(m_sd);
}

uint32_t ApfsFs::Attributes(const Stat &st, const std::string &name)
{
	uint32_t a = 0;
	if (st.type == FileType::Dir)
		a |= FILE_ATTRIBUTE_DIRECTORY;
	if (ReparseTag(st))
	{
		a |= FILE_ATTRIBUTE_REPARSE_POINT;
		if (st.type == FileType::Symlink && SymlinkIsDir(st))
			a |= FILE_ATTRIBUTE_DIRECTORY;   // a directory link, as mklink /d makes
	}
	if (st.bsd_flags & kUF_HIDDEN)
		a |= FILE_ATTRIBUTE_HIDDEN;
	if (m_opts.hide_dotfiles && !name.empty() && name[0] == '.')
		a |= FILE_ATTRIBUTE_HIDDEN;
	if (st.bsd_flags & (kUF_IMMUTABLE | kSF_IMMUTABLE))
		a |= FILE_ATTRIBUTE_READONLY;
	// writable mounts: "read-only" is a file without write permission bits
	if (m_vol->Writable() && st.type == FileType::File && !(st.mode & 0222))
		a |= FILE_ATTRIBUTE_READONLY;
	if (a == 0)
		a = FILE_ATTRIBUTE_ARCHIVE;
	return a;
}

uint32_t ApfsFs::ReparseTag(const Stat &st)
{
	switch (st.type)
	{
	case FileType::Symlink: return IO_REPARSE_TAG_SYMLINK;
	case FileType::Fifo: return kTagLxFifo;
	case FileType::CharDev: return kTagLxChr;
	case FileType::BlockDev: return kTagLxBlk;
	case FileType::Socket: return kTagAfUnix;
	default: return 0;
	}
}

// Resolves a symlink target relative to directory dir, following further
// symlinks (at most 8 deep). Targets that leave the volume fail.
// macOS mounts volumes at /Volumes/<name>; absolute targets into this volume
// become volume-rooted ("/x").
std::string ApfsFs::LocalTarget(const std::string &t)
{
	std::string prefix = "/Volumes/" + m_vol->Info().name;
	if (t.compare(0, prefix.size(), prefix) == 0 && (t.size() == prefix.size() || t[prefix.size()] == '/'))
		return t.size() == prefix.size() ? "/" : t.substr(prefix.size());
	return t;
}

bool ApfsFs::FollowLink(uint64_t dir, const std::string &t_in, int depth, Stat &out)
{
	std::string t = LocalTarget(t_in);
	if (depth > 8 || t.empty() || (t.size() >= 2 && t[1] == ':'))
		return false;
	uint64_t cur = t[0] == '/' ? kRootInode : dir;
	Stat cst;
	if (!m_vol->GetStat(cur, cst))
		return false;
	size_t pos = 0;
	while (pos <= t.size())
	{
		size_t next = t.find('/', pos);
		if (next == std::string::npos)
			next = t.size();
		std::string c = t.substr(pos, next - pos);
		pos = next + 1;
		if (c.empty() || c == ".")
			continue;
		if (cst.type != FileType::Dir)
			return false;
		if (c == "..")
		{
			if (cur != kRootInode)
				cur = cst.parent;
			if (!m_vol->GetStat(cur, cst))
				return false;
			continue;
		}
		DirEntry e;
		Stat s;
		if (!m_vol->Lookup(cur, c, e) || !m_vol->GetStat(e.inode, s))
			return false;
		if (s.type == FileType::Symlink)
		{
			std::string t2;
			if (!m_vol->ReadLink(s.inode, t2) || !FollowLink(cur, t2, depth + 1, s))
				return false;
		}
		cur = s.inode;
		cst = s;
	}
	out = cst;
	return true;
}

bool ApfsFs::SymlinkIsDir(const Stat &st)
{
	std::string t;
	Stat target;
	return m_vol->ReadLink(st.inode, t) && FollowLink(st.parent, t, 0, target) && target.type == FileType::Dir;
}

// POSIX target -> Windows. Components are name-mapped like file names;
// "/x" (relative to the volume root on Windows: rooted relative link) keeps
// its leading separator; "C:/x" (a link made on Windows to another drive) is
// absolute again.
void ApfsFs::PosixTargetToWindows(const std::string &target, std::wstring &sub, std::wstring &print, bool &relative)
{
	bool drive = target.size() >= 2 && isalpha(static_cast<unsigned char>(target[0])) && target[1] == ':' &&
		(target.size() == 2 || target[2] == '/');
	std::wstring w;
	size_t pos = 0;
	bool first = true;
	while (pos <= target.size())
	{
		size_t next = target.find('/', pos);
		if (next == std::string::npos)
			next = target.size();
		std::string c = target.substr(pos, next - pos);
		if (!first)
			w.push_back(L'\\');
		if (first && drive)
			w += Utf8ToWide(c);
		else if (c == "." || c == "..")
			w += Utf8ToWide(c);
		else
			w += ApfsNameToWindows(c);
		first = false;
		pos = next + 1;
	}
	relative = !drive;
	print = w;
	sub = drive ? L"\\??\\" + w : w;
}

std::string ApfsFs::WindowsTargetToPosix(const std::wstring &sub_in, bool relative, const std::wstring &link_dir)
{
	auto split = [](const std::wstring &s) {
		std::vector<std::wstring> v;
		size_t pos = 0;
		while (pos <= s.size())
		{
			size_t next = s.find(L'\\', pos);
			if (next == std::wstring::npos)
				next = s.size();
			v.push_back(s.substr(pos, next - pos));
			pos = next + 1;
		}
		return v;
	};
	auto join = [](const std::vector<std::wstring> &comps, size_t from, bool keep_first_raw) {
		std::string out;
		for (size_t i = from; i < comps.size(); i++)
		{
			if (i > from)
				out.push_back('/');
			const std::wstring &c = comps[i];
			out += (c == L"." || c == L".." || c.empty() || (keep_first_raw && i == from)) ? WideToUtf8(c) : WindowsNameToApfs(c);
		}
		return out;
	};
	std::wstring sub = sub_in;
	for (auto &c : sub)
		if (c == L'/')
			c = L'\\';
	if (relative)
		return join(split(sub), 0, false);

	if (sub.compare(0, 4, L"\\??\\") == 0 || sub.compare(0, 4, L"\\\\?\\") == 0)
		sub = sub.substr(4);
	// inside this volume: store it relative to the link, so it works on macOS too
	std::wstring mp = MountPoint();
	if (!mp.empty() && sub.size() >= mp.size() && _wcsnicmp(sub.c_str(), mp.c_str(), mp.size()) == 0 &&
		(sub.size() == mp.size() || sub[mp.size()] == L'\\'))
	{
		std::vector<std::wstring> t, d;
		for (const auto &c : split(sub.substr(mp.size())))
			if (!c.empty())
				t.push_back(c);
		for (const auto &c : split(link_dir))
			if (!c.empty())
				d.push_back(c);
		bool ci = !m_vol->Info().case_sensitive;
		size_t common = 0;
		while (common < t.size() && common < d.size() &&
			(ci ? _wcsicmp(t[common].c_str(), d[common].c_str()) == 0 : t[common] == d[common]))
			common++;
		std::vector<std::wstring> rel;
		for (size_t i = common; i < d.size(); i++)
			rel.push_back(L"..");
		for (size_t i = common; i < t.size(); i++)
			rel.push_back(t[i]);
		return rel.empty() ? "." : join(rel, 0, false);
	}
	if (sub.compare(0, 4, L"UNC\\") == 0)
		return "/" + join(split(sub.substr(3)), 0, false);   // //server/share/...
	return join(split(sub), 0, true);                        // C:/x/y
}

NTSTATUS ApfsFs::MakeReparseData(const Stat &st, PVOID buf, PSIZE_T size)
{
	uint32_t tag = ReparseTag(st);
	if (!tag)
		return STATUS_NOT_A_REPARSE_POINT;
	std::vector<uint8_t> data(REPARSE_DATA_BUFFER_HEADER_SIZE);
	if (tag == IO_REPARSE_TAG_SYMLINK)
	{
		std::string target;
		if (!m_vol->ReadLink(st.inode, target))
			return STATUS_FILE_CORRUPT_ERROR;
		// the substitute name (what Windows follows) has /Volumes/<this
		// volume>/... rooted at the volume; the print name keeps the stored target
		std::wstring sub, print, unused;
		bool relative, unused_rel;
		PosixTargetToWindows(LocalTarget(target), sub, unused, relative);
		PosixTargetToWindows(target, unused, print, unused_rel);
		size_t head = FIELD_OFFSET(REPARSE_DATA_BUFFER, SymbolicLinkReparseBuffer.PathBuffer);
		size_t sub_b = sub.size() * sizeof(WCHAR), print_b = print.size() * sizeof(WCHAR);
		if (head + sub_b + print_b > MAXIMUM_REPARSE_DATA_BUFFER_SIZE)
			return STATUS_IO_REPARSE_DATA_INVALID;
		data.assign(head + sub_b + print_b, 0);
		REPARSE_DATA_BUFFER *r = reinterpret_cast<REPARSE_DATA_BUFFER *>(data.data());
		r->SymbolicLinkReparseBuffer.SubstituteNameOffset = 0;
		r->SymbolicLinkReparseBuffer.SubstituteNameLength = static_cast<USHORT>(sub_b);
		r->SymbolicLinkReparseBuffer.PrintNameOffset = static_cast<USHORT>(sub_b);
		r->SymbolicLinkReparseBuffer.PrintNameLength = static_cast<USHORT>(print_b);
		r->SymbolicLinkReparseBuffer.Flags = relative ? SYMLINK_FLAG_RELATIVE : 0;
		memcpy(r->SymbolicLinkReparseBuffer.PathBuffer, sub.data(), sub_b);
		memcpy(reinterpret_cast<uint8_t *>(r->SymbolicLinkReparseBuffer.PathBuffer) + sub_b, print.data(), print_b);
	}
	REPARSE_DATA_BUFFER *r = reinterpret_cast<REPARSE_DATA_BUFFER *>(data.data());
	r->ReparseTag = tag;
	r->ReparseDataLength = static_cast<USHORT>(data.size() - REPARSE_DATA_BUFFER_HEADER_SIZE);
	if (*size < data.size())
	{
		*size = data.size();
		return STATUS_BUFFER_TOO_SMALL;
	}
	memcpy(buf, data.data(), data.size());
	*size = data.size();
	return STATUS_SUCCESS;
}

bool ApfsFs::StreamVisible(const Stat &st, const std::string &xattr)
{
	std::vector<std::string> names;
	if (!m_vol->ListXattrs(st.inode, st, names))
		return false;
	for (const auto &n : names)
		if (n == xattr)
			return true;
	return false;
}

uint64_t ApfsFs::PendingSize(uint64_t inode)
{
	std::lock_guard<std::mutex> lock(m_pending_mutex);
	auto it = m_pending_size.find(inode);
	return it == m_pending_size.end() ? 0 : it->second;
}

void ApfsFs::SetPendingSize(uint64_t inode, uint64_t size)
{
	std::lock_guard<std::mutex> lock(m_pending_mutex);
	if (size)
		m_pending_size[inode] = size;
	else
		m_pending_size.erase(inode);
}

void ApfsFs::ClearPendingSizeIfAtMost(uint64_t inode, uint64_t reached)
{
	std::lock_guard<std::mutex> lock(m_pending_mutex);
	auto it = m_pending_size.find(inode);
	if (it != m_pending_size.end() && it->second <= reached)
		m_pending_size.erase(it);
}

uint64_t ApfsFs::EffectiveSize(const Stat &st)
{
	if (st.type != FileType::File)
		return st.size;
	return std::max<uint64_t>(st.size, PendingSize(st.inode));
}

// Makes a pending size real (if the writes didn't get there) and forgets it.
int ApfsFs::ApplyPendingSize(uint64_t inode)
{
	uint64_t want = PendingSize(inode);
	if (!want)
		return 0;
	Stat st;
	if (!m_vol->GetStat(inode, st) || st.size >= want)
	{
		ClearPendingSizeIfAtMost(inode, want);
		return 0;
	}
	int err = m_vol->SetSize(inode, want);
	if (!err)
		ClearPendingSizeIfAtMost(inode, want);
	return err;
}

void ApfsFs::ApplyAllPendingSizes()
{
	std::vector<uint64_t> inodes;
	{
		std::lock_guard<std::mutex> lock(m_pending_mutex);
		for (const auto &kv : m_pending_size)
			inodes.push_back(kv.first);
	}
	for (uint64_t ino : inodes)
		ApplyPendingSize(ino);
}

void ApfsFs::FillFileInfo(const Stat &st, const std::string &name, FSP_FSCTL_FILE_INFO &fi)
{
	uint64_t bs = m_block_size;
	memset(&fi, 0, sizeof(fi));
	fi.FileAttributes = Attributes(st, name);
	fi.ReparseTag = ReparseTag(st);
	if (st.type == FileType::File)
	{
		fi.FileSize = EffectiveSize(st);
		fi.AllocationSize = st.compressed ? (st.size + bs - 1) / bs * bs : st.alloc_size;
		if (fi.FileSize > st.size && fi.AllocationSize < fi.FileSize)
			fi.AllocationSize = (fi.FileSize + bs - 1) / bs * bs;   // pending growth
	}
	fi.CreationTime = NsToFileTime(st.create_ns);
	fi.LastAccessTime = NsToFileTime(st.access_ns);
	fi.LastWriteTime = NsToFileTime(st.mod_ns);
	fi.ChangeTime = NsToFileTime(st.change_ns);
	fi.IndexNumber = st.inode;
	fi.HardLinks = 0;
	fi.EaSize = 0;
}

NTSTATUS ApfsFs::CopySecurity(PSECURITY_DESCRIPTOR sd, SIZE_T *size)
{
	if (!size)
		return STATUS_SUCCESS;
	if (*size < m_sd_size)
	{
		*size = m_sd_size;
		return STATUS_BUFFER_OVERFLOW;
	}
	*size = m_sd_size;
	if (sd)
		memcpy(sd, m_sd, m_sd_size);
	return STATUS_SUCCESS;
}

bool ApfsFs::CacheGet(const std::wstring &dir, uint64_t &inode, std::wstring &normalized)
{
	std::lock_guard<std::mutex> lock(m_cache_mutex);
	auto it = m_dir_cache.find(dir);
	if (it == m_dir_cache.end())
		return false;
	inode = it->second.first;
	normalized = it->second.second;
	return true;
}

void ApfsFs::CachePut(const std::wstring &dir, uint64_t inode, const std::wstring &normalized, uint64_t gen)
{
	std::lock_guard<std::mutex> lock(m_cache_mutex);
	if (gen != m_cache_gen)
		return;
	if (m_dir_cache.size() >= kDirCacheMax)
		m_dir_cache.clear();
	m_dir_cache.emplace(dir, std::make_pair(inode, normalized));
}

NTSTATUS ApfsFs::Resolve(const wchar_t *path, Stat &st, std::wstring *normalized, std::string *stream)
{
	std::wstring p = path ? path : L"";
	std::string stream_name;

	// "\dir\file:stream:$DATA" -> stream. Mapped names never contain ':'.
	size_t colon = p.find(L':');
	if (colon != std::wstring::npos)
	{
		std::wstring s = p.substr(colon + 1);
		p.resize(colon);
		size_t type = s.find(L':');
		if (type != std::wstring::npos)
		{
			std::wstring t = s.substr(type + 1);
			s.resize(type);
			if (_wcsicmp(t.c_str(), L"$DATA") != 0)
				return STATUS_OBJECT_NAME_INVALID;
		}
		stream_name = WindowsNameToApfs(s);
	}
	if (stream)
		*stream = stream_name;
	else if (!stream_name.empty())
		return STATUS_OBJECT_NAME_NOT_FOUND;

	// split into components
	std::vector<std::wstring> comps;
	size_t pos = 0;
	while (pos < p.size())
	{
		size_t next = p.find(L'\\', pos);
		if (next == std::wstring::npos)
			next = p.size();
		if (next > pos)
			comps.push_back(p.substr(pos, next - pos));
		pos = next + 1;
	}

	uint64_t cur = kRootInode;
	std::wstring norm;
	size_t first = 0;
	uint64_t cache_gen;
	{
		std::lock_guard<std::mutex> lock(m_cache_mutex);
		cache_gen = m_cache_gen;
	}

	// longest cached directory prefix
	for (size_t k = comps.size(); k > 0; k--)
	{
		std::wstring key;
		for (size_t i = 0; i < k; i++)
			key += L"\\" + comps[i];
		uint64_t ino;
		std::wstring n;
		if (CacheGet(key, ino, n))
		{
			cur = ino;
			norm = n;
			first = k;
			break;
		}
	}

	std::wstring key = L"";
	for (size_t i = 0; i < first; i++)
		key += L"\\" + comps[i];

	for (size_t i = first; i < comps.size(); i++)
	{
		DirEntry e;
		if (!m_vol->Lookup(cur, WindowsNameToApfs(comps[i]), e))
			return i + 1 == comps.size() ? STATUS_OBJECT_NAME_NOT_FOUND : STATUS_OBJECT_PATH_NOT_FOUND;
		norm += L"\\" + ApfsNameToWindows(e.name);
		key += L"\\" + comps[i];
		if (i + 1 < comps.size())
		{
			if (e.type != FileType::Dir)
				return STATUS_OBJECT_PATH_NOT_FOUND;
			CachePut(key, e.inode, norm, cache_gen);
		}
		cur = e.inode;
	}

	if (!m_vol->GetStat(cur, st))
		return STATUS_FILE_CORRUPT_ERROR;
	if (normalized)
		*normalized = norm.empty() ? L"\\" : norm;
	if (stream && !stream_name.empty() && !StreamVisible(st, stream_name))
		return STATUS_OBJECT_NAME_NOT_FOUND;
	return STATUS_SUCCESS;
}

NTSTATUS ApfsFs::ResolveParent(const wchar_t *path, uint64_t &parent, std::string &name, std::wstring *parent_normalized)
{
	std::wstring p = path ? path : L"";
	while (p.size() > 1 && p.back() == L'\\')
		p.pop_back();
	size_t slash = p.rfind(L'\\');
	if (slash == std::wstring::npos || slash + 1 >= p.size())
		return STATUS_OBJECT_NAME_INVALID;
	std::wstring last = p.substr(slash + 1);
	if (last.find(L':') != std::wstring::npos)
		return STATUS_OBJECT_NAME_INVALID;   // no named streams yet
	Stat st;
	std::wstring norm;
	NTSTATUS r = Resolve(slash == 0 ? L"\\" : p.substr(0, slash).c_str(), st, &norm, nullptr);
	if (!NT_SUCCESS(r))
		return r == STATUS_OBJECT_NAME_NOT_FOUND ? STATUS_OBJECT_PATH_NOT_FOUND : r;
	if (st.type != FileType::Dir)
		return STATUS_OBJECT_PATH_NOT_FOUND;
	parent = st.inode;
	name = WindowsNameToApfs(last);
	if (last.size() > 255 || name.size() > 255)
		return STATUS_OBJECT_NAME_INVALID;   // APFS: 255 UTF-8 bytes
	if (parent_normalized)
		*parent_normalized = norm == L"\\" ? L"" : norm;
	return STATUS_SUCCESS;
}

void ApfsFs::InvalidateCache()
{
	std::lock_guard<std::mutex> lock(m_cache_mutex);
	m_cache_gen++;
	m_dir_cache.clear();
}

// errno from the engine -> NTSTATUS
static NTSTATUS Errno(int err)
{
	switch (-err)
	{
	case 0: return STATUS_SUCCESS;
	case 1:  /* EPERM */
	case 13: /* EACCES */ return STATUS_ACCESS_DENIED;
	case 2:  /* ENOENT */ return STATUS_OBJECT_NAME_NOT_FOUND;
	case 17: /* EEXIST */ return STATUS_OBJECT_NAME_COLLISION;
	case 20: /* ENOTDIR */ return STATUS_NOT_A_DIRECTORY;
	case 21: /* EISDIR */ return STATUS_FILE_IS_A_DIRECTORY;
	case 22: /* EINVAL */ return STATUS_INVALID_PARAMETER;
	case 27: /* EFBIG */
	case 28: /* ENOSPC */ return STATUS_DISK_FULL;
	case 30: /* EROFS */ return STATUS_MEDIA_WRITE_PROTECTED;
	case 36: /* ENAMETOOLONG */ return STATUS_OBJECT_NAME_INVALID;
	case 39: /* ENOTEMPTY */ return STATUS_DIRECTORY_NOT_EMPTY;
	case 95: /* EOPNOTSUPP */ return STATUS_NOT_SUPPORTED;
	case 12: /* ENOMEM */ return STATUS_INSUFFICIENT_RESOURCES;
	default: return STATUS_UNEXPECTED_IO_ERROR;
	}
}

static int64_t FileTimeToNs(uint64_t ft)
{
	return (static_cast<int64_t>(ft) - 116444736000000000LL) * 100;
}

// ---------------------------------------------------------------- WinFsp callbacks

static NTSTATUS GetVolumeInfo(FSP_FILE_SYSTEM *fs, FSP_FSCTL_VOLUME_INFO *vi)
{
	ApfsFs *self = Self(fs);
	const VolumeInfo &info = self->Vol().Info();
	VolumeSpace space;

	self->Vol().GetSpace(space);
	vi->TotalSize = space.total_bytes;
	vi->FreeSize = space.free_bytes;

	std::wstring label = Utf8ToWide(info.name);
	if (label.size() > 32)
		label.resize(32);
	memcpy(vi->VolumeLabel, label.data(), label.size() * sizeof(wchar_t));
	vi->VolumeLabelLength = static_cast<UINT16>(label.size() * sizeof(wchar_t));
	return STATUS_SUCCESS;
}

static NTSTATUS GetReparsePointByName(FSP_FILE_SYSTEM *fs, PVOID, PWSTR name, BOOLEAN is_dir, PVOID buf, PSIZE_T size)
{
	ApfsFs *self = Self(fs);
	Stat st;
	NTSTATUS r = self->Resolve(name, st, nullptr, nullptr);
	if (!NT_SUCCESS(r))
		return r;
	if (!ApfsFs::ReparseTag(st))
		return STATUS_NOT_A_REPARSE_POINT;
	if (!buf)
		return STATUS_SUCCESS;
	return self->MakeReparseData(st, buf, size);
}

static NTSTATUS GetSecurityByName(FSP_FILE_SYSTEM *fs, PWSTR name, PUINT32 attrs, PSECURITY_DESCRIPTOR sd, SIZE_T *sd_size)
{
	ApfsFs *self = Self(fs);
	Stat st;
	std::wstring norm;
	std::string stream;
	NTSTATUS r = self->Resolve(name, st, &norm, &stream);
	if (r == STATUS_OBJECT_PATH_NOT_FOUND || r == STATUS_OBJECT_NAME_NOT_FOUND)
	{
		// a symlink in the middle of the path: let WinFsp resolve it
		if (FspFileSystemFindReparsePoint(fs, GetReparsePointByName, nullptr, name, attrs))
			return STATUS_REPARSE;
	}
	if (!NT_SUCCESS(r))
		return r;
	if (attrs)
	{
		size_t slash = norm.rfind(L'\\');
		*attrs = self->Attributes(st, WindowsNameToApfs(norm.substr(slash + 1)));
	}
	return self->CopySecurity(sd, sd_size);
}

// File info from copied/locked node state; a named stream reports the xattr's size.
static void FillNodeInfoLocked(ApfsFs *self, const Stat &st, const std::string &name, const std::string &stream, FSP_FSCTL_FILE_INFO &fi)
{
	self->FillFileInfo(st, name, fi);
	if (stream.empty())
		return;
	uint64_t size = 0, bs = 4096;
	self->Vol().GetXattrSize(st.inode, stream, size);
	fi.FileAttributes &= ~(FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT);
	if (!fi.FileAttributes)
		fi.FileAttributes = FILE_ATTRIBUTE_ARCHIVE;
	fi.ReparseTag = 0;
	fi.FileSize = size;
	fi.AllocationSize = (size + bs - 1) / bs * bs;
}

static void SnapshotNode(ApfsFs::Node *node, Stat &st, std::string &name)
{
	std::lock_guard<std::mutex> lock(node->mutex);
	st = node->st;
	name = node->name;
}

// File info of an open node; a named stream reports the xattr's size.
static void FillNodeInfo(ApfsFs *self, ApfsFs::Node *node, FSP_FSCTL_FILE_INFO &fi)
{
	Stat st;
	std::string name, stream;
	{
		std::lock_guard<std::mutex> lock(node->mutex);
		st = node->st;
		name = node->name;
		stream = node->stream;
	}
	FillNodeInfoLocked(self, st, name, stream, fi);
}

static NTSTATUS Open(FSP_FILE_SYSTEM *fs, PWSTR name, UINT32 create_options, UINT32 granted_access, PVOID *ctx, FSP_FSCTL_FILE_INFO *fi)
{
	ApfsFs *self = Self(fs);
	std::unique_ptr<ApfsFs::Node> node(new ApfsFs::Node());
	std::wstring norm;

	NTSTATUS r = self->Resolve(name, node->st, &norm, &node->stream);
	if (!NT_SUCCESS(r))
		return r;

	bool is_dir = node->st.type == FileType::Dir && node->stream.empty();
	if ((create_options & FILE_DIRECTORY_FILE) && !is_dir)
		return STATUS_NOT_A_DIRECTORY;
	if ((create_options & FILE_NON_DIRECTORY_FILE) && is_dir)
		return STATUS_FILE_IS_A_DIRECTORY;

	size_t slash = norm.rfind(L'\\');
	node->name = WindowsNameToApfs(norm.substr(slash + 1));
	FillNodeInfo(self, node.get(), *fi);
	if (!node->stream.empty())
		norm += L":" + ApfsNameToWindows(node->stream);

	FSP_FSCTL_OPEN_FILE_INFO *ofi = FspFileSystemGetOpenFileInfo(fi);
	size_t bytes = norm.size() * sizeof(wchar_t);
	if (ofi->NormalizedName && bytes <= ofi->NormalizedNameSize)
	{
		memcpy(ofi->NormalizedName, norm.data(), bytes);
		ofi->NormalizedNameSize = static_cast<UINT16>(bytes);
	}

	*ctx = node.release();
	return STATUS_SUCCESS;
}

// ---------------------------------------------------------------- writing
// (the create dispatcher needs Create/Overwrite even on read-only volumes)

static void Refresh(ApfsFs *self, ApfsFs::Node *node)
{
	uint64_t ino;
	{
		std::lock_guard<std::mutex> lock(node->mutex);
		ino = node->st.inode;
	}
	Stat st;
	if (self->Vol().GetStat(ino, st))
	{
		std::lock_guard<std::mutex> lock(node->mutex);
		node->st = st;
	}
}

static uint32_t ModeForAttributes(uint32_t mode, uint32_t attrs)
{
	mode &= 07777;
	if (attrs & FILE_ATTRIBUTE_READONLY)
		return mode & ~0222u;
	if (!(mode & 0222))
		return mode | 0200;
	return mode;
}

// UF_HIDDEN follows FILE_ATTRIBUTE_HIDDEN (unless --hide-dotfiles already shows it)
static int SetHidden(ApfsFs *self, const Stat &st, const std::string &name, uint32_t attrs)
{
	bool want = (attrs & FILE_ATTRIBUTE_HIDDEN) != 0;
	bool have = (self->Attributes(st, name) & FILE_ATTRIBUTE_HIDDEN) != 0;
	if (want == have)
		return 0;
	return self->Vol().SetFlags(st.inode, want ? kUF_HIDDEN : 0, kUF_HIDDEN);
}

static void SetNormalizedName(FSP_FSCTL_FILE_INFO *fi, const std::wstring &norm)
{
	FSP_FSCTL_OPEN_FILE_INFO *ofi = FspFileSystemGetOpenFileInfo(fi);
	size_t bytes = norm.size() * sizeof(wchar_t);
	if (ofi->NormalizedName && bytes <= ofi->NormalizedNameSize)
	{
		memcpy(ofi->NormalizedName, norm.data(), bytes);
		ofi->NormalizedNameSize = static_cast<UINT16>(bytes);
	}
}

// "file:stream": a new xattr. Like NTFS, the file is created too if needed.
static NTSTATUS CreateStream(ApfsFs *self, const std::wstring &base, const std::wstring &wstream, PVOID *ctx, FSP_FSCTL_FILE_INFO *fi)
{
	std::unique_ptr<ApfsFs::Node> node(new ApfsFs::Node());
	std::wstring norm;
	NTSTATUS r = self->Resolve(base.c_str(), node->st, &norm, nullptr);
	if (r == STATUS_OBJECT_NAME_NOT_FOUND)
	{
		uint64_t parent;
		std::string aname;
		std::wstring pnorm;
		r = self->ResolveParent(base.c_str(), parent, aname, &pnorm);
		if (!NT_SUCCESS(r))
			return r;
		DirEntry e;
		int err = self->Vol().Create(parent, aname, false, 0644, e);
		if (err)
			return Errno(err);
		if (!self->Vol().GetStat(e.inode, node->st))
			return STATUS_FILE_CORRUPT_ERROR;
		norm = pnorm + L"\\" + ApfsNameToWindows(aname);
	}
	else if (!NT_SUCCESS(r))
		return r;
	node->name = WindowsNameToApfs(norm.substr(norm.rfind(L'\\') + 1));
	node->stream = WindowsNameToApfs(wstream);
	int err = self->Vol().SetXattr(node->st.inode, node->stream, "", 0, kXattrCreate);
	if (err)
		return Errno(err);
	FillNodeInfo(self, node.get(), *fi);
	SetNormalizedName(fi, norm + L":" + wstream);
	*ctx = node.release();
	return STATUS_SUCCESS;
}

static NTSTATUS Create(FSP_FILE_SYSTEM *fs, PWSTR name, UINT32 create_options, UINT32 granted_access, UINT32 attrs,
	PSECURITY_DESCRIPTOR sd, UINT64 alloc, PVOID *ctx, FSP_FSCTL_FILE_INFO *fi)
{
	ApfsFs *self = Self(fs);
	if (!self->Vol().Writable())
		return STATUS_MEDIA_WRITE_PROTECTED;

	std::wstring base, wstream;
	if (!SplitStream(name, base, wstream))
		return STATUS_OBJECT_NAME_INVALID;
	if (!wstream.empty())
		return CreateStream(self, base, wstream, ctx, fi);

	uint64_t parent;
	std::string aname;
	std::wstring pnorm;
	NTSTATUS r = self->ResolveParent(base.c_str(), parent, aname, &pnorm);
	if (!NT_SUCCESS(r))
		return r;
	bool dir = (create_options & FILE_DIRECTORY_FILE) != 0;
	uint32_t mode = dir ? 0755 : ((attrs & FILE_ATTRIBUTE_READONLY) ? 0444 : 0644);
	DirEntry e;
	int err = self->Vol().Create(parent, aname, dir, mode, e);
	if (err)
		return Errno(err);

	std::unique_ptr<ApfsFs::Node> node(new ApfsFs::Node());
	if (!self->Vol().GetStat(e.inode, node->st))
		return STATUS_FILE_CORRUPT_ERROR;
	node->name = aname;
	if (attrs & FILE_ATTRIBUTE_HIDDEN)
	{
		SetHidden(self, node->st, node->name, attrs);
		Refresh(self, node.get());
	}
	FillNodeInfoLocked(self, node->st, node->name, node->stream, *fi);
	SetNormalizedName(fi, pnorm + L"\\" + ApfsNameToWindows(aname));
	*ctx = node.release();
	return STATUS_SUCCESS;
}

static NTSTATUS Overwrite(FSP_FILE_SYSTEM *fs, PVOID ctx, UINT32 attrs, BOOLEAN replace, UINT64 alloc, FSP_FSCTL_FILE_INFO *fi)
{
	ApfsFs *self = Self(fs);
	ApfsFs::Node *node = static_cast<ApfsFs::Node *>(ctx);
	if (!self->Vol().Writable())
		return STATUS_MEDIA_WRITE_PROTECTED;
	int err;
	if (!node->stream.empty())
	{
		err = self->Vol().SetXattr(node->st.inode, node->stream, "", 0, 0);
		FillNodeInfo(self, node, *fi);
		return Errno(err);
	}
	self->SetPendingSize(node->st.inode, 0);
	err = self->Vol().SetSize(node->st.inode, 0);
	Refresh(self, node);   // a compressed file is now a new inode
	if (!err)
	{
		Stat st;
		std::string name;
		SnapshotNode(node, st, name);
		uint32_t all = replace ? attrs : (attrs | self->Attributes(st, name));
		uint32_t mode = ModeForAttributes(st.mode, all);
		if (mode != (st.mode & 07777u))
			err = self->Vol().SetMode(st.inode, mode);
		if (!err)
			err = SetHidden(self, st, name, all);
	}
	Refresh(self, node);
	FillNodeInfo(self, node, *fi);
	return Errno(err);
}

// Named stream writes: the whole xattr is read, patched and written back.
static NTSTATUS WriteStream(ApfsFs *self, ApfsFs::Node *node, PVOID buf, UINT64 off, ULONG len, BOOLEAN to_eof, BOOLEAN constrained,
	PULONG transferred, FSP_FSCTL_FILE_INFO *fi)
{
	std::lock_guard<std::mutex> lock(node->mutex);
	std::vector<uint8_t> data;
	if (!self->Vol().GetXattr(node->st.inode, node->stream, data))
		return STATUS_OBJECT_NAME_NOT_FOUND;
	if (to_eof)
		off = data.size();
	if (constrained)
	{
		if (off >= data.size())
		{
			FillNodeInfoLocked(self, node->st, node->name, node->stream, *fi);
			return STATUS_SUCCESS;
		}
		len = static_cast<ULONG>(std::min<uint64_t>(len, data.size() - off));
	}
	if (off + len > (256ull << 20))
		return STATUS_DISK_FULL;   // the engine's xattr size limit
	if (off + len > data.size())
		data.resize(static_cast<size_t>(off + len));
	memcpy(data.data() + off, buf, len);
	int err = self->Vol().SetXattr(node->st.inode, node->stream, data.data(), data.size(), 0);
	if (!err)
		*transferred = len;
	FillNodeInfoLocked(self, node->st, node->name, node->stream, *fi);
	return Errno(err);
}

static NTSTATUS Write(FSP_FILE_SYSTEM *fs, PVOID ctx, PVOID buf, UINT64 off, ULONG len, BOOLEAN to_eof, BOOLEAN constrained,
	PULONG transferred, FSP_FSCTL_FILE_INFO *fi)
{
	ApfsFs *self = Self(fs);
	ApfsFs::Node *node = static_cast<ApfsFs::Node *>(ctx);
	*transferred = 0;
	if (!node->stream.empty())
		return WriteStream(self, node, buf, off, len, to_eof, constrained, transferred, fi);
	Refresh(self, node);
	Stat st;
	std::string name;
	SnapshotNode(node, st, name);
	if (st.type != FileType::File)
		return STATUS_INVALID_DEVICE_REQUEST;
	uint64_t size = self->EffectiveSize(st);   // includes a pending growth
	if (to_eof)
		off = size;
	if (constrained)
	{
		// paging I/O: never extends the file
		if (off >= size)
		{
			self->FillFileInfo(st, name, *fi);
			return STATUS_SUCCESS;
		}
		len = static_cast<ULONG>(std::min<uint64_t>(len, size - off));
	}
	size_t written = 0;
	int err = self->Vol().Write(st.inode, off, buf, len, written);
	*transferred = static_cast<ULONG>(written);
	if (written)
		self->AddWriteBytes(written);
	if (written)
		self->ClearPendingSizeIfAtMost(st.inode, off + written);   // cleared only up to what was written
	{
		std::lock_guard<std::mutex> lock(node->mutex);
		node->reader.reset();   // a compressed file was replaced by a plain copy
	}
	Refresh(self, node);
	FillNodeInfo(self, node, *fi);
	if (err == -5 || err == -28)   /* -EIO / -ENOSPC: never mask fatal I/O or disk-full errors behind partial writes */
		return Errno(err);
	return written ? STATUS_SUCCESS : Errno(err);
}

static NTSTATUS SetFileSize(FSP_FILE_SYSTEM *fs, PVOID ctx, UINT64 size, BOOLEAN set_alloc, FSP_FSCTL_FILE_INFO *fi)
{
	ApfsFs *self = Self(fs);
	ApfsFs::Node *node = static_cast<ApfsFs::Node *>(ctx);
	int err = 0;
	if (!node->stream.empty())
	{
		std::lock_guard<std::mutex> lock(node->mutex);
		std::vector<uint8_t> data;
		if (!self->Vol().GetXattr(node->st.inode, node->stream, data))
			return STATUS_OBJECT_NAME_NOT_FOUND;
		if ((!set_alloc || size < data.size()) && size != data.size())
		{
			if (size > (256ull << 20))
				return STATUS_DISK_FULL;
			data.resize(static_cast<size_t>(size));
			err = self->Vol().SetXattr(node->st.inode, node->stream, data.data(), data.size(), 0);
		}
		FillNodeInfoLocked(self, node->st, node->name, node->stream, *fi);
		return Errno(err);
	}
	Refresh(self, node);
	Stat st;
	std::string name;
	SnapshotNode(node, st, name);
	uint64_t cur = self->EffectiveSize(st);
	if (set_alloc)
	{
		// an allocation size below the file size truncates (NTFS semantics);
		// otherwise allocation requests are hints the engine doesn't need
		if (size < cur)
		{
			self->SetPendingSize(st.inode, size > st.size ? size : 0);
			if (size < st.size)
				err = self->Vol().SetSize(st.inode, size);
		}
	}
	else if (size > st.size && !st.compressed)
	{
		// growth: reported now, written by the data or applied at cleanup
		self->SetPendingSize(st.inode, size);
	}
	else
	{
		self->SetPendingSize(st.inode, 0);
		if (size != st.size)
			err = self->Vol().SetSize(st.inode, size);
	}
	{
		std::lock_guard<std::mutex> lock(node->mutex);
		node->reader.reset();
	}
	Refresh(self, node);
	FillNodeInfo(self, node, *fi);
	return Errno(err);
}

static NTSTATUS SetBasicInfo(FSP_FILE_SYSTEM *fs, PVOID ctx, UINT32 attrs, UINT64 ctime, UINT64 atime, UINT64 mtime, UINT64 chtime,
	FSP_FSCTL_FILE_INFO *fi)
{
	ApfsFs *self = Self(fs);
	ApfsFs::Node *node = static_cast<ApfsFs::Node *>(ctx);
	int err = 0;
	Refresh(self, node);
	Stat st;
	std::string name;
	SnapshotNode(node, st, name);
	if (attrs != INVALID_FILE_ATTRIBUTES)
	{
		if (st.type == FileType::File || st.type == FileType::Dir)
		{
			uint32_t mode = ModeForAttributes(st.mode, attrs);
			if (st.type == FileType::File && mode != (st.mode & 07777u))
				err = self->Vol().SetMode(st.inode, mode);
		}
		if (!err)
			err = SetHidden(self, st, name, attrs);
	}
	if (!err && TimeGiven(ctime))
		err = self->Vol().SetCreateTime(st.inode, FileTimeToNs(ctime));
	if (!err && (TimeGiven(atime) || TimeGiven(mtime)))
		err = self->Vol().SetTimes(st.inode, TimeGiven(atime) ? FileTimeToNs(atime) : INT64_MIN,
			TimeGiven(mtime) ? FileTimeToNs(mtime) : INT64_MIN);
	Refresh(self, node);
	FillNodeInfo(self, node, *fi);
	return Errno(err);
}

static NTSTATUS CanDelete(FSP_FILE_SYSTEM *fs, PVOID ctx, PWSTR name)
{
	ApfsFs *self = Self(fs);
	ApfsFs::Node *node = static_cast<ApfsFs::Node *>(ctx);
	if (!self->Vol().Writable())
		return STATUS_MEDIA_WRITE_PROTECTED;
	if (node->st.type == FileType::Dir && node->stream.empty())
	{
		std::vector<DirEntry> ents;
		if (!self->Vol().ListDir(node->st.inode, ents))
			return STATUS_FILE_CORRUPT_ERROR;
		if (!ents.empty())
			return STATUS_DIRECTORY_NOT_EMPTY;
	}
	return STATUS_SUCCESS;
}

static VOID Cleanup(FSP_FILE_SYSTEM *fs, PVOID ctx, PWSTR name, ULONG flags)
{
	ApfsFs *self = Self(fs);
	ApfsFs::Node *node = static_cast<ApfsFs::Node *>(ctx);
	if (node->stream.empty() && node->st.type == FileType::File)
	{
		if (flags & FspCleanupDelete)
			self->SetPendingSize(node->st.inode, 0);
		else if (self->ApplyPendingSize(node->st.inode) && self->Options().debug)
			fprintf(stderr, "set size of %ls failed\n", name ? name : L"?");
	}
	if (!(flags & FspCleanupDelete) || !name)
		return;
	if (!node->stream.empty())
	{
		int err = self->Vol().RemoveXattr(node->st.inode, node->stream);
		if (err && self->Options().debug)
			fprintf(stderr, "delete stream %ls: errno %d\n", name, -err);
		return;
	}
	uint64_t parent;
	std::string aname;
	if (!NT_SUCCESS(self->ResolveParent(name, parent, aname, nullptr)))
		return;
	int err = self->Vol().Remove(parent, aname, node->st.type == FileType::Dir);
	if (err && self->Options().debug)
		fprintf(stderr, "delete %ls: errno %d\n", name, -err);
	self->InvalidateCache();
}

static NTSTATUS Rename(FSP_FILE_SYSTEM *fs, PVOID ctx, PWSTR name, PWSTR new_name, BOOLEAN replace)
{
	ApfsFs *self = Self(fs);
	ApfsFs::Node *node = static_cast<ApfsFs::Node *>(ctx);
	if (!node->stream.empty())
		return STATUS_NOT_SUPPORTED;   // renaming a stream
	uint64_t op = 0, np = 0;
	std::string on, nn;
	NTSTATUS r = self->ResolveParent(name, op, on, nullptr);
	if (NT_SUCCESS(r))
		r = self->ResolveParent(new_name, np, nn, nullptr);
	if (!NT_SUCCESS(r))
		return r;
	int err = self->Vol().Rename(op, on, np, nn, replace != FALSE);
	if (err == -21 || err == -20)   /* EISDIR/ENOTDIR: replacing across file and directory */
		return STATUS_ACCESS_DENIED;
	if (!err)
	{
		std::lock_guard<std::mutex> lock(node->mutex);
		node->name = nn;
	}
	self->InvalidateCache();
	return Errno(err);
}

static NTSTATUS Flush(FSP_FILE_SYSTEM *fs, PVOID ctx, FSP_FSCTL_FILE_INFO *fi)
{
	ApfsFs *self = Self(fs);
	int err = 0;
	if (ctx && self->Vol().Writable())
		err = self->ApplyPendingSize(static_cast<ApfsFs::Node *>(ctx)->st.inode);
	else if (self->Vol().Writable())
		self->ApplyAllPendingSizes();   // volume flush
	if (!err && self->Vol().Writable())
		err = self->Vol().Sync();
	if (ctx)
	{
		ApfsFs::Node *node = static_cast<ApfsFs::Node *>(ctx);
		Refresh(self, node);
		FillNodeInfo(self, node, *fi);
	}
	return Errno(err);
}

static VOID Close(FSP_FILE_SYSTEM *fs, PVOID ctx)
{
	ApfsFs::Node *node = static_cast<ApfsFs::Node *>(ctx);
	FspFileSystemDeleteDirectoryBuffer(&node->dir_buffer);
	delete node;
}

static NTSTATUS Read(FSP_FILE_SYSTEM *fs, PVOID ctx, PVOID buf, UINT64 off, ULONG len, PULONG transferred)
{
	ApfsFs *self = Self(fs);
	ApfsFs::Node *node = static_cast<ApfsFs::Node *>(ctx);
	*transferred = 0;

	if (!node->stream.empty())
	{
		uint64_t size = 0;
		size_t got = 0;
		if (!self->Vol().GetXattrSize(node->st.inode, node->stream, size))
			return STATUS_OBJECT_NAME_NOT_FOUND;
		if (off >= size)
			return STATUS_END_OF_FILE;
		if (!self->Vol().ReadXattr(node->st.inode, node->stream, off, buf, len, got))
			return STATUS_FILE_CORRUPT_ERROR;
		*transferred = static_cast<ULONG>(got);
		return STATUS_SUCCESS;
	}
	if (node->st.type != FileType::File)
		return STATUS_INVALID_DEVICE_REQUEST;

	std::shared_ptr<FileReader> reader;
	uint64_t ino = 0;
	{
		std::lock_guard<std::mutex> lock(node->mutex);
		uint64_t gen = self->Vol().DecompressGen();
		if (node->reader && node->reader_gen != gen)
		{
			node->reader.reset();
			Stat updated;
			if (self->Vol().GetStat(node->st.inode, updated))
				node->st = updated;
		}
		if (!node->reader)
		{
			std::string err;
			node->reader = self->Vol().OpenFile(node->st, err);
			if (!node->reader)
			{
				if (self->Options().debug)
					fprintf(stderr, "open inode %llu: %s\n", static_cast<unsigned long long>(node->st.inode), err.c_str());
				return STATUS_FILE_CORRUPT_ERROR;
			}
			node->reader_gen = gen;
		}
		reader = node->reader;
		ino = node->st.inode;
	}

	uint64_t size = std::max<uint64_t>(reader->Size(), self->PendingSize(ino));
	if (off >= size)
		return STATUS_END_OF_FILE;

	size_t got = 0;
	if (off < reader->Size() && !reader->Read(off, buf, len, got))
		return STATUS_FILE_CORRUPT_ERROR;
	// a pending growth (see SetFileSize) reads as zeros
	uint64_t want = std::min<uint64_t>(len, size - off);
	if (got < want)
	{
		memset(static_cast<uint8_t *>(buf) + got, 0, static_cast<size_t>(want - got));
		got = static_cast<size_t>(want);
	}
	*transferred = static_cast<ULONG>(got);
	if (got)
		self->AddReadBytes(got);
	return STATUS_SUCCESS;
}

static NTSTATUS GetFileInfo(FSP_FILE_SYSTEM *fs, PVOID ctx, FSP_FSCTL_FILE_INFO *fi)
{
	ApfsFs *self = Self(fs);
	ApfsFs::Node *node = static_cast<ApfsFs::Node *>(ctx);
	if (self->Vol().Writable())
		Refresh(self, node);
	FillNodeInfo(self, node, *fi);
	return STATUS_SUCCESS;
}

static NTSTATUS GetStreamInfo(FSP_FILE_SYSTEM *fs, PVOID ctx, PVOID buf, ULONG len, PULONG transferred)
{
	ApfsFs *self = Self(fs);
	ApfsFs::Node *node = static_cast<ApfsFs::Node *>(ctx);
	alignas(8) UINT8 raw[sizeof(FSP_FSCTL_STREAM_INFO) + 256 * sizeof(WCHAR)];
	FSP_FSCTL_STREAM_INFO *si = reinterpret_cast<FSP_FSCTL_STREAM_INFO *>(raw);
	if (self->Vol().Writable())
		Refresh(self, node);
	*transferred = 0;
	Stat st;
	std::string name;
	SnapshotNode(node, st, name);
	if (st.type == FileType::File)
	{
		FSP_FSCTL_FILE_INFO fi;
		self->FillFileInfo(st, name, fi);
		si->Size = sizeof(FSP_FSCTL_STREAM_INFO);   // the unnamed data stream
		si->StreamSize = fi.FileSize;
		si->StreamAllocationSize = fi.AllocationSize;
		if (!FspFileSystemAddStreamInfo(si, buf, len, transferred))
			return STATUS_SUCCESS;
	}
	std::vector<std::string> names;
	self->Vol().ListXattrs(node->st.inode, node->st, names);
	for (const auto &n : names)
	{
		std::wstring w = ApfsNameToWindows(n);
		uint64_t size = 0;
		if (w.empty() || w.size() > 255 || !self->Vol().GetXattrSize(node->st.inode, n, size))
			continue;
		si->Size = static_cast<UINT16>(sizeof(FSP_FSCTL_STREAM_INFO) + w.size() * sizeof(WCHAR));
		si->StreamSize = size;
		si->StreamAllocationSize = (size + 4095) / 4096 * 4096;
		memcpy(si->StreamNameBuf, w.data(), w.size() * sizeof(WCHAR));
		if (!FspFileSystemAddStreamInfo(si, buf, len, transferred))
			return STATUS_SUCCESS;
	}
	FspFileSystemAddStreamInfo(nullptr, buf, len, transferred);
	return STATUS_SUCCESS;
}

// ---------------------------------------------------------------- reparse points

static NTSTATUS ResolveReparsePoints(FSP_FILE_SYSTEM *fs, PWSTR name, UINT32 index, BOOLEAN resolve_last, PIO_STATUS_BLOCK ios,
	PVOID buf, PSIZE_T size)
{
	return FspFileSystemResolveReparsePoints(fs, GetReparsePointByName, nullptr, name, index, resolve_last, ios, buf, size);
}

static NTSTATUS GetReparsePoint(FSP_FILE_SYSTEM *fs, PVOID ctx, PWSTR name, PVOID buf, PSIZE_T size)
{
	ApfsFs *self = Self(fs);
	ApfsFs::Node *node = static_cast<ApfsFs::Node *>(ctx);
	if (self->Vol().Writable())
		Refresh(self, node);
	return self->MakeReparseData(node->st, buf, size);
}

// A symlink made on Windows (CreateSymbolicLink creates an empty file or
// directory, then sets the reparse point): the empty object is replaced by an
// APFS symlink with the target converted to POSIX form.
static NTSTATUS SetReparsePoint(FSP_FILE_SYSTEM *fs, PVOID ctx, PWSTR name, PVOID buf, SIZE_T size)
{
	ApfsFs *self = Self(fs);
	ApfsFs::Node *node = static_cast<ApfsFs::Node *>(ctx);
	if (!self->Vol().Writable())
		return STATUS_MEDIA_WRITE_PROTECTED;
	if (!node->stream.empty())
		return STATUS_INVALID_PARAMETER;
	const REPARSE_DATA_BUFFER *r = static_cast<const REPARSE_DATA_BUFFER *>(buf);
	size_t head = FIELD_OFFSET(REPARSE_DATA_BUFFER, SymbolicLinkReparseBuffer.PathBuffer);
	if (size < REPARSE_DATA_BUFFER_HEADER_SIZE)
		return STATUS_IO_REPARSE_DATA_INVALID;
	if (r->ReparseTag != IO_REPARSE_TAG_SYMLINK)
		return STATUS_NOT_SUPPORTED;   // APFS has nowhere to keep other reparse data
	const auto &s = r->SymbolicLinkReparseBuffer;
	if (size < head || s.SubstituteNameLength == 0 || head + s.SubstituteNameOffset + s.SubstituteNameLength > size)
		return STATUS_IO_REPARSE_DATA_INVALID;
	std::wstring sub(reinterpret_cast<const WCHAR *>(reinterpret_cast<const uint8_t *>(s.PathBuffer) + s.SubstituteNameOffset),
		s.SubstituteNameLength / sizeof(WCHAR));

	Refresh(self, node);
	bool was_dir = node->st.type == FileType::Dir;
	if (node->st.type == FileType::File && node->st.size != 0)
		return STATUS_NOT_SUPPORTED;   // the data would be lost
	if (was_dir)
	{
		std::vector<DirEntry> ents;
		if (!self->Vol().ListDir(node->st.inode, ents))
			return STATUS_FILE_CORRUPT_ERROR;
		if (!ents.empty())
			return STATUS_DIRECTORY_NOT_EMPTY;
	}
	else if (node->st.type != FileType::File && node->st.type != FileType::Symlink)
		return STATUS_NOT_SUPPORTED;

	uint64_t parent;
	std::string aname;
	std::wstring pnorm;
	NTSTATUS st = self->ResolveParent(name, parent, aname, &pnorm);
	if (!NT_SUCCESS(st))
		return st;
	std::string target = self->WindowsTargetToPosix(sub, (s.Flags & SYMLINK_FLAG_RELATIVE) != 0, pnorm);
	if (target.empty())
		return STATUS_IO_REPARSE_DATA_INVALID;
	int err = self->Vol().Remove(parent, aname, was_dir);
	if (err)
		return Errno(err);
	DirEntry e;
	err = self->Vol().Symlink(parent, aname, target, e);
	if (err)
	{
		DirEntry back;   // put the empty file or directory back
		Stat st_cur;
		std::string name_cur;
		SnapshotNode(node, st_cur, name_cur);
		if (self->Vol().Create(parent, aname, was_dir, st_cur.mode & 07777, back) == 0 &&
		    self->Vol().GetStat(back.inode, st_cur))
		{
			std::lock_guard<std::mutex> lock(node->mutex);
			node->st = st_cur;
		}
		self->InvalidateCache();
		return Errno(err);
	}
	Stat st_new;
	if (self->Vol().GetStat(e.inode, st_new))
	{
		std::lock_guard<std::mutex> lock(node->mutex);
		node->st = st_new;
	}
	self->InvalidateCache();
	return STATUS_SUCCESS;
}

// Removing a symlink's reparse point leaves an empty file (or directory, for
// a link to a directory), as on NTFS.
static NTSTATUS DeleteReparsePoint(FSP_FILE_SYSTEM *fs, PVOID ctx, PWSTR name, PVOID buf, SIZE_T size)
{
	ApfsFs *self = Self(fs);
	ApfsFs::Node *node = static_cast<ApfsFs::Node *>(ctx);
	if (!self->Vol().Writable())
		return STATUS_MEDIA_WRITE_PROTECTED;
	Refresh(self, node);
	Stat st_cur;
	std::string name_cur;
	SnapshotNode(node, st_cur, name_cur);
	if (st_cur.type != FileType::Symlink)
		return STATUS_NOT_A_REPARSE_POINT;
	bool dir = self->SymlinkIsDir(st_cur);
	uint64_t parent;
	std::string aname;
	NTSTATUS st = self->ResolveParent(name, parent, aname, nullptr);
	if (!NT_SUCCESS(st))
		return st;
	int err = self->Vol().Remove(parent, aname, false);
	DirEntry e;
	if (!err)
		err = self->Vol().Create(parent, aname, dir, dir ? 0755 : 0644, e);
	if (!err)
	{
		Stat st_new;
		if (self->Vol().GetStat(e.inode, st_new))
		{
			std::lock_guard<std::mutex> lock(node->mutex);
			node->st = st_new;
		}
	}
	self->InvalidateCache();
	return Errno(err);
}

static NTSTATUS GetSecurity(FSP_FILE_SYSTEM *fs, PVOID ctx, PSECURITY_DESCRIPTOR sd, SIZE_T *sd_size)
{
	return Self(fs)->CopySecurity(sd, sd_size);
}

static bool AddDirEntry(ApfsFs *self, ApfsFs::Node *node, const std::wstring &wname, const std::string &name, const Stat &st, NTSTATUS &result)
{
	alignas(8) UINT8 raw[sizeof(FSP_FSCTL_DIR_INFO) + 256 * sizeof(WCHAR)];
	FSP_FSCTL_DIR_INFO *d = reinterpret_cast<FSP_FSCTL_DIR_INFO *>(raw);
	size_t bytes = std::min<size_t>(wname.size(), 255) * sizeof(WCHAR);
	memset(d, 0, sizeof(FSP_FSCTL_DIR_INFO));
	d->Size = static_cast<UINT16>(sizeof(FSP_FSCTL_DIR_INFO) + bytes);
	self->FillFileInfo(st, name, d->FileInfo);
	memcpy(d->FileNameBuf, wname.data(), bytes);
	return FspFileSystemFillDirectoryBuffer(&node->dir_buffer, d, &result) != FALSE;
}

// WINAPFS_PROFILE or WINAPFS_DIRSTATS: who lists directories (process id ->
// full listings), printed at exit. dirquery_test.ps1 uses it.
static std::mutex g_rd_mutex;
static std::map<DWORD, long> g_rd_by_pid;
static bool g_rd_count = [] {
	char v[4];
	return (GetEnvironmentVariableA("WINAPFS_PROFILE", v, 4) && v[0] != '0') ||
	       (GetEnvironmentVariableA("WINAPFS_DIRSTATS", v, 4) && v[0] != '0');
}();

void ReportDirectoryListings()
{
	if (!g_rd_count)
		return;
	std::lock_guard<std::mutex> lock(g_rd_mutex);
	for (const auto &kv : g_rd_by_pid)
	{
		wchar_t name[MAX_PATH] = L"?";
		DWORD n = MAX_PATH;
		HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, kv.first);
		if (p)
		{
			QueryFullProcessImageNameW(p, 0, name, &n);
			CloseHandle(p);
		}
		fprintf(stderr, "directory listings: %ld by pid %lu (%ls)\n", kv.second, kv.first, name);
	}
}

static NTSTATUS ReadDirectory(FSP_FILE_SYSTEM *fs, PVOID ctx, PWSTR pattern, PWSTR marker, PVOID buf, ULONG len, PULONG transferred)
{
	ApfsFs *self = Self(fs);
	ApfsFs::Node *node = static_cast<ApfsFs::Node *>(ctx);
	NTSTATUS result = STATUS_SUCCESS;

	if (node->st.type != FileType::Dir)
		return STATUS_NOT_A_DIRECTORY;
	if (g_rd_count && marker == nullptr)
	{
		std::lock_guard<std::mutex> lock(g_rd_mutex);
		g_rd_by_pid[FspFileSystemOperationProcessId()]++;
	}

	if (FspFileSystemAcquireDirectoryBuffer(&node->dir_buffer, marker == nullptr, &result))
	{
		bool ok = true;
		if (node->st.inode != kRootInode)
		{
			Stat parent;
			if (!self->Vol().GetStat(node->st.parent, parent))
				parent = node->st;
			ok = AddDirEntry(self, node, L".", ".", node->st, result) && AddDirEntry(self, node, L"..", "..", parent, result);
		}

		std::vector<DirEntry> ents;
		if (ok && !self->Vol().ListDir(node->st.inode, ents))
		{
			result = STATUS_FILE_CORRUPT_ERROR;
			ok = false;
		}
		for (size_t i = 0; ok && i < ents.size(); i++)
		{
			Stat st;
			if (!self->Vol().GetStat(ents[i].inode, st))
				continue;
			ok = AddDirEntry(self, node, ApfsNameToWindows(ents[i].name), ents[i].name, st, result);
		}
		FspFileSystemReleaseDirectoryBuffer(&node->dir_buffer);
	}

	if (!NT_SUCCESS(result))
		return result;

	FspFileSystemReadDirectoryBuffer(&node->dir_buffer, marker, buf, len, transferred);
	return STATUS_SUCCESS;
}

// A QueryDirectory for one exact name (filters and Explorer do this after every
// create, to fetch the stored name). Without this callback WinFsp answers it by
// listing the whole directory, which made copying many files into one folder
// quadratic.
static NTSTATUS GetDirInfoByName(FSP_FILE_SYSTEM *fs, PVOID ctx, PWSTR name, FSP_FSCTL_DIR_INFO *d)
{
	ApfsFs *self = Self(fs);
	ApfsFs::Node *node = static_cast<ApfsFs::Node *>(ctx);
	if (node->st.type != FileType::Dir)
		return STATUS_NOT_A_DIRECTORY;
	std::wstring wname(name);
	if (wname.empty() || wname.size() > 255)
		return STATUS_OBJECT_NAME_NOT_FOUND;
	DirEntry e;
	Stat st;
	if (!self->Vol().Lookup(node->st.inode, WindowsNameToApfs(wname), e) || !self->Vol().GetStat(e.inode, st))
		return STATUS_OBJECT_NAME_NOT_FOUND;
	std::wstring stored = ApfsNameToWindows(e.name);
	size_t bytes = std::min<size_t>(stored.size(), 255) * sizeof(WCHAR);
	memset(d, 0, sizeof(FSP_FSCTL_DIR_INFO));
	d->Size = static_cast<UINT16>(sizeof(FSP_FSCTL_DIR_INFO) + bytes);
	self->FillFileInfo(st, e.name, d->FileInfo);
	memcpy(d->FileNameBuf, stored.data(), bytes);
	return STATUS_SUCCESS;
}

static FSP_FILE_SYSTEM_INTERFACE g_interface = [] {
	FSP_FILE_SYSTEM_INTERFACE i;
	memset(&i, 0, sizeof(i));
	i.GetVolumeInfo = GetVolumeInfo;
	i.GetSecurityByName = GetSecurityByName;
	i.Create = Create;
	i.Open = Open;
	i.Overwrite = Overwrite;
	i.Cleanup = Cleanup;
	i.Close = Close;
	i.Read = Read;
	i.Write = Write;
	i.Flush = Flush;
	i.GetFileInfo = GetFileInfo;
	i.SetBasicInfo = SetBasicInfo;
	i.SetFileSize = SetFileSize;
	i.CanDelete = CanDelete;
	i.Rename = Rename;
	i.GetSecurity = GetSecurity;
	i.ReadDirectory = ReadDirectory;
	i.GetDirInfoByName = GetDirInfoByName;
	i.GetStreamInfo = GetStreamInfo;
	i.ResolveReparsePoints = ResolveReparsePoints;
	i.GetReparsePoint = GetReparsePoint;
	i.SetReparsePoint = SetReparsePoint;
	i.DeleteReparsePoint = DeleteReparsePoint;
	return i;
}();

static bool IsElevated()
{
	HANDLE token = nullptr;
	TOKEN_ELEVATION e = {};
	DWORD n = 0;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
		return false;
	bool elevated = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &n) && e.TokenIsElevated;
	CloseHandle(token);
	return elevated;
}

// Highest free letter from Z: down to D: (what WinFsp picks for NULL).
static wchar_t FreeDriveLetter()
{
	DWORD used = GetLogicalDrives();
	for (wchar_t c = L'Z'; c >= L'D'; c--)
	{
		wchar_t root[] = { c, L':', 0 };
		wchar_t target[MAX_PATH];
		if (!(used & (1u << (c - L'A'))) && !QueryDosDeviceW(root, target, MAX_PATH))
			return c;
	}
	return 0;
}

NTSTATUS ApfsFs::Start(const wchar_t *mount_point)
{
	const VolumeInfo &info = m_vol->Info();
	FSP_FSCTL_VOLUME_PARAMS vp;
	memset(&vp, 0, sizeof(vp));
	vp.Version = sizeof(FSP_FSCTL_VOLUME_PARAMS);
	vp.SectorSize = 4096;
	vp.SectorsPerAllocationUnit = static_cast<UINT16>(std::max<uint32_t>(1, m_block_size / 4096));
	// WinFsp checks "name:stream:$DATA" as one component (NTFS checks the parts
	// separately): room for a 255-character name plus a 127-byte xattr name.
	// Longer file names are refused in ResolveParent.
	vp.MaxComponentLength = 255 + 1 + 127 + 6;
	vp.VolumeCreationTime = NsToFileTime(info.last_mod_ns);
	vp.VolumeSerialNumber = static_cast<UINT32>(std::hash<std::string>()(info.uuid));
	// read-only: metadata never changes, cache it forever
	vp.FileInfoTimeout = m_vol->Writable() ? 1000 : 0xFFFFFFFF;
	vp.CaseSensitiveSearch = info.case_sensitive ? 1 : 0;
	vp.CasePreservedNames = 1;
	vp.UnicodeOnDisk = 1;
	vp.PersistentAcls = 1;
	vp.ReadOnlyVolume = m_vol->Writable() ? 0 : 1;
	vp.PostCleanupWhenModifiedOnly = 1;
	vp.PassQueryDirectoryFileName = 1;  // GetDirInfoByName
	vp.NamedStreams = 1;                // xattrs
	vp.ReparsePoints = 1;               // symlinks, FIFOs, devices
	vp.ReparsePointsAccessCheck = 0;
	vp.UmFileContextIsUserContext2 = 1;
	wcscpy_s(vp.FileSystemName, L"APFS");

	NTSTATUS r = FspFileSystemCreate(const_cast<PWSTR>(L"" FSP_FSCTL_DISK_DEVICE_NAME), &vp, &g_interface, &m_fs);
	if (!NT_SUCCESS(r))
		return r;
	m_fs->UserContext = this;
	if (m_opts.debug)
	{
		FspDebugLogSetHandle(GetStdHandle(STD_ERROR_HANDLE));
		FspFileSystemSetDebugLog(m_fs, static_cast<UINT32>(-1));
	}

	// Elevated: a plain drive letter would live in the admin token's own DOS
	// device namespace, invisible to the user's (non-elevated) Explorer. The
	// WinFsp form \\.\X: creates it through the Mount Manager instead: a
	// global drive letter every session sees (needs administrator rights).
	r = STATUS_UNSUCCESSFUL;
	bool letter = !mount_point || (iswalpha(mount_point[0]) && mount_point[1] == L':' && mount_point[2] == 0);
	if (letter && IsElevated())
	{
		wchar_t c = mount_point ? static_cast<wchar_t>(towupper(mount_point[0])) : FreeDriveLetter();
		if (c)
		{
			std::wstring mm = std::wstring(L"\\\\.\\") + c + L":";
			r = FspFileSystemSetMountPoint(m_fs, const_cast<PWSTR>(mm.c_str()));
			if (!NT_SUCCESS(r) && m_opts.debug)
				fprintf(stderr, "Mount Manager mount at %lc: failed (0x%08lx); using a session drive letter\n", c, r);
		}
	}
	if (!NT_SUCCESS(r))
		r = FspFileSystemSetMountPoint(m_fs, const_cast<PWSTR>(mount_point));
	if (NT_SUCCESS(r))
		r = FspFileSystemStartDispatcher(m_fs, 0);
	if (!NT_SUCCESS(r))
	{
		FspFileSystemDelete(m_fs);
		m_fs = nullptr;
	}
	return r;
}

void ApfsFs::Stop()
{
	if (!m_fs)
		return;
	FspFileSystemStopDispatcher(m_fs);
	FspFileSystemDelete(m_fs);
	m_fs = nullptr;
	if (m_vol->Writable())
	{
		ApplyAllPendingSizes();   // handles still open at unmount
		m_vol->Sync();
	}
}

std::wstring ApfsFs::MountPoint() const
{
	if (!m_fs)
		return std::wstring();
	PWSTR mp = FspFileSystemMountPoint(m_fs);
	std::wstring s = mp ? std::wstring(mp) : std::wstring();
	if (s.compare(0, 4, L"\\\\.\\") == 0)
		s = s.substr(4);   // Mount Manager form \\.\X: -> X:
	return s;
}

bool LoadWinFsp()
{
	return NT_SUCCESS(FspLoad(nullptr));
}

} // namespace winapfs
