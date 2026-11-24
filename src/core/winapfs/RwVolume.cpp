// winapfs - read-only APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Volume on the linux-apfs-rw engine (apfsrw API). The engine works with
// nodes (dentries); the Volume interface works with inode numbers. Recently
// used nodes are cached (LRU, bounded); others are opened again by inode
// number (apfsrw_iget). Every node handed out is a held reference (NodeRef),
// so eviction never frees a node another thread is using.

#include "Apfs.h"

#include <apfsrw.h>

#include <atomic>
#include <list>
#include <mutex>
#include <unordered_map>

namespace winapfs {

namespace {

FileType TypeFromMode(uint32_t mode)
{
	switch (mode & 0170000)
	{
	case 0100000: return FileType::File;
	case 0040000: return FileType::Dir;
	case 0120000: return FileType::Symlink;
	case 0010000: return FileType::Fifo;
	case 0020000: return FileType::CharDev;
	case 0060000: return FileType::BlockDev;
	case 0140000: return FileType::Socket;
	default: return FileType::Unknown;
	}
}

FileType TypeFromDtype(unsigned dt)
{
	switch (dt)
	{
	case APFSRW_DT_REG: return FileType::File;
	case APFSRW_DT_DIR: return FileType::Dir;
	case APFSRW_DT_LNK: return FileType::Symlink;
	case APFSRW_DT_FIFO: return FileType::Fifo;
	case APFSRW_DT_CHR: return FileType::CharDev;
	case APFSRW_DT_BLK: return FileType::BlockDev;
	case APFSRW_DT_SOCK: return FileType::Socket;
	default: return FileType::Unknown;
	}
}

class RwReader : public FileReader
{
public:
	RwReader(apfsrw_file *f, bool serialize) : m_file(f), m_serialize(serialize) {}
	~RwReader() override { apfsrw_release(m_file); }

	uint64_t Size() const override { return apfsrw_file_size(m_file); }   // live: files can grow

	bool Read(uint64_t off, void *buf, size_t len, size_t &got) override
	{
		// the driver keeps decompression state per open file
		std::unique_lock<std::mutex> lock(m_mutex, std::defer_lock);
		if (m_serialize)
			lock.lock();
		return apfsrw_read(m_file, off, buf, len, &got) == 0;
	}

private:
	apfsrw_file *m_file;
	bool m_serialize;
	std::mutex m_mutex;
};

// A held node reference (apfsrw_put on destruction); converts to the pointer.
class NodeRef
{
public:
	NodeRef() = default;
	explicit NodeRef(apfsrw_node *n) : m_node(n) {}
	NodeRef(NodeRef &&o) noexcept : m_node(o.m_node) { o.m_node = nullptr; }
	NodeRef &operator=(NodeRef &&o) noexcept
	{
		if (this != &o)
		{
			if (m_node)
				apfsrw_put(m_node);
			m_node = o.m_node;
			o.m_node = nullptr;
		}
		return *this;
	}
	NodeRef(const NodeRef &) = delete;
	NodeRef &operator=(const NodeRef &) = delete;
	~NodeRef()
	{
		if (m_node)
			apfsrw_put(m_node);
	}
	operator apfsrw_node *() const { return m_node; }

private:
	apfsrw_node *m_node = nullptr;
};

class RwVolume : public Volume
{
public:
	RwVolume(apfsrw_mount *m, const VolumeInfo &info) : Volume(info), m_mount(m)
	{
		apfsrw_node *root = nullptr;
		apfsrw_root(m_mount, &root);
		m_root = root;   // pinned, never evicted
	}

	~RwVolume() override
	{
		for (auto &kv : m_nodes)
			apfsrw_put(kv.second.node);
		m_nodes.clear();
		apfsrw_put(m_root);
		apfsrw_unmount(m_mount);
	}

	const char *EngineName() const override { return "linux-apfs-rw"; }

