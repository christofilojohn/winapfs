// winapfs - read-only APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Thin, thread-safe layer over ApfsLib used by the CLI, the WinFsp front-end
// and the tests. All names and paths are UTF-8 exactly as stored on disk.

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class Device;
class ApfsContainer;
class ApfsVolume;

namespace winapfs {

constexpr uint64_t kRootInode = 2;      // ROOT_DIR_INO_NUM
constexpr uint64_t kRootParent = 1;     // ROOT_DIR_PARENT

// APFS_GPT_PARTITION_UUID, 7C3457EF-0000-11AA-AA11-00306543ECAC
struct PartitionInfo
{
	int index = -1;             // GPT entry index
	uint64_t offset = 0;        // bytes
	uint64_t size = 0;          // bytes
	std::string name;           // GPT partition name (UTF-8)
};

struct VolumeInfo
{
	unsigned fsid = 0;          // index in nx_fs_oid[]
	std::string name;
	std::string uuid;
	uint16_t role = 0;
	bool encrypted = false;     // needs a key (FileVault / software encryption)
	bool case_sensitive = false;
	bool normalization_insensitive = false;
	bool sealed = false;
	uint64_t bytes_used = 0;
	uint64_t quota_bytes = 0;
	uint64_t reserve_bytes = 0;
	int64_t last_mod_ns = 0;
	std::string password_hint;  // encrypted volumes only, if the keybag has one
};

enum class FileType { Unknown, File, Dir, Symlink, Fifo, CharDev, BlockDev, Socket };

const char *FileTypeName(FileType t);

struct Stat
{
	uint64_t inode = 0;
	uint64_t parent = 0;
	uint64_t private_id = 0;    // data stream object id
	FileType type = FileType::Unknown;
	uint16_t mode = 0;          // full st_mode incl. S_IFMT
	uint32_t uid = 0;
	uint32_t gid = 0;
	uint32_t bsd_flags = 0;
	uint64_t internal_flags = 0;
	uint64_t nlink = 1;         // files: link count; dirs: 2 + subdirs is not stored, so number of children
	uint64_t size = 0;          // logical size (uncompressed for compressed files)
	uint64_t alloc_size = 0;
	int64_t create_ns = 0;      // nanoseconds since 1970-01-01 UTC
	int64_t mod_ns = 0;
	int64_t change_ns = 0;
	int64_t access_ns = 0;
	bool compressed = false;    // UF_COMPRESSED (decmpfs)
	uint32_t rdev = 0;
};

struct DirEntry
{
	std::string name;
	uint64_t inode = 0;
	FileType type = FileType::Unknown;
};

// BSD flags
constexpr uint32_t kUF_NODUMP = 0x00000001;
constexpr uint32_t kUF_IMMUTABLE = 0x00000002;
constexpr uint32_t kUF_APPEND = 0x00000004;
constexpr uint32_t kUF_COMPRESSED = 0x00000020;
constexpr uint32_t kUF_HIDDEN = 0x00008000;
constexpr uint32_t kSF_IMMUTABLE = 0x00020000;

// Volume::SetXattr flags
constexpr int kXattrCreate = 1;
constexpr int kXattrReplace = 2;

class Volume;

// Random-access reader over a file's logical (decompressed) contents.
class FileReader
{
public:
	virtual ~FileReader() = default;
	virtual uint64_t Size() const = 0;
	// Reads up to len bytes at off; stops at EOF. Returns false on I/O or
	// decompression error.
	virtual bool Read(uint64_t off, void *buf, size_t len, size_t &got) = 0;
};

class Container
{
public:
	~Container();

	// path: an image file (bare container or GPT disk image) or a raw device
	// such as \\.\PhysicalDrive1. partition = -1 picks the first APFS GPT
	// partition, or the whole device when there is no GPT.
	static std::unique_ptr<Container> Open(const std::string &path, std::string &err, int partition = -1);

	// Lists APFS partitions on a GPT disk or image. Empty if no GPT.
	static bool FindApfsPartitions(const std::string &path, std::vector<PartitionInfo> &parts, std::string &err);

	const std::vector<VolumeInfo> &Volumes() const { return m_volumes; }
	const VolumeInfo *FindVolume(const std::string &name_or_index) const;

