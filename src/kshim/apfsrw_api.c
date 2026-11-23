/*
 * winapfs - read-only APFS for Windows
 * Copyright (C) 2026 winapfs contributors
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * apfsrw: the C API over the linux-apfs-rw engine. Each call plays the part
 * of the Linux VFS for one system call: it takes the locks the VFS would
 * take and calls the driver's operation tables.
 */
#include "apfsrw_internal.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

/* provided by the driver through module_init()/module_exit() */
int kshim_module_init(void);
/* kshim */
int kshim_bdev_register(const wchar_t *path, u64 offset, u64 size, bool writable, char *name, size_t name_size);
void kshim_bdev_unregister(const char *name);
struct file_system_type *kshim_get_fs_type(const char *name);
void kshim_deactivate_super(struct super_block *sb);
void kshim_set_log_sink(void (*sink)(int level, const char *msg));

struct apfsrw_mount {
	struct dentry *root;
	struct super_block *sb;
	char devname[32];
	bool readwrite;
};

static INIT_ONCE g_init_once = INIT_ONCE_STATIC_INIT;
static int g_init_err;

static BOOL CALLBACK engine_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
	g_init_err = kshim_module_init();
	return TRUE;
}

void apfsrw_set_log(apfsrw_log_fn fn)
{
	kshim_set_log_sink(fn);
}

int apfsrw_mount_volume(const wchar_t *path, uint64_t offset, uint64_t size, unsigned vol_index, bool readwrite,
			const char *snapshot, apfsrw_mount **out)
{
	struct file_system_type *fs;
	struct apfsrw_mount *m;
	struct dentry *root;
	char opts[256];
	int err;

	InitOnceExecuteOnce(&g_init_once, engine_init, NULL, NULL);
	if (g_init_err)
		return g_init_err;
	fs = kshim_get_fs_type("apfs");
	if (!fs)
		return -ENODEV;

	m = calloc(1, sizeof(*m));
	err = kshim_bdev_register(path, offset, size, readwrite, m->devname, sizeof(m->devname));
	if (err) {
		free(m);
		return err;
	}
	snprintf(opts, sizeof(opts), "vol=%u%s%s%s", vol_index, readwrite ? ",readwrite" : "",
		 snapshot ? ",snap=" : "", snapshot ? snapshot : "");

	root = fs->mount(fs, readwrite ? 0 : SB_RDONLY, m->devname, opts);
	if (IS_ERR(root)) {
		kshim_bdev_unregister(m->devname);
		free(m);
		return (int)PTR_ERR(root);
	}
	m->root = root;
	m->sb = root->d_sb;
	m->readwrite = readwrite;
	up_write(&m->sb->s_umount);   /* as the VFS does after ->mount() */
	*out = m;
	return 0;
}

int apfsrw_unmount(apfsrw_mount *m)
{
	if (!m)
		return 0;
	dput(m->root);
	kshim_deactivate_super(m->sb);
	kshim_bdev_unregister(m->devname);
	free(m);
	return 0;
}

int apfsrw_sync(apfsrw_mount *m)
{
	int err;
	down_read(&m->sb->s_umount);
	err = sync_filesystem(m->sb);
	up_read(&m->sb->s_umount);
	return err;
}

bool apfsrw_is_readwrite(apfsrw_mount *m)
{
	return !sb_rdonly(m->sb);
}

int apfsrw_get_statfs(apfsrw_mount *m, apfsrw_statfs *st)
{
	struct kstatfs k;
	int err;
	memset(&k, 0, sizeof(k));
	err = m->sb->s_op->statfs(m->root, &k);
	if (err)
		return err;
	st->block_size = (uint64_t)k.f_bsize;
	st->blocks = k.f_blocks;
	st->blocks_free = k.f_bfree;
	st->blocks_avail = k.f_bavail;
	st->files = k.f_files;
	st->files_free = k.f_ffree;
	return 0;
}

int apfsrw_root(apfsrw_mount *m, apfsrw_node **out)
{
	*out = N(dget(m->root));
	return 0;
}