	bool GetSpace(VolumeSpace &space) override
	{
		apfsrw_statfs s;
		if (apfsrw_get_statfs(m_mount, &s))
			return false;
		space.block_size = static_cast<uint32_t>(s.block_size);
		space.total_bytes = s.blocks * s.block_size;
		space.free_bytes = s.blocks_avail * s.block_size;
		return true;
	}

	bool GetStat(uint64_t inode, Stat &st) override
	{
		apfsrw_stat s;
		st = Stat();
		if (inode == kRootParent)
		{
			st.inode = kRootParent;
			st.type = FileType::Dir;
			st.mode = 0040755;
			return true;
		}
		NodeRef n = Get(inode);
		if (!n || apfsrw_getattr(n, &s))
			return false;
		st.inode = s.ino;
		st.parent = s.ino == kRootInode ? kRootParent : s.parent_ino;
		st.mode = static_cast<uint16_t>(s.mode);
		st.type = TypeFromMode(s.mode);
		st.uid = s.uid;
		st.gid = s.gid;
		st.bsd_flags = s.bsd_flags;
		st.nlink = s.nlink;
		st.size = s.size;
		st.alloc_size = s.blocks * 512;
		st.create_ns = s.btime_ns;
		st.mod_ns = s.mtime_ns;
		st.change_ns = s.ctime_ns;
		st.access_ns = s.atime_ns;
		st.compressed = (s.bsd_flags & kUF_COMPRESSED) != 0;
		st.rdev = s.rdev;
		return true;
	}

	bool Lookup(uint64_t parent, const std::string &name, DirEntry &out) override
	{
		NodeRef p = Get(parent);
		if (!p)
			return false;
		char stored[1024];
		apfsrw_node *child = nullptr;
		if (apfsrw_lookup_stored(p, name.data(), name.size(), &child, stored, sizeof(stored)))
			return false;
		apfsrw_stat s;
		if (apfsrw_getattr(child, &s))
		{
			apfsrw_put(child);
			return false;
		}
		Remember(s.ino, child);
		out.name = stored;
		out.inode = s.ino;
		out.type = TypeFromMode(s.mode);
		return true;
	}

	bool ListDir(uint64_t dir_inode, std::vector<DirEntry> &entries) override
	{
		NodeRef dir = Get(dir_inode);
		entries.clear();
		if (!dir)
			return false;
		struct Ctx
		{
			std::vector<DirEntry> *out;
		} ctx{ &entries };
		int err = apfsrw_readdir(dir, [](void *c, const char *name, size_t len, uint64_t ino, unsigned dtype) {
			DirEntry e;
			e.name.assign(name, len);
			e.inode = ino;
			e.type = TypeFromDtype(dtype);
			static_cast<Ctx *>(c)->out->push_back(std::move(e));
			return 0;
		}, &ctx);
		// children are opened by inode number when stat'ed (Get)
		return err == 0;
	}

	std::unique_ptr<FileReader> OpenFile(const Stat &st, std::string &err) override
	{
		NodeRef n = Get(st.inode);
		apfsrw_file *f = nullptr;
		if (st.type != FileType::File)
		{
			err = "not a regular file";
			return nullptr;
		}
		int rc = n ? apfsrw_open(n, false, &f) : -2;
		if (rc)
		{
			err = "open failed (errno " + std::to_string(-rc) + ")";
			return nullptr;
		}
		return std::unique_ptr<FileReader>(new RwReader(f, st.compressed));
	}

	bool ListXattrs(uint64_t inode, const Stat &st, std::vector<std::string> &names) override
	{
		NodeRef n = Get(inode);
		names.clear();
		if (!n)
			return false;
		long size = apfsrw_listxattr(n, nullptr, 0);
		if (size <= 0)
			return true;
		std::vector<char> buf(size);
		size = apfsrw_listxattr(n, buf.data(), buf.size());
		for (long p = 0; p < size; p += static_cast<long>(strlen(buf.data() + p)) + 1)
		{
			std::string name = buf.data() + p;
			if (name == "com.apple.decmpfs" || name.compare(0, 13, "com.apple.fs.") == 0)
				continue;
			if (name == "com.apple.ResourceFork")
			{
				uint64_t rsize = 0;
				if (st.compressed || !GetXattrSize(inode, name, rsize) || rsize == 0)
					continue;
			}
			names.push_back(std::move(name));
		}
		return true;
	}