	// passphrase may be a volume password or a personal recovery key.
	std::unique_ptr<Volume> OpenVolume(unsigned fsid, const std::string &passphrase, std::string &err);

	uint32_t BlockSize() const;
	uint64_t BlockCount() const;
	uint64_t FreeBlocks() const;

	// macOS writes the superblock copy in block zero at a clean unmount. A
	// newer checkpoint means it was not (a crash, a hibernated Mac, a disk
	// unplugged without ejecting): the Mac may still expect that state.
	bool CleanlyUnmounted() const;
	bool IsFusion() const;
	// Why a volume must not be opened read-write, or "" if it may be.
	std::string WriteRefusal(const VolumeInfo &v, bool ignore_unclean = false) const;
	const std::string &Path() const { return m_path; }
	uint64_t PartitionOffset() const { return m_offset; }
	uint64_t PartitionSize() const { return m_size; }

private:
	Container() = default;

	std::string m_path;
	uint64_t m_offset = 0;
	uint64_t m_size = 0;
	std::unique_ptr<Device> m_dev;
	std::unique_ptr<ApfsContainer> m_nx;
	std::vector<VolumeInfo> m_volumes;
	std::mutex m_open_mutex;  // GetVolume stores the passphrase in the container
};

struct VolumeSpace
{
	uint32_t block_size = 4096;
	uint64_t total_bytes = 0;
	uint64_t free_bytes = 0;
};

// A mounted APFS volume, independent of the engine behind it: ApfsLib
// (LibVolume, read-only, supports encryption) or linux-apfs-rw (RwVolume).
// Inode numbers are the APFS object ids. Thread-safe.
class Volume
{
public:
	virtual ~Volume() = default;

	const VolumeInfo &Info() const { return m_info; }
	virtual const char *EngineName() const = 0;
	virtual bool GetSpace(VolumeSpace &space) = 0;

	virtual bool GetStat(uint64_t inode, Stat &st) = 0;

	// One path component, matched the way APFS does on this volume
	// (case/normalization-insensitive where the volume says so). out.name is
	// the stored name.
	virtual bool Lookup(uint64_t parent, const std::string &name, DirEntry &out) = 0;

	// "/a/b/c" (or "" / "/" for the root). Does not follow symlinks.
	bool ResolvePath(const std::string &path, uint64_t &inode);

	virtual bool ListDir(uint64_t dir_inode, std::vector<DirEntry> &entries) = 0;

	virtual std::unique_ptr<FileReader> OpenFile(const Stat &st, std::string &err) = 0;

	// Extended attributes as macOS userland sees them: the decmpfs xattr,
	// a compressed file's resource fork, empty resource forks and
	// com.apple.fs.* are hidden.
	virtual bool ListXattrs(uint64_t inode, const Stat &st, std::vector<std::string> &names) = 0;
	virtual bool GetXattr(uint64_t inode, const std::string &name, std::vector<uint8_t> &data) = 0;
	virtual bool GetXattrSize(uint64_t inode, const std::string &name, uint64_t &size) = 0;
	virtual bool ReadXattr(uint64_t inode, const std::string &name, uint64_t off, void *buf, size_t len, size_t &got) = 0;
	virtual bool ReadLink(uint64_t inode, std::string &target) = 0;