int apfsrw_iget(apfsrw_mount *m, uint64_t ino, apfsrw_node **out)
{
	struct inode *inode;
	struct dentry *d;

	if (ino == apfs_ino(m->root->d_inode))
		return apfsrw_root(m, out);
	inode = apfs_iget(m->sb, ino);
	if (IS_ERR(inode))
		return (int)PTR_ERR(inode);
	d = d_obtain_alias(inode);   /* takes over the inode reference */
	if (IS_ERR(d)) {
		iput(inode);
		return (int)PTR_ERR(d);
	}
	*out = N(d);
	return 0;
}

apfsrw_node *apfsrw_get(apfsrw_node *n)
{
	return N(dget(D(n)));
}

void apfsrw_put(apfsrw_node *n)
{
	dput(D(n));
}

const char *apfsrw_name(apfsrw_node *n, size_t *len)
{
	struct dentry *d = D(n);
	if (len)
		*len = d->d_name.len;
	return (const char *)d->d_name.name;
}

int apfsrw_lookup(apfsrw_node *dir_node, const char *name, size_t len, apfsrw_node **out)
{
	struct dentry *dir = D(dir_node), *d, *res;
	struct inode *di = dir->d_inode;

	if (!S_ISDIR(di->i_mode))
		return -ENOTDIR;
	if (len == 0 || len > NAME_MAX)
		return len ? -ENAMETOOLONG : -ENOENT;
	d = kshim_d_alloc(dir, name, len);
	inode_lock_shared(di);
	res = di->i_op->lookup(di, d, 0);
	inode_unlock_shared(di);
	if (IS_ERR(res)) {
		dput(d);
		return (int)PTR_ERR(res);
	}
	if (res) {
		dput(d);
		d = res;
	}
	if (!d->d_inode) {
		dput(d);
		return -ENOENT;
	}
	*out = N(d);
	return 0;
}

int apfsrw_getattr(apfsrw_node *n, apfsrw_stat *st)
{
	struct dentry *d = D(n);
	struct inode *i = d->d_inode;
	struct path p = { NULL, d };
	struct kstat k;
	int err = 0;

	memset(&k, 0, sizeof(k));
	if (i->i_op && i->i_op->getattr)
		err = i->i_op->getattr(&nop_mnt_idmap, &p, &k, STATX_BASIC_STATS | STATX_BTIME, 0);
	else
		generic_fillattr(&nop_mnt_idmap, STATX_BASIC_STATS, i, &k);
	if (err)
		return err;
	st->ino = k.ino;
	st->mode = k.mode;
	st->nlink = k.nlink;
	st->uid = k.uid.val;
	st->gid = k.gid.val;
	st->size = (uint64_t)k.size;
	st->blocks = k.blocks;
	st->atime_ns = timespec64_to_ns(&k.atime);
	st->mtime_ns = timespec64_to_ns(&k.mtime);
	st->ctime_ns = timespec64_to_ns(&k.ctime);
	st->btime_ns = (k.result_mask & STATX_BTIME) ? timespec64_to_ns(&k.btime) : st->ctime_ns;
	st->rdev = k.rdev;
	st->attributes = k.attributes;
	st->bsd_flags = APFS_I(i)->i_bsd_flags;
	st->parent_ino = APFS_I(i)->i_parent_id;
	return 0;
}

/* ------------------------------------------------------------ files */

int apfsrw_open(apfsrw_node *n, bool write, apfsrw_file **out)
{
	struct dentry *d = D(n);
	struct inode *i = d->d_inode;
	struct apfsrw_file *af;
	int err = 0;

	if (write && sb_rdonly(i->i_sb))
		return -EROFS;
	af = calloc(1, sizeof(*af));
	af->f.f_inode = i;
	af->f.f_mapping = i->i_mapping;
	af->f.f_op = i->i_fop;
	af->f.f_path.dentry = dget(d);
	af->f.f_mode = FMODE_READ | FMODE_LSEEK | (write ? FMODE_WRITE : 0);
	af->f.f_flags = O_LARGEFILE | (write ? 2 : 0);
	if (af->f.f_op && af->f.f_op->open)
		err = af->f.f_op->open(i, &af->f);
	if (err) {
		dput(d);
		free(af);
		return err;
	}
	*out = af;
	return 0;
}