	bool GetXattr(uint64_t inode, const std::string &name, std::vector<uint8_t> &data) override
	{
		NodeRef n = Get(inode);
		if (!n)
			return false;
		long size = apfsrw_getxattr(n, name.c_str(), nullptr, 0);
		if (size < 0)
			return false;
		data.resize(size);
		size = apfsrw_getxattr(n, name.c_str(), data.data(), data.size());
		if (size < 0)
			return false;
		data.resize(size);
		return true;
	}

	bool GetXattrSize(uint64_t inode, const std::string &name, uint64_t &size) override
	{
		NodeRef n = Get(inode);
		long s = n ? apfsrw_getxattr(n, name.c_str(), nullptr, 0) : -1;
		if (s < 0)
			return false;
		size = static_cast<uint64_t>(s);
		return true;
	}

	bool ReadXattr(uint64_t inode, const std::string &name, uint64_t off, void *buf, size_t len, size_t &got) override
	{
		std::vector<uint8_t> data;
		got = 0;
		if (!GetXattr(inode, name, data))
			return false;
		if (off < data.size())
		{
			got = static_cast<size_t>(std::min<uint64_t>(len, data.size() - off));
			memcpy(buf, data.data() + off, got);
		}
		return true;
	}

	bool ReadLink(uint64_t inode, std::string &target) override
	{
		NodeRef n = Get(inode);
		if (!n)
			return false;
		std::vector<char> buf(4096);
		int len = apfsrw_readlink(n, buf.data(), buf.size());
		if (len < 0)
			return false;
		if (static_cast<size_t>(len) >= buf.size())
		{
			buf.resize(len + 1);
			len = apfsrw_readlink(n, buf.data(), buf.size());
		}
		target.assign(buf.data(), len);
		return true;
	}

	// ---------------------------------------------------------------- writing

	bool Writable() const override { return apfsrw_is_readwrite(m_mount); }
	bool IoFailed() const override { return apfsrw_io_failed(m_mount); }
	uint64_t DecompressGen() const override { return m_decompress_gen.load(std::memory_order_acquire); }

	int Create(uint64_t parent, const std::string &name, bool dir, uint32_t mode, DirEntry &out) override
	{
		NodeRef p = Get(parent);
		apfsrw_node *n = nullptr;
		if (!p)
			return -2;
		int err = dir ? apfsrw_mkdir(p, name.data(), name.size(), mode, &n) : apfsrw_create(p, name.data(), name.size(), mode, &n);
		if (err)
			return err;
		apfsrw_stat s;
		err = apfsrw_getattr(n, &s);
		if (err)
		{
			apfsrw_put(n);
			return err;
		}
		Remember(s.ino, n);
		out.name = name;
		out.inode = s.ino;
		out.type = dir ? FileType::Dir : FileType::File;
		return 0;
	}

	int Remove(uint64_t parent, const std::string &name, bool dir) override
	{
		NodeRef p = Get(parent);
		if (!p)
			return -2;
		uint64_t victim = InodeOf(p, name);
		int err = dir ? apfsrw_rmdir(p, name.data(), name.size()) : apfsrw_unlink(p, name.data(), name.size());
		if (!err && victim)
			ForgetIfUnlinked(victim);
		return err;
	}