	// ---- writing (linux-apfs-rw volumes mounted read-write). Every call
	// returns 0 or a negative errno; read-only volumes return -EROFS.
	//
	// Write and SetSize on a compressed file first replace it by an
	// uncompressed copy (a new inode); the old inode number keeps working
	// for the rest of the mount, and GetStat on it reports the new number.
	virtual bool Writable() const { return false; }
	virtual bool IoFailed() const { return false; }
	virtual uint64_t DecompressGen() const { return 0; }
	virtual int Create(uint64_t parent, const std::string &name, bool dir, uint32_t mode, DirEntry &out) { return -30; }
	virtual int Symlink(uint64_t parent, const std::string &name, const std::string &target, DirEntry &out) { return -30; }
	virtual int Link(uint64_t inode, uint64_t parent, const std::string &name) { return -30; }
	// value replaces the whole attribute; flags: kXattrCreate, kXattrReplace.
	// File-system-owned names (com.apple.fs.*, com.apple.decmpfs, a compressed
	// file's resource fork) give -EPERM.
	virtual int SetXattr(uint64_t inode, const std::string &name, const void *data, size_t len, int flags) { return -30; }
	virtual int RemoveXattr(uint64_t inode, const std::string &name) { return -30; }
	virtual int SetCreateTime(uint64_t inode, int64_t ns) { return -30; }
	// BSD flags: the bits in mask take their values from flags. Only
	// UF_NODUMP and UF_HIDDEN can be changed.
	virtual int SetFlags(uint64_t inode, uint32_t flags, uint32_t mask) { return -30; }
	// Removes one name; the inode is freed when its last name and use go.
	virtual int Remove(uint64_t parent, const std::string &name, bool dir) { return -30; }
	virtual int Rename(uint64_t old_parent, const std::string &old_name, uint64_t new_parent, const std::string &new_name, bool replace) { return -30; }
	virtual int Write(uint64_t inode, uint64_t off, const void *buf, size_t len, size_t &written) { return -30; }
	virtual int SetSize(uint64_t inode, uint64_t size) { return -30; }
	// INT64_MIN leaves a time unchanged; ctime becomes now.
	virtual int SetTimes(uint64_t inode, int64_t atime_ns, int64_t mtime_ns) { return -30; }
	virtual int SetMode(uint64_t inode, uint32_t mode) { return -30; }
	// Makes everything written so far durable (a transaction commit).
	virtual int Sync() { return -30; }

protected:
	explicit Volume(const VolumeInfo &info) : m_info(info) {}
	VolumeInfo m_info;
};

// ApfsLib-backed volume (read-only; the only engine that decrypts).
class LibVolume : public Volume
{
public:
	~LibVolume() override;

	const char *EngineName() const override { return "apfs-fuse"; }
	bool GetSpace(VolumeSpace &space) override;
	bool GetStat(uint64_t inode, Stat &st) override;
	bool Lookup(uint64_t parent, const std::string &name, DirEntry &out) override;
	bool ListDir(uint64_t dir_inode, std::vector<DirEntry> &entries) override;
	std::unique_ptr<FileReader> OpenFile(const Stat &st, std::string &err) override;
	bool ListXattrs(uint64_t inode, const Stat &st, std::vector<std::string> &names) override;
	bool GetXattr(uint64_t inode, const std::string &name, std::vector<uint8_t> &data) override;
	bool GetXattrSize(uint64_t inode, const std::string &name, uint64_t &size) override;
	// Stream-backed xattrs are read in place.
	bool ReadXattr(uint64_t inode, const std::string &name, uint64_t off, void *buf, size_t len, size_t &got) override;
	bool ReadLink(uint64_t inode, std::string &target) override;

	// Raw data-stream read (no decompression), used by FileReader.
	bool ReadStream(uint64_t private_id, uint64_t off, void *buf, size_t len);
	ApfsVolume &Raw() { return *m_vol; }
	Container &GetContainer() const { return m_container; }

private:
	friend class Container;
	LibVolume(Container &c, std::unique_ptr<ApfsVolume> v, const VolumeInfo &info);

	Container &m_container;
	std::unique_ptr<ApfsVolume> m_vol;
};

// Opens volume fsid of the container with the linux-apfs-rw engine. The
// volume keeps its own device handle, so the Container may be closed after.
// Encrypted volumes are not supported by that engine (err says so).
std::unique_ptr<Volume> OpenRwVolume(const Container &c, unsigned fsid, bool readwrite, std::string &err);

struct PhysicalDrive
{
	std::string path;           // \\.\PhysicalDriveN
	uint64_t size = 0;
	bool access_denied = false; // raw disk access needs an elevated process
	bool has_gpt = false;
	std::vector<PartitionInfo> apfs;
};

// Probes \\.\PhysicalDrive0..63 and lists their APFS partitions.
std::vector<PhysicalDrive> EnumeratePhysicalDrives();

// Encoding helpers (Windows)
std::wstring Utf8ToWide(const std::string &s);
std::string WideToUtf8(const std::wstring &s);

} // namespace winapfs
