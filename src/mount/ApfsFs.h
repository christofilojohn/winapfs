// winapfs - read-only APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// WinFsp file system for one APFS volume (native WinFsp API; read-only, or
// read-write when the volume is writable).

#pragma once

#include <winapfs/Apfs.h>

#include <winfsp/winfsp.h>

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

namespace winapfs {

struct MountOptions
{
	bool debug = false;
	bool hide_dotfiles = true;    // names starting with '.' are FILE_ATTRIBUTE_HIDDEN (as on macOS)
};

class ApfsFs
{
public:
	ApfsFs(std::unique_ptr<Volume> vol, const MountOptions &opts);
	~ApfsFs();

	// mount_point: "X:" or a directory path, or nullptr to let WinFsp pick a
	// free drive letter.
	NTSTATUS Start(const wchar_t *mount_point);
	void Stop();
	std::wstring MountPoint() const;

	Volume &Vol() { return *m_vol; }

	// Shared with the WinFsp callbacks in ApfsFs.cpp.
	struct Node;
	NTSTATUS Resolve(const wchar_t *path, Stat &st, std::wstring *normalized, std::string *stream);
	// Parent directory inode and last component (APFS bytes) of a path.
	NTSTATUS ResolveParent(const wchar_t *path, uint64_t &parent, std::string &name, std::wstring *parent_normalized);
	// Forget cached path -> inode mappings (after a rename or delete).
	void InvalidateCache();
	void FillFileInfo(const Stat &st, const std::string &name, FSP_FSCTL_FILE_INFO &fi);
	NTSTATUS CopySecurity(PSECURITY_DESCRIPTOR sd, SIZE_T *size);
	uint32_t Attributes(const Stat &st, const std::string &name);

	// Reparse points: symlinks (IO_REPARSE_TAG_SYMLINK), FIFOs, devices and
	// sockets (the WSL tags).
	static uint32_t ReparseTag(const Stat &st);
	// Whether a symlink resolves, inside the volume, to a directory.
	bool SymlinkIsDir(const Stat &st);
	NTSTATUS MakeReparseData(const Stat &st, PVOID buf, PSIZE_T size);
	// POSIX symlink target -> Windows substitute name (+ relative flag) and back.
	// link_dir: Windows path of the link's directory inside the volume ("" = root).
	static void PosixTargetToWindows(const std::string &target, std::wstring &sub, std::wstring &print, bool &relative);
	std::string WindowsTargetToPosix(const std::wstring &sub, bool relative, const std::wstring &link_dir);

	// Named streams are xattrs; only those ListXattrs shows are visible.
	bool StreamVisible(const Stat &st, const std::string &xattr);

	// Pending file growth. Windows sets the final size before writing (CopyFile,
	// Explorer); growing the APFS file then would make every write land in a
	// hole mid-file, where the engine creates one extent per block. The size
	// is reported at once and applied when the data didn't reach it (cleanup,
	// flush, unmount).
	uint64_t EffectiveSize(const Stat &st);
	void SetPendingSize(uint64_t inode, uint64_t size);   // 0 = none
	void ClearPendingSizeIfAtMost(uint64_t inode, uint64_t reached);
	uint64_t PendingSize(uint64_t inode);
	int ApplyPendingSize(uint64_t inode);
	void ApplyAllPendingSizes();

	const MountOptions &Options() const { return m_opts; }
	FSP_FILE_SYSTEM *FileSystem() { return m_fs; }

	void AddReadBytes(uint64_t n) { m_read_bytes.fetch_add(n, std::memory_order_relaxed); }
	void AddWriteBytes(uint64_t n) { m_write_bytes.fetch_add(n, std::memory_order_relaxed); }
	uint64_t ReadBytes() const { return m_read_bytes.load(std::memory_order_relaxed); }
	uint64_t WriteBytes() const { return m_write_bytes.load(std::memory_order_relaxed); }

private:
	bool FollowLink(uint64_t dir, const std::string &target, int depth, Stat &out);
	std::string LocalTarget(const std::string &target);
	bool CacheGet(const std::wstring &dir, uint64_t &inode, std::wstring &normalized);
	void CachePut(const std::wstring &dir, uint64_t inode, const std::wstring &normalized, uint64_t gen);

	std::unique_ptr<Volume> m_vol;
	MountOptions m_opts;
	FSP_FILE_SYSTEM *m_fs = nullptr;
	PSECURITY_DESCRIPTOR m_sd = nullptr;
	DWORD m_sd_size = 0;
	uint32_t m_block_size = 4096;

	// Directory path -> (inode, normalized path). Only directories are cached
	// (the hot part of path resolution); entries never go stale on a read-only
	// volume.
	std::mutex m_cache_mutex;
	uint64_t m_cache_gen = 0;
	std::unordered_map<std::wstring, std::pair<uint64_t, std::wstring>> m_dir_cache;

	std::mutex m_pending_mutex;
	std::unordered_map<uint64_t, uint64_t> m_pending_size;   // inode -> size Windows set
	std::atomic<uint64_t> m_read_bytes{ 0 };
	std::atomic<uint64_t> m_write_bytes{ 0 };
};

// Loads winfsp-x64.dll from the WinFsp install directory (delay-loaded).
bool LoadWinFsp();
// WINAPFS_PROFILE: directory listings per requesting process (stderr)
void ReportDirectoryListings();

} // namespace winapfs