	int Rename(uint64_t old_parent, const std::string &old_name, uint64_t new_parent, const std::string &new_name, bool replace) override
	{
		NodeRef op = Get(old_parent), np = Get(new_parent);
		if (!op || !np)
			return -2;
		uint64_t moved = InodeOf(op, old_name), replaced = InodeOf(np, new_name);
		int err = apfsrw_rename(op, old_name.data(), old_name.size(), np, new_name.data(), new_name.size(),
			replace ? 0 : APFSRW_RENAME_NOREPLACE);
		if (err)
			return err;
		if (replaced && replaced != moved)
			ForgetIfUnlinked(replaced);
		return 0;
	}

	int Symlink(uint64_t parent, const std::string &name, const std::string &target, DirEntry &out) override
	{
		NodeRef p = Get(parent);
		apfsrw_node *n = nullptr;
		if (!p)
			return -2;
		int err = apfsrw_symlink(p, name.data(), name.size(), target.c_str(), &n);
		if (err)
			return err;
		apfsrw_stat s;
		err = apfsrw_getattr(n, &s);
		if (err)
		{
			apfsrw_put(n);
			return err;
		}
		Remember(s.ino, n);
		out.name = name;
		out.inode = s.ino;
		out.type = FileType::Symlink;
		return 0;
	}

	int Link(uint64_t inode, uint64_t parent, const std::string &name) override
	{
		NodeRef src = Get(inode), p = Get(parent);
		if (!src || !p)
			return -2;
		return apfsrw_link(src, p, name.data(), name.size(), nullptr);
	}

	int SetXattr(uint64_t inode, const std::string &name, const void *data, size_t len, int flags) override
	{
		NodeRef n = Get(inode);
		if (!n)
			return -2;
		int f = ((flags & kXattrCreate) ? APFSRW_XATTR_CREATE : 0) | ((flags & kXattrReplace) ? APFSRW_XATTR_REPLACE : 0);
		return apfsrw_setxattr(n, name.c_str(), data ? data : "", len, f);
	}

	int RemoveXattr(uint64_t inode, const std::string &name) override
	{
		NodeRef n = Get(inode);
		return n ? apfsrw_removexattr(n, name.c_str()) : -2;
	}

	int SetCreateTime(uint64_t inode, int64_t ns) override
	{
		apfsrw_setattr_t a = {};
		a.mask = APFSRW_SET_CRTIME;
		a.crtime_ns = ns;
		return Setattr(inode, a);
	}

	int SetFlags(uint64_t inode, uint32_t flags, uint32_t mask) override
	{
		if (mask & ~APFSRW_USER_FLAGS)
			return -1;   // EPERM
		apfsrw_setattr_t a = {};
		a.mask = APFSRW_SET_FLAGS;
		a.flags = flags;
		a.flags_mask = mask;
		return Setattr(inode, a);
	}

	int Write(uint64_t inode, uint64_t off, const void *buf, size_t len, size_t &written) override
	{
		written = 0;
		int err = Decompressed(inode);
		if (err)
			return err;
		NodeRef n = Get(inode);
		apfsrw_file *f = nullptr;
		if (!n)
			return -2;
		err = apfsrw_open(n, true, &f);
		if (err)
			return err;
		err = apfsrw_write(f, off, buf, len, &written);
		apfsrw_release(f);
		return err;
	}

	int SetSize(uint64_t inode, uint64_t size) override
	{
		int err = Decompressed(inode);
		if (err)
			return err;
		apfsrw_setattr_t a = {};
		a.mask = APFSRW_SET_SIZE;
		a.size = size;
		return Setattr(inode, a);
	}

	int SetTimes(uint64_t inode, int64_t atime_ns, int64_t mtime_ns) override
	{
		apfsrw_setattr_t a = {};
		if (atime_ns != INT64_MIN)
		{
			a.mask |= APFSRW_SET_ATIME;
			a.atime_ns = atime_ns;
		}
		if (mtime_ns != INT64_MIN)
		{
			a.mask |= APFSRW_SET_MTIME;
			a.mtime_ns = mtime_ns;
		}
		return a.mask ? Setattr(inode, a) : 0;
	}