int apfsrw_release(apfsrw_file *af)
{
	int err = 0;
	if (!af)
		return 0;
	if (af->f.f_op && af->f.f_op->release)
		err = af->f.f_op->release(af->f.f_inode, &af->f);
	dput(af->f.f_path.dentry);
	free(af);
	return err;
}

int apfsrw_read(apfsrw_file *af, uint64_t off, void *buf, size_t len, size_t *got)
{
	struct inode *i = af->f.f_inode;
	u64 size = (u64)i_size_read(i);
	u8 *out = buf;

	*got = 0;
	if (S_ISDIR(i->i_mode))
		return -EISDIR;
	if (off >= size)
		return 0;
	if (len > size - off)
		len = (size_t)(size - off);

	/*
	 * Bring the range into the page cache with large device reads (the
	 * driver's readahead op, batched by mpage_readahead), plus 4 MiB ahead for
	 * a sequential reader; read_mapping_folio below then finds every page.
	 */
	{
		const u64 window = 4u << 20;
		u64 from = off, to = off + len;
		if (off == af->ra_next)
			to = max(to, off + window);
		if (to > size)
			to = size;
		if (off < af->ra_end && af->ra_end <= to && off + window >= af->ra_end)
			from = af->ra_end;   /* the start was read ahead last time */
		if (from < to) {
			pgoff_t first = (pgoff_t)(from >> PAGE_SHIFT);
			pgoff_t last = (pgoff_t)((to - 1) >> PAGE_SHIFT);
			kshim_file_readahead(af->f.f_mapping, &af->f, first, (unsigned long)(last - first + 1));
			af->ra_end = to;
		}
		af->ra_next = off + len;
	}

	while (len > 0) {
		pgoff_t idx = (pgoff_t)(off >> PAGE_SHIFT);
		size_t in = (size_t)(off & (PAGE_SIZE - 1));
		size_t n = min(len, PAGE_SIZE - in);
		struct folio *f = read_mapping_folio(af->f.f_mapping, idx, &af->f);
		if (IS_ERR(f))
			return *got ? 0 : (int)PTR_ERR(f);
		memcpy(out, (char *)f->page.data + in, n);
		folio_put(f);
		out += n;
		off += n;
		len -= n;
		*got += n;
	}
	return 0;
}

/* ------------------------------------------------------------ directories */

struct rd_ctx {
	struct dir_context ctx;
	apfsrw_dir_cb cb;
	void *user;
	bool stopped;
};

static bool rd_actor(struct dir_context *ctx, const char *name, int len, loff_t pos, u64 ino, unsigned type)
{
	struct rd_ctx *rc = container_of(ctx, struct rd_ctx, ctx);
	if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.'))
		return true;
	if (rc->cb(rc->user, name, (size_t)len, ino, type)) {
		rc->stopped = true;
		return false;
	}
	return true;
}

int apfsrw_readdir(apfsrw_node *n, apfsrw_dir_cb cb, void *user)
{
	struct dentry *d = D(n);
	struct inode *i = d->d_inode;
	struct apfsrw_file *af;
	struct rd_ctx rc;
	int err;

	if (!S_ISDIR(i->i_mode))
		return -ENOTDIR;
	err = apfsrw_open(n, false, &af);
	if (err)
		return err;
	memset(&rc, 0, sizeof(rc));
	rc.ctx.actor = rd_actor;
	rc.cb = cb;
	rc.user = user;
	inode_lock_shared(i);
	for (;;) {
		loff_t before = rc.ctx.pos;
		err = af->f.f_op->iterate_shared(&af->f, &rc.ctx);
		if (err || rc.stopped || rc.ctx.pos == before)
			break;
	}
	inode_unlock_shared(i);
	apfsrw_release(af);
	return err;
}

