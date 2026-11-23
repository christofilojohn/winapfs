/*
 * winapfs - read-only APFS for Windows
 * Copyright (C) 2026 winapfs contributors
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * apfsrw: the writing half of the API. Each function plays the VFS's part
 * for one system call.
 *
 * Locking: namespace changes hold one global lock (g_ns_lock) and then the
 * directory inode locks exclusively, so cross-directory renames cannot
 * deadlock against each other. Writes and attribute changes hold the file's
 * inode lock, as generic_file_write_iter() and notify_change() do. The
 * driver adds its own container-wide semaphore around every transaction.
 */
#include "apfsrw_internal.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

static SRWLOCK g_ns_lock = SRWLOCK_INIT;

static bool name_ok(const char *name, size_t len)
{
	if (len == 0 || len > NAME_MAX || memchr(name, '/', len) || memchr(name, 0, len))
		return false;
	return !((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.'));
}

/* Dentry for name in dir, positive or negative. Caller holds the dir lock. */
static int child_dentry(struct dentry *dir, const char *name, size_t len, struct dentry **out)
{
	struct inode *di = dir->d_inode;
	struct dentry *d = kshim_d_alloc(dir, name, len), *res;

	res = di->i_op->lookup(di, d, 0);
	if (IS_ERR(res)) {
		dput(d);
		return (int)PTR_ERR(res);
	}
	if (res) {
		dput(d);
		d = res;
	}
	*out = d;
	return 0;
}

/* ------------------------------------------------------------ new names */

enum mk_kind { MK_FILE, MK_DIR, MK_SYMLINK };

static int make(apfsrw_node *dir_node, const char *name, size_t len, enum mk_kind kind, uint32_t mode,
		const char *target, apfsrw_node **out)
{
	struct dentry *dir = D(dir_node), *d = NULL;
	struct inode *di = dir->d_inode;
	int err;

	if (sb_rdonly(di->i_sb))
		return -EROFS;
	if (!S_ISDIR(di->i_mode))
		return -ENOTDIR;
	if (!name_ok(name, len))
		return -EINVAL;

	AcquireSRWLockExclusive(&g_ns_lock);
	inode_lock(di);
	err = child_dentry(dir, name, len, &d);
	if (!err && d->d_inode)
		err = -EEXIST;
	if (!err) {
		switch (kind) {
		case MK_FILE:
			err = di->i_op->create(&nop_mnt_idmap, di, d, (umode_t)(S_IFREG | (mode & 07777)), true);
			break;
		case MK_DIR:
			err = di->i_op->mkdir(&nop_mnt_idmap, di, d, (umode_t)(S_IFDIR | (mode & 07777)));
			break;
		case MK_SYMLINK:
			err = di->i_op->symlink(&nop_mnt_idmap, di, d, target);
			break;
		}
	}
	inode_unlock(di);
	ReleaseSRWLockExclusive(&g_ns_lock);

	if (!err && !d->d_inode)
		err = -EIO;
	if (err) {
		if (d)
			dput(d);
		return err;
	}
	if (out)
		*out = N(d);
	else
		dput(d);
	return 0;
}

int apfsrw_create(apfsrw_node *dir, const char *name, size_t len, uint32_t mode, apfsrw_node **out)
{
	return make(dir, name, len, MK_FILE, mode, NULL, out);
}

int apfsrw_mkdir(apfsrw_node *dir, const char *name, size_t len, uint32_t mode, apfsrw_node **out)
{
	return make(dir, name, len, MK_DIR, mode, NULL, out);
}

int apfsrw_symlink(apfsrw_node *dir, const char *name, size_t len, const char *target, apfsrw_node **out)
{
	if (!target || !*target)
		return -EINVAL;
	return make(dir, name, len, MK_SYMLINK, 0777, target, out);
}

int apfsrw_link(apfsrw_node *src_node, apfsrw_node *dir_node, const char *name, size_t len, apfsrw_node **out)
{
	struct dentry *src = D(src_node), *dir = D(dir_node), *d = NULL;
	struct inode *si = src->d_inode, *di = dir->d_inode;
	int err;

	if (sb_rdonly(di->i_sb))
		return -EROFS;
	if (S_ISDIR(si->i_mode))
		return -EPERM;
	if (!name_ok(name, len))
		return -EINVAL;

	AcquireSRWLockExclusive(&g_ns_lock);
	inode_lock(di);
	if (si != di)
		inode_lock(si);
	err = child_dentry(dir, name, len, &d);
	if (!err && d->d_inode)
		err = -EEXIST;
	if (!err)
		err = di->i_op->link(src, di, d);
	if (si != di)
		inode_unlock(si);
	inode_unlock(di);
	ReleaseSRWLockExclusive(&g_ns_lock);

	if (err) {
		if (d)
			dput(d);
		return err;
	}
	if (out)
		*out = N(d);
	else
		dput(d);
	return 0;
}

/* ------------------------------------------------------------ removing names */

static int remove_name(apfsrw_node *dir_node, const char *name, size_t len, bool is_dir)
{
	struct dentry *dir = D(dir_node), *d = NULL;
	struct inode *di = dir->d_inode, *vi;
	int err;

	if (sb_rdonly(di->i_sb))
		return -EROFS;
	if (!name_ok(name, len))
		return -EINVAL;

	AcquireSRWLockExclusive(&g_ns_lock);
	inode_lock(di);
	err = child_dentry(dir, name, len, &d);
	if (!err && !d->d_inode)
		err = -ENOENT;
	if (!err) {
		vi = d->d_inode;
		if (is_dir && !S_ISDIR(vi->i_mode)) {
			err = -ENOTDIR;
		} else if (!is_dir && S_ISDIR(vi->i_mode)) {
			err = -EISDIR;
		} else {
			inode_lock(vi);
			err = is_dir ? di->i_op->rmdir(di, d) : di->i_op->unlink(di, d);
			inode_unlock(vi);
		}
	}
	inode_unlock(di);
	ReleaseSRWLockExclusive(&g_ns_lock);
	/* the last reference to an unlinked inode evicts it, which frees it on disk */
	if (d)
		dput(d);
	return err;
}

int apfsrw_unlink(apfsrw_node *dir, const char *name, size_t len)
{
	return remove_name(dir, name, len, false);
}

int apfsrw_rmdir(apfsrw_node *dir, const char *name, size_t len)
{
	return remove_name(dir, name, len, true);
}

/* ------------------------------------------------------------ rename */

/* Is `victim` the directory `dir` or one of its ancestors? */
static bool is_ancestor(struct inode *victim, struct inode *dir)
{
	struct super_block *sb = dir->i_sb;
	struct inode *cur = dir;
	bool found = false;
	int depth = 0;

	ihold(cur);
	while (depth++ < 4096) {
		struct inode *next;
		if (cur == victim) {
			found = true;
			break;
		}
		if (apfs_ino(cur) == APFS_ROOT_DIR_INO_NUM)
			break;
		next = apfs_iget(sb, APFS_I(cur)->i_parent_id);
		iput(cur);
		if (IS_ERR(next))
			return false;
		cur = next;
	}
	iput(cur);
	return found;
}

int apfsrw_rename(apfsrw_node *olddir_node, const char *oldname, size_t oldlen,
		  apfsrw_node *newdir_node, const char *newname, size_t newlen, unsigned flags)
{
	struct dentry *odir = D(olddir_node), *ndir = D(newdir_node), *od = NULL, *nd = NULL;
	struct inode *odi = odir->d_inode, *ndi = ndir->d_inode, *first, *second;
	struct inode *src = NULL, *tgt = NULL;
	bool noop = false;
	int err;

	if (sb_rdonly(odi->i_sb))
		return -EROFS;
	if (!name_ok(oldname, oldlen) || !name_ok(newname, newlen))
		return -EINVAL;

	AcquireSRWLockExclusive(&g_ns_lock);
	first = odi < ndi ? odi : ndi;
	second = odi < ndi ? ndi : odi;
	inode_lock(first);
	if (second != first)
		inode_lock(second);

	err = child_dentry(odir, oldname, oldlen, &od);
	if (!err && !od->d_inode)
		err = -ENOENT;
	if (!err)
		err = child_dentry(ndir, newname, newlen, &nd);
	if (!err) {
		src = od->d_inode;
		tgt = nd->d_inode;
		if (tgt == src && odi == ndi && oldlen == newlen && !memcmp(oldname, newname, oldlen))
			noop = true;	/* same name: nothing to do */
		else if (tgt && tgt != src && (flags & APFSRW_RENAME_NOREPLACE))
			err = -EEXIST;
		else if (tgt && tgt != src && S_ISDIR(src->i_mode) && !S_ISDIR(tgt->i_mode))
			err = -ENOTDIR;
		else if (tgt && tgt != src && !S_ISDIR(src->i_mode) && S_ISDIR(tgt->i_mode))
			err = -EISDIR;
		else if (S_ISDIR(src->i_mode) && odi != ndi && is_ancestor(src, ndi))
			err = -EINVAL;	/* into its own subtree */
		else if (tgt == src)
			noop = true;	/* another link to the same inode (POSIX: no-op) */
	}
	/*
	 * A case-only rename on a case-insensitive volume finds the source
	 * itself as the target; the driver handles that as a plain rename.
	 */
	if (!err && !noop) {
		inode_lock(src);
		if (tgt && tgt != src)
			inode_lock(tgt);
		err = odi->i_op->rename(&nop_mnt_idmap, odi, od, ndi, nd,
					(flags & APFSRW_RENAME_NOREPLACE) ? RENAME_NOREPLACE : 0);
		if (tgt && tgt != src)
			inode_unlock(tgt);
		inode_unlock(src);
	}

	if (second != first)
		inode_unlock(second);
	inode_unlock(first);
	ReleaseSRWLockExclusive(&g_ns_lock);
	if (od)
		dput(od);
	if (nd)
		dput(nd);	/* a replaced target loses its last name here */
	return err;
}

/* ------------------------------------------------------------ file data */

/* generic_perform_write(): one page at a time through write_begin/write_end */
int apfsrw_write(apfsrw_file *af, uint64_t off, const void *buf, size_t len, size_t *written)
{
	struct inode *inode = af->f.f_inode;
	struct address_space *mapping = af->f.f_mapping;
	const u8 *src = buf;
	int err = 0;

	*written = 0;
	if (!(af->f.f_mode & FMODE_WRITE))
		return -EBADF;
	if (S_ISDIR(inode->i_mode))
		return -EISDIR;
	if (!mapping->a_ops->write_begin || !mapping->a_ops->write_end)
		return -EOPNOTSUPP;	/* decmpfs-compressed files */
	if (len == 0)
		return 0;

	inode_lock(inode);
	file_update_time(&af->f);
	while (len > 0) {
		size_t in = (size_t)(off & (PAGE_SIZE - 1));
		unsigned bytes = (unsigned)min(len, PAGE_SIZE - in);
		struct folio *folio = NULL;
		void *fsdata = NULL;
		int status;

		err = mapping->a_ops->write_begin(&af->f, mapping, (loff_t)off, bytes, &folio, &fsdata);
		if (err)
			break;
		memcpy((char *)folio->page.data + in, src, bytes);
		status = mapping->a_ops->write_end(&af->f, mapping, (loff_t)off, bytes, bytes, folio, fsdata);
		if (status < 0) {
			err = status;
			break;
		}
		if (status == 0) {
			err = -EIO;
			break;
		}
		src += status;
		off += (uint64_t)status;
		len -= (size_t)status;
		*written += (size_t)status;
	}
	inode_unlock(inode);
	return *written ? 0 : err;
}

int apfsrw_fsync(apfsrw_file *af)
{
	if (!af->f.f_op || !af->f.f_op->fsync)
		return 0;
	return af->f.f_op->fsync(&af->f, 0, LLONG_MAX, 0);
}

/* notify_change(): fill in the times the VFS would, then ->setattr */
/*
 * Creation time and BSD flags (bits in mask take those of flags). The driver
 * writes both from apfs_inode_info when the inode joins a transaction.
 * Caller holds the inode lock.
 */
static int set_crtime_flags(struct inode *inode, bool set_crtime, s64 crtime_ns, u32 flags, u32 mask)
{
	struct super_block *sb = inode->i_sb;
	struct apfs_inode_info *ai = APFS_I(inode);
	int err = apfs_transaction_start(sb, APFS_TRANS_REG);

	if (err)
		return err;
	apfs_inode_join_transaction(sb, inode);
	if (set_crtime)
		ai->i_crtime = ns_to_timespec64(crtime_ns);
	ai->i_bsd_flags = (ai->i_bsd_flags & ~mask) | (flags & mask);
	err = apfs_transaction_commit(sb);
	if (err)
		apfs_transaction_abort(sb);
	return err;
}

int apfsrw_setattr(apfsrw_node *n, const apfsrw_setattr_t *a)
{
	struct dentry *d = D(n);
	struct inode *inode = d->d_inode;
	struct timespec64 now = current_time(inode);
	struct iattr ia;
	int err;

	if (sb_rdonly(inode->i_sb))
		return -EROFS;
	memset(&ia, 0, sizeof(ia));
	if (a->mask & APFSRW_SET_MODE) {
		ia.ia_valid |= ATTR_MODE;
		ia.ia_mode = (umode_t)((inode->i_mode & S_IFMT) | (a->mode & 07777));
	}
	if (a->mask & APFSRW_SET_UID) {
		ia.ia_valid |= ATTR_UID;
		ia.ia_uid.val = a->uid;
	}
	if (a->mask & APFSRW_SET_GID) {
		ia.ia_valid |= ATTR_GID;
		ia.ia_gid.val = a->gid;
	}
	if (a->mask & APFSRW_SET_SIZE) {
		if (S_ISDIR(inode->i_mode))
			return -EISDIR;
		ia.ia_valid |= ATTR_SIZE | ATTR_MTIME;	/* do_truncate() */
		ia.ia_size = (loff_t)a->size;
		ia.ia_mtime = now;
	}
	if (a->mask & APFSRW_SET_ATIME) {
		ia.ia_valid |= ATTR_ATIME | ATTR_ATIME_SET;
		ia.ia_atime = ns_to_timespec64(a->atime_ns);
	}
	if (a->mask & APFSRW_SET_MTIME) {
		ia.ia_valid |= ATTR_MTIME | ATTR_MTIME_SET;
		ia.ia_mtime = ns_to_timespec64(a->mtime_ns);
	}
	ia.ia_valid |= ATTR_CTIME;
	ia.ia_ctime = now;

	inode_lock(inode);
	if (inode->i_op->setattr) {
		err = inode->i_op->setattr(&nop_mnt_idmap, d, &ia);
	} else {
		setattr_copy(&nop_mnt_idmap, inode, &ia);
		mark_inode_dirty(inode);
		err = 0;
	}
	if (!err && (a->mask & (APFSRW_SET_CRTIME | APFSRW_SET_FLAGS)))
		err = set_crtime_flags(inode, (a->mask & APFSRW_SET_CRTIME) != 0, a->crtime_ns,
				       a->flags, (a->mask & APFSRW_SET_FLAGS) ? a->flags_mask & APFSRW_USER_FLAGS : 0);
	inode_unlock(inode);
	return err;
}

/* ------------------------------------------------------------ xattrs */

static const struct xattr_handler *osx_xattr_handler(struct super_block *sb)
{
	const struct xattr_handler * const *h;
	for (h = sb->s_xattr; h && *h; h++)
		if ((*h)->prefix && !strcmp((*h)->prefix, XATTR_MAC_OSX_PREFIX))
			return *h;
	return NULL;
}

int apfsrw_setxattr(apfsrw_node *n, const char *name, const void *value, size_t size, int flags)
{
	struct dentry *d = D(n);
	struct inode *inode = d->d_inode;
	const struct xattr_handler *h = osx_xattr_handler(d->d_sb);
	size_t len = name ? strlen(name) : 0;
	int lflags = 0, err;

	if (sb_rdonly(inode->i_sb))
		return -EROFS;
	if (len == 0)
		return -EINVAL;
	if (len > 127)
		return -ENAMETOOLONG;
	/* xattrs that carry file system state (symlink targets, compression) */
	if (!strncmp(name, "com.apple.fs.", 13) || !strcmp(name, APFS_XATTR_NAME_COMPRESSED) ||
	    (!strcmp(name, APFS_XATTR_NAME_RSRC_FORK) && (APFS_I(inode)->i_bsd_flags & APFS_INOBSD_COMPRESSED)))
		return -EPERM;
	if (!h || !h->set)
		return -EOPNOTSUPP;
	if (flags & APFSRW_XATTR_CREATE)
		lflags |= XATTR_CREATE;
	if (flags & APFSRW_XATTR_REPLACE)
		lflags |= XATTR_REPLACE;
	inode_lock(inode);
	err = h->set(h, &nop_mnt_idmap, d, inode, name, value, size, lflags);
	inode_unlock(inode);
	return err;
}

int apfsrw_removexattr(apfsrw_node *n, const char *name)
{
	return apfsrw_setxattr(n, name, NULL, 0, APFSRW_XATTR_REPLACE);
}

/* ------------------------------------------------------------ decompression */

/*
 * The driver never writes compressed (decmpfs) files. To write one, it is
 * replaced by an uncompressed copy: a temporary file in the same directory
 * gets the data, the xattrs other than the compression ones, the owner,
 * mode, times and flags, and is then renamed over the original. The rename
 * is atomic, so a crash leaves either file intact (plus, at worst, the
 * temporary, which the next attempt removes).
 */
int apfsrw_decompress(apfsrw_node *dir, const char *name, size_t len, apfsrw_node **out)
{
	enum { CHUNK = 4 << 20 };
	apfsrw_node *src = NULL, *tmp = NULL;
	apfsrw_file *in = NULL, *outf = NULL;
	apfsrw_stat st;
	char tname[64];
	char *buf = NULL, *names = NULL, *p;
	long nlen = 0;
	u64 off = 0;
	int err;

	if (out)
		*out = NULL;
	err = apfsrw_lookup(dir, name, len, &src);
	if (err)
		return err;
	err = apfsrw_getattr(src, &st);
	if (err)
		goto done;
	if ((st.mode & 0170000) != 0100000 || !(st.bsd_flags & APFS_INOBSD_COMPRESSED)) {
		err = -EINVAL;
		goto done;
	}
	if (st.nlink > 1) {
		err = -EMLINK;	/* the copy would split the hard links */
		goto done;
	}
	snprintf(tname, sizeof(tname), ".winapfs-decompress-%llx", (unsigned long long)st.ino);
	apfsrw_unlink(dir, tname, strlen(tname));
	err = apfsrw_create(dir, tname, strlen(tname), st.mode & 07777, &tmp);
	if (err)
		goto done;

	buf = malloc(CHUNK);
	err = buf ? apfsrw_open(src, false, &in) : -ENOMEM;
	if (!err)
		err = apfsrw_open(tmp, true, &outf);
	while (!err) {
		size_t got = 0, w = 0;
		err = apfsrw_read(in, off, buf, CHUNK, &got);
		if (err || !got)
			break;
		err = apfsrw_write(outf, off, buf, got, &w);
		if (!err && w != got)
			err = -EIO;
		off += got;
	}
	if (!err && off != st.size)
		err = -EIO;
	if (in)
		apfsrw_release(in);
	if (outf)
		apfsrw_release(outf);

	if (!err)
		nlen = apfsrw_listxattr(src, NULL, 0);
	if (nlen < 0)
		err = (int)nlen;
	if (!err && nlen > 0) {
		names = malloc((size_t)nlen);
		nlen = names ? apfsrw_listxattr(src, names, (size_t)nlen) : -ENOMEM;
		if (nlen < 0)
			err = (int)nlen;
		for (p = names; !err && p < names + nlen; p += strlen(p) + 1) {
			long vs;
			void *v;
			if (!strcmp(p, APFS_XATTR_NAME_COMPRESSED) || !strcmp(p, APFS_XATTR_NAME_RSRC_FORK) ||
			    !strncmp(p, "com.apple.fs.", 13))
				continue;
			vs = apfsrw_getxattr(src, p, NULL, 0);
			if (vs < 0) {
				err = (int)vs;
				break;
			}
			v = malloc(vs ? (size_t)vs : 1);
			if (!v) {
				err = -ENOMEM;
				break;
			}
			if (vs > 0)
				vs = apfsrw_getxattr(src, p, v, (size_t)vs);
			err = vs < 0 ? (int)vs : apfsrw_setxattr(tmp, p, v, (size_t)vs, APFSRW_XATTR_CREATE);
			free(v);
		}
	}

	if (!err) {
		struct inode *ti = D(tmp)->d_inode;
		apfsrw_setattr_t a;
		memset(&a, 0, sizeof(a));
		a.mask = APFSRW_SET_MODE | APFSRW_SET_UID | APFSRW_SET_GID | APFSRW_SET_ATIME | APFSRW_SET_MTIME;
		a.mode = st.mode & 07777;
		a.uid = st.uid;
		a.gid = st.gid;
		a.atime_ns = st.atime_ns;
		a.mtime_ns = st.mtime_ns;
		err = apfsrw_setattr(tmp, &a);
		if (!err) {
			inode_lock(ti);
			err = set_crtime_flags(ti, true, st.btime_ns, st.bsd_flags & ~APFS_INOBSD_COMPRESSED, ~0u);
			inode_unlock(ti);
		}
	}
	if (!err)
		err = apfsrw_rename(dir, tname, strlen(tname), dir, name, len, 0);
	if (err && tmp)
		apfsrw_unlink(dir, tname, strlen(tname));
	if (!err && out) {
		*out = tmp;
		tmp = NULL;
	}

done:
	free(names);
	free(buf);
	if (tmp)
		apfsrw_put(tmp);
	if (src)
		apfsrw_put(src);
	return err;
}