	int SetMode(uint64_t inode, uint32_t mode) override
	{
		apfsrw_setattr_t a = {};
		a.mask = APFSRW_SET_MODE;
		a.mode = mode;
		return Setattr(inode, a);
	}

	int Sync() override
	{
		return apfsrw_sync(m_mount);
	}

private:
	int Setattr(uint64_t inode, const apfsrw_setattr_t &a)
	{
		NodeRef n = Get(inode);
		return n ? apfsrw_setattr(n, &a) : -2;
	}

	// The engine can't write compressed files: replace one by an uncompressed
	// copy (apfsrw_decompress) and make the old inode number an alias of the
	// new one. inode becomes the new number.
	int Decompressed(uint64_t &inode)
	{
		Stat st;
		if (!GetStat(inode, st))
			return -2;
		if (!st.compressed)
		{
			inode = st.inode;
			return 0;
		}
		std::lock_guard<std::mutex> dlock(m_decompress_mutex);
		if (!GetStat(inode, st))
			return -2;
		if (!st.compressed)
		{
			inode = st.inode;
			return 0;
		}
		NodeRef p = Get(st.parent);
		std::vector<DirEntry> ents;
		if (!p || !ListDir(st.parent, ents))
			return -2;
		const DirEntry *me = nullptr;
		for (const auto &e : ents)
			if (e.inode == st.inode)
				me = &e;
		if (!me)
			return -2;
		apfsrw_node *n = nullptr;
		int err = apfsrw_decompress(p, me->name.data(), me->name.size(), &n);
		if (err)
			return err;
		apfsrw_stat s;
		err = apfsrw_getattr(n, &s);
		if (err)
		{
			apfsrw_put(n);
			return err;
		}
		std::vector<apfsrw_node *> to_put;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_alias[st.inode] = s.ino;
			if (s.ino == kRootInode || m_nodes.count(s.ino))
			{
				to_put.push_back(n);
			}
			else
			{
				m_lru.push_back(s.ino);
				m_nodes[s.ino] = Entry{ n, std::prev(m_lru.end()) };
				while (m_nodes.size() > kMaxNodes)
				{
					uint64_t ev = m_lru.front();
					m_lru.pop_front();
					auto eit = m_nodes.find(ev);
					to_put.push_back(eit->second.node);
					m_nodes.erase(eit);
				}
			}
			auto oit = m_nodes.find(st.inode);
			if (oit != m_nodes.end())
			{
				to_put.push_back(oit->second.node);
				m_lru.erase(oit->second.lru);
				m_nodes.erase(oit);
			}
			m_decompress_gen.fetch_add(1, std::memory_order_release);
		}
		for (apfsrw_node *pnode : to_put)
			apfsrw_put(pnode);
		inode = s.ino;
		return 0;
	}

	// Lock held. Follows decompression aliases.
	uint64_t Current(uint64_t inode) const
	{
		for (int i = 0; i < 16; i++)
		{
			auto it = m_alias.find(inode);
			if (it == m_alias.end())
				break;
			inode = it->second;
		}
		return inode;
	}

	// Inode number behind a name, or 0.
	uint64_t InodeOf(apfsrw_node *dir, const std::string &name)
	{
		apfsrw_node *n = nullptr;
		apfsrw_stat s;
		uint64_t ino = 0;
		if (apfsrw_lookup(dir, name.data(), name.size(), &n) == 0)
		{
			if (apfsrw_getattr(n, &s) == 0)
				ino = s.ino;
			apfsrw_put(n);
		}
		return ino;
	}

	// Drop our node for an inode that lost its last name, so the engine can
	// evict it (that is what frees its blocks). Only looks in the cache: an
	// unlinked inode is never opened again by number.
	void ForgetIfUnlinked(uint64_t inode)
	{
		apfsrw_node *cached = nullptr;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			auto it = m_nodes.find(inode);
			if (it == m_nodes.end())
				return;
			cached = apfsrw_get(it->second.node);
		}
		NodeRef n(cached);
		apfsrw_stat s;
		if (apfsrw_getattr(n, &s) == 0 && s.nlink > 0)
			return;
		apfsrw_node *drop = Drop(inode);
		if (drop)
			apfsrw_put(drop);
	}