/* ------------------------------------------------------------ links and xattrs */

int apfsrw_readlink(apfsrw_node *n, char *buf, size_t size)
{
	struct dentry *d = D(n);
	struct inode *i = d->d_inode;
	struct delayed_call dc = { NULL, NULL };
	const char *target;
	size_t len;

	if (!S_ISLNK(i->i_mode) || !i->i_op->get_link)
		return -EINVAL;
	target = i->i_op->get_link(d, i, &dc);
	if (IS_ERR(target))
		return (int)PTR_ERR(target);
	len = strlen(target);
	if (size) {
		size_t c = min(len, size - 1);
		memcpy(buf, target, c);
		buf[c] = 0;
	}
	if (dc.fn)
		dc.fn(dc.arg);
	return (int)len;
}

static const struct xattr_handler *osx_handler(struct super_block *sb)
{
	const struct xattr_handler * const *h;
	for (h = sb->s_xattr; h && *h; h++)
		if ((*h)->prefix && !strcmp((*h)->prefix, XATTR_MAC_OSX_PREFIX))
			return *h;
	return NULL;
}

long apfsrw_listxattr(apfsrw_node *n, char *buf, size_t size)
{
	struct dentry *d = D(n);
	struct inode *i = d->d_inode;
	ssize_t total;
	char *tmp, *p;
	size_t outlen = 0;

	if (!i->i_op->listxattr)
		return 0;
	total = i->i_op->listxattr(d, NULL, 0);
	if (total <= 0)
		return (long)total;
	tmp = malloc((size_t)total);
	total = i->i_op->listxattr(d, tmp, (size_t)total);
	if (total < 0) {
		free(tmp);
		return (long)total;
	}
	/* strip the driver's fake "osx." prefix */
	for (p = tmp; p < tmp + total; p += strlen(p) + 1) {
		const char *name = p;
		size_t l;
		if (!strncmp(name, XATTR_MAC_OSX_PREFIX, XATTR_MAC_OSX_PREFIX_LEN))
			name += XATTR_MAC_OSX_PREFIX_LEN;
		l = strlen(name) + 1;
		if (size) {
			if (outlen + l > size) {
				free(tmp);
				return -ERANGE;
			}
			memcpy(buf + outlen, name, l);
		}
		outlen += l;
	}
	free(tmp);
	return (long)outlen;
}

long apfsrw_getxattr(apfsrw_node *n, const char *name, void *buf, size_t size)
{
	struct dentry *d = D(n);
	const struct xattr_handler *h = osx_handler(d->d_sb);
	if (!h || !h->get)
		return -EOPNOTSUPP;
	return h->get(h, d, d->d_inode, name, buf, size);
}

/* dir.c (winapfs addition) */
int apfs_stored_name_by_name(struct inode *dir, const struct qstr *child, u64 *ino, char *buf, size_t size);

int apfsrw_lookup_stored(apfsrw_node *dir_node, const char *name, size_t len, apfsrw_node **out, char *stored, size_t stored_size)
{
	struct dentry *dir = D(dir_node);
	struct inode *di = dir->d_inode;
	struct qstr q;
	u64 ino;
	int err;

	if (stored_size) {
		if (len == 0 || len > NAME_MAX || !S_ISDIR(di->i_mode))
			return apfsrw_lookup(dir_node, name, len, out);
		q.hash_len = 0;
		q.len = (u32)len;
		q.name = (const unsigned char *)name;
		inode_lock_shared(di);
		err = apfs_stored_name_by_name(di, &q, &ino, stored, stored_size);
		inode_unlock_shared(di);
		if (err)
			return err;
	}
	return apfsrw_lookup(dir_node, name, len, out);
}

uint64_t apfsrw_file_size(apfsrw_file *af)
{
	return (uint64_t)i_size_read(af->f.f_inode);
}

void kshim_pagecache_stats(char *buf, size_t size);

void apfsrw_debug_stats(char *buf, size_t size)
{
	kshim_pagecache_stats(buf, size);
}