	// Remove an inode from the cache; returns its node for the caller to put.
	apfsrw_node *Drop(uint64_t inode)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_nodes.find(inode);
		if (it == m_nodes.end())
			return nullptr;
		apfsrw_node *n = it->second.node;
		m_lru.erase(it->second.lru);
		m_nodes.erase(it);
		return n;
	}

	// A held reference to the node of an inode: from the cache, or opened by
	// number (and cached). Empty if the inode doesn't exist.
	NodeRef Get(uint64_t inode)
	{
		uint64_t ino;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			ino = Current(inode);
			if (ino == kRootInode)
				return NodeRef(apfsrw_get(m_root));
			auto it = m_nodes.find(ino);
			if (it != m_nodes.end())
			{
				m_lru.splice(m_lru.end(), m_lru, it->second.lru);
				return NodeRef(apfsrw_get(it->second.node));
			}
		}
		apfsrw_node *n = nullptr;
		if (apfsrw_iget(m_mount, ino, &n))
			return NodeRef();
		Remember(ino, apfsrw_get(n));
		return NodeRef(n);
	}

	// Takes ownership of node's reference. Evicts the least recently used
	// nodes beyond kMaxNodes (their engine inodes then go on the engine's own
	// bounded unused list).
	void Remember(uint64_t inode, apfsrw_node *node)
	{
		std::vector<apfsrw_node *> evicted;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (inode == kRootInode || m_nodes.count(inode))
			{
				evicted.push_back(node);
			}
			else
			{
				m_lru.push_back(inode);
				m_nodes[inode] = Entry{ node, std::prev(m_lru.end()) };
				while (m_nodes.size() > kMaxNodes)
				{
					uint64_t old = m_lru.front();
					m_lru.pop_front();
					auto it = m_nodes.find(old);
					evicted.push_back(it->second.node);
					m_nodes.erase(it);
				}
			}
		}
		for (apfsrw_node *n : evicted)
			apfsrw_put(n);
	}

	static constexpr size_t kMaxNodes = 4096;
	struct Entry
	{
		apfsrw_node *node;
		std::list<uint64_t>::iterator lru;
	};

	apfsrw_mount *m_mount;
	apfsrw_node *m_root = nullptr;
	std::mutex m_mutex;
	std::mutex m_decompress_mutex;
	std::atomic<uint64_t> m_decompress_gen{ 0 };
	std::unordered_map<uint64_t, Entry> m_nodes;
	std::list<uint64_t> m_lru;   // least recently used first
	std::unordered_map<uint64_t, uint64_t> m_alias;   // decompressed: old inode -> new
};

} // namespace

std::unique_ptr<Volume> OpenRwVolume(const Container &c, unsigned fsid, bool readwrite, std::string &err)
{
	const VolumeInfo *info = nullptr;
	for (const auto &v : c.Volumes())
		if (v.fsid == fsid)
			info = &v;
	if (!info)
	{
		err = "no volume " + std::to_string(fsid);
		return nullptr;
	}
	if (info->encrypted)
	{
		err = "the linux-apfs-rw engine cannot open encrypted volumes";
		return nullptr;
	}
	apfsrw_mount *m = nullptr;
	int rc = apfsrw_mount_volume(Utf8ToWide(c.Path()).c_str(), c.PartitionOffset(), c.PartitionSize(), fsid, readwrite, nullptr, &m);
	if (rc)
	{
		err = "linux-apfs-rw could not mount " + info->name + " (errno " + std::to_string(-rc) + ")";
		return nullptr;
	}
	return std::unique_ptr<Volume>(new RwVolume(m, *info));
}

} // namespace winapfs
