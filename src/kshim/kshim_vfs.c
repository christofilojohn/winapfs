/*
 * winapfs - read-only APFS for Windows
 * Copyright (C) 2026 winapfs contributors
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * kshim: VFS objects - inodes and the inode cache, dentries, super blocks,
 * file system registration, attribute helpers, and the generic file
 * operations the driver references in its operation tables.
 *
 * Dentries are not cached: the front-end builds one per name it resolves and
 * drops it when done. The driver only uses dentries to carry a name, a
 * parent and an inode through a call.
 */
#include <kshim.h>
#include "kshim_internal.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#define ICACHE_BUCKETS	4096

/* ------------------------------------------------------------ inode cache */

struct icache {
	struct inode *buckets[ICACHE_BUCKETS];
};

static struct icache *icache(struct super_block *sb)
{
	if (!sb->kshim_icache)
		sb->kshim_icache = calloc(1, sizeof(struct icache));
	return sb->kshim_icache;
}

static void inode_init_always(struct super_block *sb, struct inode *inode)
{
	inode->i_sb = sb;
	inode->i_blkbits = sb->s_blocksize_bits;
	inode->i_flags = 0;
	atomic_set(&inode->i_count, 1);
	inode->__i_nlink = 1;
	inode->i_uid.val = 0;
	inode->i_gid.val = 0;
	inode->i_size = 0;
	inode->i_blocks = 0;
	inode->i_bytes = 0;
	inode->i_state = 0;
	inode->i_rdev = 0;
	inode->i_op = NULL;
	inode->i_fop = NULL;
	inode->i_private = NULL;
	inode->kshim_link = NULL;
	inode->kshim_hnext = NULL;
	INIT_LIST_HEAD(&inode->kshim_lru);
	inode->i_mapping = &inode->i_data;
	kshim_mapping_init(&inode->i_data, inode);
	init_rwsem(&inode->i_rwsem);
	mutex_init(&inode->kshim_wait);
	INIT_LIST_HEAD(&inode->kshim_sb_list);
}

void inode_init_once(struct inode *inode)
{
	memset(inode, 0, sizeof(*inode));
	INIT_LIST_HEAD(&inode->kshim_sb_list);
	INIT_LIST_HEAD(&inode->kshim_lru);
}

static struct inode *alloc_inode(struct super_block *sb)
{
	struct inode *inode = sb->s_op->alloc_inode ? sb->s_op->alloc_inode(sb) : calloc(1, sizeof(*inode));
	if (!inode)
		return NULL;
	inode_init_always(sb, inode);
	mutex_lock(&sb->kshim_ilock);
	list_add_tail(&inode->kshim_sb_list, &sb->kshim_inodes);
	mutex_unlock(&sb->kshim_ilock);
	return inode;
}

void *alloc_inode_sb(struct super_block *sb, struct kmem_cache *cache, gfp_t gfp)
{
	return kmem_cache_alloc(cache, gfp);
}

static void destroy_inode(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	mutex_lock(&sb->kshim_ilock);
	list_del_init(&inode->kshim_sb_list);
	mutex_unlock(&sb->kshim_ilock);
	kshim_mapping_release_all(&inode->i_data);
	kfree(inode->kshim_link);
	if (sb->s_op->destroy_inode)
		sb->s_op->destroy_inode(inode);
	else if (sb->s_op->free_inode)
		sb->s_op->free_inode(inode);
	else
		free(inode);
}

/* remove from the hash (caller holds kshim_ilock) */
static void unhash(struct super_block *sb, struct inode *inode)
{
	struct icache *ic = icache(sb);
	struct inode **pp = &ic->buckets[inode->kshim_hashval % ICACHE_BUCKETS];
	while (*pp) {
		if (*pp == inode) {
			*pp = inode->kshim_hnext;
			inode->kshim_hnext = NULL;
			return;
		}
		pp = &(*pp)->kshim_hnext;
	}
}

static void hash(struct super_block *sb, struct inode *inode, unsigned long hashval)
{
	struct icache *ic = icache(sb);
	inode->kshim_hashval = hashval;
	inode->kshim_hnext = ic->buckets[hashval % ICACHE_BUCKETS];
	ic->buckets[hashval % ICACHE_BUCKETS] = inode;
}

/* take an inode off the unused list (caller holds kshim_ilock) */
static void lru_del(struct super_block *sb, struct inode *inode)
{
	if (!list_empty(&inode->kshim_lru)) {
		list_del_init(&inode->kshim_lru);
		sb->kshim_nr_unused--;
	}
}

/*
 * As in Linux, the inode stays hashed while the driver evicts it, and a
 * lookup that finds an I_FREEING inode waits for it to go: otherwise an iget
 * during the eviction of an unlinked inode would read it again from disk and
 * its orphan could be cleaned twice. (RwVolume opens inodes by number.)
 */
static void evict(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;

	mutex_lock(&sb->kshim_ilock);
	inode->i_state |= I_FREEING;
	lru_del(sb, inode);
	mutex_unlock(&sb->kshim_ilock);
	if (sb->s_op->evict_inode) {
		sb->s_op->evict_inode(inode);
	} else {
		truncate_inode_pages_final(&inode->i_data);
		clear_inode(inode);
	}
	mutex_lock(&sb->kshim_ilock);
	unhash(sb, inode);
	mutex_unlock(&sb->kshim_ilock);
	destroy_inode(inode);
}

static void wait_new(struct inode *inode)
{
	/* an I_NEW inode is filled in by the thread that created it */
	while (inode->i_state & I_NEW)
		SwitchToThread();
}

struct inode *iget5_locked(struct super_block *sb, unsigned long hashval, int (*test)(struct inode *, void *),
			   int (*set)(struct inode *, void *), void *data)
{
	struct icache *ic;
	struct inode *inode, *fresh = NULL;

again:
	mutex_lock(&sb->kshim_ilock);
	ic = icache(sb);
	for (inode = ic->buckets[hashval % ICACHE_BUCKETS]; inode; inode = inode->kshim_hnext) {
		if (inode->kshim_hashval == hashval && (inode->i_state & I_FREEING) && test(inode, data)) {
			/* being evicted: wait until it is gone, then look again */
			mutex_unlock(&sb->kshim_ilock);
			Sleep(1);
			goto again;
		}
		if (inode->kshim_hashval == hashval && !(inode->i_state & I_FREEING) && test(inode, data)) {
			atomic_inc(&inode->i_count);
			lru_del(sb, inode);
			mutex_unlock(&sb->kshim_ilock);
			if (fresh)
				destroy_inode(fresh);
			wait_new(inode);
			if (is_bad_inode(inode)) {
				iput(inode);
				return NULL;
			}
			return inode;
		}
	}
	if (!fresh) {
		mutex_unlock(&sb->kshim_ilock);
		fresh = alloc_inode(sb);
		if (!fresh)
			return NULL;
		goto again;
	}
	if (set(fresh, data)) {
		mutex_unlock(&sb->kshim_ilock);
		destroy_inode(fresh);
		return NULL;
	}
	fresh->i_state = I_NEW;
	hash(sb, fresh, hashval);
	mutex_unlock(&sb->kshim_ilock);
	return fresh;
}

static int test_ino(struct inode *inode, void *data)
{
	return inode->i_ino == *(unsigned long *)data;
}

static int set_ino(struct inode *inode, void *data)
{
	inode->i_ino = *(unsigned long *)data;
	return 0;
}

struct inode *iget_locked(struct super_block *sb, unsigned long ino)
{
	return iget5_locked(sb, ino, test_ino, set_ino, &ino);
}

struct inode *ilookup(struct super_block *sb, unsigned long ino)
{
	struct inode *inode;
	mutex_lock(&sb->kshim_ilock);
	for (inode = icache(sb)->buckets[ino % ICACHE_BUCKETS]; inode; inode = inode->kshim_hnext)
		if (inode->i_ino == ino && !(inode->i_state & I_FREEING)) {
			atomic_inc(&inode->i_count);
			lru_del(sb, inode);
			break;
		}
	mutex_unlock(&sb->kshim_ilock);
	if (inode)
		wait_new(inode);
	return inode;
}

int insert_inode_locked4(struct inode *inode, unsigned long hashval, int (*test)(struct inode *, void *), void *data)
{
	struct super_block *sb = inode->i_sb;
	struct inode *old;

	mutex_lock(&sb->kshim_ilock);
	for (old = icache(sb)->buckets[hashval % ICACHE_BUCKETS]; old; old = old->kshim_hnext) {
		if (old != inode && old->kshim_hashval == hashval && !(old->i_state & I_FREEING) && test(old, data)) {
			mutex_unlock(&sb->kshim_ilock);
			return -EBUSY;
		}
	}
	inode->i_state |= I_NEW;
	hash(sb, inode, hashval);
	mutex_unlock(&sb->kshim_ilock);
	return 0;
}

int insert_inode_locked(struct inode *inode)
{
	unsigned long ino = inode->i_ino;
	return insert_inode_locked4(inode, ino, test_ino, &ino);
}

void unlock_new_inode(struct inode *inode)
{
	inode->i_state &= ~I_NEW;
}

void discard_new_inode(struct inode *inode)
{
	unlock_new_inode(inode);
	iput(inode);
}

void iget_failed(struct inode *inode)
{
	make_bad_inode(inode);
	unlock_new_inode(inode);
	iput(inode);
}

struct inode *new_inode(struct super_block *sb)
{
	return alloc_inode(sb);
}

void ihold(struct inode *inode)
{
	atomic_inc(&inode->i_count);
}

/*
 * Unreferenced inodes kept cached. Beyond this many, the oldest are evicted
 * by evict_unused_work. Without a bound every inode a long session touched
 * stayed in memory until unmount (~1 KB each plus its cached pages), lookups
 * walked ever longer hash chains, and unmount got slow.
 */
#define ICACHE_MAX_UNUSED	4096

/*
 * Evicts the oldest unreferenced inodes down to 3/4 of the limit. Runs on the
 * work thread: writing a dirty inode starts a transaction, which iput's
 * callers (the transaction commit among them) can't do. Transactions hold
 * every inode they change until they commit, so an unreferenced inode has
 * normally been written already; a leftover I_DIRTY is written first.
 */
static void evict_unused_work(struct work_struct *w)
{
	struct super_block *sb = container_of(w, struct super_block, kshim_evict_work);
	long budget;

	mutex_lock(&sb->kshim_ilock);
	budget = sb->kshim_nr_unused;   /* one pass at most: a re-dirtied inode goes back on the list */
	mutex_unlock(&sb->kshim_ilock);
	for (; budget > 0; budget--) {
		struct inode *inode;
		bool last;

		mutex_lock(&sb->kshim_ilock);
		if (!(sb->s_flags & SB_ACTIVE) || sb->kshim_nr_unused <= ICACHE_MAX_UNUSED * 3 / 4 ||
		    list_empty(&sb->kshim_unused)) {
			mutex_unlock(&sb->kshim_ilock);
			return;
		}
		inode = list_first_entry(&sb->kshim_unused, struct inode, kshim_lru);
		lru_del(sb, inode);
		if (inode->i_state & (I_FREEING | I_NEW)) {
			mutex_unlock(&sb->kshim_ilock);
			continue;
		}
		atomic_inc(&inode->i_count);   /* ours while writing */
		mutex_unlock(&sb->kshim_ilock);

		if ((inode->i_state & I_DIRTY) && inode->i_nlink && !is_bad_inode(inode) && !sb_rdonly(sb) &&
		    sb->s_op->write_inode) {
			inode->i_state &= ~I_DIRTY;
			sb->s_op->write_inode(inode, NULL);
		}

		/* evict only if nobody took it meanwhile */
		mutex_lock(&sb->kshim_ilock);
		last = atomic_read(&inode->i_count) == 1 && !(inode->i_state & (I_FREEING | I_DIRTY));
		if (last) {
			atomic_set(&inode->i_count, 0);
			inode->i_state |= I_FREEING;
		}
		mutex_unlock(&sb->kshim_ilock);
		if (last)
			evict(inode);
		else
			iput(inode);   /* back on the list when its user lets go */
	}
}

/*
 * Last reference: unlinked inodes are evicted at once (that is what deletes
 * them on disk); others go on the unused list (see evict_unused_work).
 */
void iput(struct inode *inode)
{
	struct super_block *sb;

	if (!inode)
		return;
	if (!atomic_dec_and_test(&inode->i_count))
		return;
	sb = inode->i_sb;
	if (inode->i_nlink == 0 || is_bad_inode(inode) || !(sb->s_flags & SB_ACTIVE)) {
		if ((inode->i_state & I_DIRTY) && inode->i_nlink && sb->s_op->write_inode && !is_bad_inode(inode)) {
			inode->i_state &= ~I_DIRTY;
			sb->s_op->write_inode(inode, NULL);   /* may take and drop references */
		}
		/* evict unless someone took it meanwhile (their iput comes back here) */
		mutex_lock(&sb->kshim_ilock);
		if (atomic_read(&inode->i_count) != 0 || (inode->i_state & I_FREEING)) {
			mutex_unlock(&sb->kshim_ilock);
			return;
		}
		inode->i_state |= I_FREEING;
		mutex_unlock(&sb->kshim_ilock);
		evict(inode);
		return;
	}
	mutex_lock(&sb->kshim_ilock);
	if (atomic_read(&inode->i_count) == 0 && !(inode->i_state & (I_FREEING | I_NEW)) && list_empty(&inode->kshim_lru)) {
		list_add_tail(&inode->kshim_lru, &sb->kshim_unused);
		sb->kshim_nr_unused++;
		if (sb->kshim_nr_unused > ICACHE_MAX_UNUSED)
			schedule_work(&sb->kshim_evict_work);
	}
	mutex_unlock(&sb->kshim_ilock);
}

/* Unmount: evict every unreferenced inode (one pass; it was a rescan per inode). */
void evict_inodes(struct super_block *sb)
{
	for (;;) {
		struct inode **victims;
		struct list_head *pos;
		size_t n = 0, cap = 0, k;

		mutex_lock(&sb->kshim_ilock);
		list_for_each(pos, &sb->kshim_inodes)
			cap++;
		victims = malloc((cap + 1) * sizeof(*victims));
		list_for_each(pos, &sb->kshim_inodes) {
			struct inode *i = container_of(pos, struct inode, kshim_sb_list);
			if (atomic_read(&i->i_count) == 0 && !(i->i_state & I_FREEING)) {
				atomic_inc(&i->i_count);   /* held, so evicting one can't free another under us */
				lru_del(sb, i);
				victims[n++] = i;
			}
		}
		mutex_unlock(&sb->kshim_ilock);
		for (k = 0; k < n; k++) {
			struct inode *i = victims[k];
			bool last;
			mutex_lock(&sb->kshim_ilock);
			last = atomic_read(&i->i_count) == 1;
			if (last) {
				atomic_set(&i->i_count, 0);
				i->i_state |= I_FREEING;   /* only now: the driver may iget the others meanwhile */
			} else {
				atomic_dec(&i->i_count);   /* someone took it; their iput evicts it (!SB_ACTIVE) */
			}
			mutex_unlock(&sb->kshim_ilock);
			if (last)
				evict(i);
		}
		free(victims);
		if (!n)
			break;   /* evicting can release others (a dir's children); loop until none */
	}
}

static const struct inode_operations bad_iops;
static const struct file_operations bad_fops;

void make_bad_inode(struct inode *inode)
{
	inode->i_mode = S_IFREG;
	inode->i_op = &bad_iops;
	inode->i_fop = &bad_fops;
}

bool is_bad_inode(struct inode *inode)
{
	return inode->i_op == &bad_iops;
}

void clear_inode(struct inode *inode)
{
	inode->i_state = I_FREEING | I_CLEAR;
}

void mark_inode_dirty(struct inode *inode)
{
	inode->i_state |= I_DIRTY_SYNC | I_DIRTY_DATASYNC;
	if (inode->i_sb->s_op->dirty_inode)
		inode->i_sb->s_op->dirty_inode(inode, I_DIRTY_SYNC);
}

void mark_inode_dirty_sync(struct inode *inode)
{
	mark_inode_dirty(inode);
}

void set_nlink(struct inode *i, unsigned int n) { i->__i_nlink = n; }
void inc_nlink(struct inode *i) { i->__i_nlink++; }
void drop_nlink(struct inode *i) { if (i->__i_nlink) i->__i_nlink--; }
void clear_nlink(struct inode *i) { i->__i_nlink = 0; }

void inode_add_bytes(struct inode *i, loff_t bytes)
{
	inode_set_bytes(i, inode_get_bytes(i) + bytes);
}

void inode_sub_bytes(struct inode *i, loff_t bytes)
{
	inode_set_bytes(i, inode_get_bytes(i) - bytes);
}

void inode_set_bytes(struct inode *i, loff_t bytes)
{
	i->i_blocks = (blkcnt_t)(bytes >> 9);
	i->i_bytes = (unsigned short)(bytes & 511);
}

loff_t inode_get_bytes(struct inode *i)
{
	return ((loff_t)i->i_blocks << 9) + i->i_bytes;
}

void inode_init_owner(struct mnt_idmap *idmap, struct inode *i, const struct inode *dir, umode_t mode)
{
	i->i_uid = current_fsuid();
	if (dir && (dir->i_mode & S_ISGID)) {
		i->i_gid = dir->i_gid;
		if (S_ISDIR(mode))
			mode |= S_ISGID;
	} else {
		i->i_gid = current_fsgid();
	}
	i->i_mode = mode;
}

bool inode_owner_or_capable(struct mnt_idmap *idmap, const struct inode *i)
{
	return true;
}

void inode_set_flags(struct inode *i, unsigned int flags, unsigned int mask)
{
	i->i_flags = (i->i_flags & ~mask) | (flags & mask);
}

void init_special_inode(struct inode *i, umode_t mode, dev_t rdev)
{
	i->i_mode = mode;
	i->i_rdev = rdev;
}

void inode_nohighmem(struct inode *i)
{
}

struct timespec64 simple_inode_init_ts(struct inode *i)
{
	struct timespec64 t = current_time(i);
	i->__i_atime = i->__i_mtime = i->__i_ctime = t;
	return t;
}

struct timespec64 inode_set_ctime_current(struct inode *i)
{
	i->__i_ctime = current_time(i);
	return i->__i_ctime;
}

int generic_update_time(struct inode *i, int flags)
{
	struct timespec64 now = current_time(i);
	if (flags & S_ATIME)
		i->__i_atime = now;
	if (flags & S_MTIME)
		i->__i_mtime = now;
	if (flags & S_CTIME)
		i->__i_ctime = now;
	mark_inode_dirty_sync(i);
	return 0;
}

void file_update_time(struct file *f)
{
	struct inode *i = file_inode(f);
	if (i->i_op && i->i_op->update_time)
		i->i_op->update_time(i, S_MTIME | S_CTIME);
	else
		generic_update_time(i, S_MTIME | S_CTIME);
}

void file_accessed(struct file *f)
{
	/* noatime: winapfs never updates access times on reads */
}

void inode_dio_wait(struct inode *i)
{
}

/* ------------------------------------------------------------ dentries */

struct dentry *kshim_d_alloc(struct dentry *parent, const char *name, size_t len)
{
	struct dentry *d = calloc(1, sizeof(*d));
	d->kshim_name = malloc(len + 1);
	memcpy(d->kshim_name, name, len);
	d->kshim_name[len] = 0;
	d->d_name.name = (const unsigned char *)d->kshim_name;
	d->d_name.len = (u32)len;
	d->d_parent = parent ? dget(parent) : d;
	d->d_sb = parent ? parent->d_sb : NULL;
	d->d_op = d->d_sb ? d->d_sb->s_d_op : NULL;
	atomic_set(&d->kshim_count, 1);
	return d;
}

struct dentry *dget(struct dentry *d)
{
	if (d)
		atomic_inc(&d->kshim_count);
	return d;
}

struct dentry *dget_parent(struct dentry *d)
{
	return dget(d->d_parent);
}

void dput(struct dentry *d)
{
	while (d && atomic_dec_and_test(&d->kshim_count)) {
		struct dentry *parent = d->d_parent == d ? NULL : d->d_parent;
		if (d->d_inode)
			iput(d->d_inode);
		free(d->kshim_name);
		free(d);
		d = parent;
	}
}

void d_instantiate(struct dentry *d, struct inode *i)
{
	d->d_inode = i;
}

void d_instantiate_new(struct dentry *d, struct inode *i)
{
	d->d_inode = i;
	unlock_new_inode(i);
}

void d_add(struct dentry *d, struct inode *i)
{
	d->d_inode = i;
}

struct dentry *d_splice_alias(struct inode *i, struct dentry *d)
{
	if (IS_ERR(i))
		return ERR_CAST(i);
	d->d_inode = i;
	return NULL;
}

struct dentry *d_obtain_alias(struct inode *i)
{
	struct dentry *d;
	if (IS_ERR(i))
		return ERR_CAST(i);
	d = kshim_d_alloc(NULL, "", 0);
	d->d_sb = i->i_sb;
	d->d_inode = i;
	return d;
}

struct dentry *d_make_root(struct inode *root)
{
	struct dentry *d;
	if (!root)
		return NULL;
	d = kshim_d_alloc(NULL, "/", 1);
	d->d_sb = root->i_sb;
	d->d_op = root->i_sb->s_d_op;
	d->d_inode = root;
	return d;
}

void d_drop(struct dentry *d)
{
}

void set_default_d_op(struct super_block *sb, const struct dentry_operations *ops)
{
	sb->s_d_op = ops;
}

void d_set_d_op(struct dentry *d, const struct dentry_operations *ops)
{
	d->d_op = ops;
}

const char *kshim_dentry_name(const struct dentry *d)
{
	return (const char *)d->d_name.name;
}

/* ------------------------------------------------------------ super blocks */

static LIST_HEAD(g_supers);
static struct mutex g_supers_lock;
static INIT_ONCE g_supers_once = INIT_ONCE_STATIC_INIT;
static struct file_system_type *g_fs_types;

static BOOL CALLBACK supers_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
	mutex_init(&g_supers_lock);
	return TRUE;
}

int register_filesystem(struct file_system_type *fs)
{
	fs->next = g_fs_types;
	g_fs_types = fs;
	return 0;
}

int unregister_filesystem(struct file_system_type *fs)
{
	struct file_system_type **pp = &g_fs_types;
	while (*pp) {
		if (*pp == fs) {
			*pp = fs->next;
			return 0;
		}
		pp = &(*pp)->next;
	}
	return -EINVAL;
}

struct file_system_type *kshim_get_fs_type(const char *name)
{
	struct file_system_type *t;
	for (t = g_fs_types; t; t = t->next)
		if (!strcmp(t->name, name))
			return t;
	return NULL;
}

static struct super_block *alloc_super(struct file_system_type *type, int flags)
{
	struct super_block *sb = calloc(1, sizeof(*sb));
	INIT_LIST_HEAD(&sb->s_list);
	INIT_LIST_HEAD(&sb->kshim_inodes);
	INIT_LIST_HEAD(&sb->kshim_unused);
	kshim_init_work(&sb->kshim_evict_work, evict_unused_work);
	mutex_init(&sb->kshim_ilock);
	init_rwsem(&sb->s_umount);
	sb->s_type = type;
	sb->s_flags = (unsigned long)flags;
	sb->s_blocksize = 512;
	sb->s_blocksize_bits = 9;
	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_time_gran = 1;
	atomic_set(&sb->s_active, 1);
	sb->s_count = 1;
	return sb;
}

/* Returned with s_umount held for writing, like Linux. */
struct super_block *sget(struct file_system_type *type, int (*test)(struct super_block *, void *),
			 int (*set)(struct super_block *, void *), int flags, void *data)
{
	struct super_block *sb;
	struct list_head *pos;
	int err;

	InitOnceExecuteOnce(&g_supers_once, supers_init, NULL, NULL);
	mutex_lock(&g_supers_lock);
	if (test) {
		list_for_each(pos, &g_supers) {
			sb = container_of(pos, struct super_block, s_list);
			if (sb->s_type == type && test(sb, data)) {
				atomic_inc(&sb->s_active);
				mutex_unlock(&g_supers_lock);
				down_write(&sb->s_umount);
				return sb;
			}
		}
	}
	sb = alloc_super(type, flags);
	err = set(sb, data);
	if (err) {
		mutex_unlock(&g_supers_lock);
		free(sb);
		return ERR_PTR(err);
	}
	list_add_tail(&sb->s_list, &g_supers);
	mutex_unlock(&g_supers_lock);
	down_write(&sb->s_umount);
	return sb;
}

int set_anon_super(struct super_block *sb, void *data)
{
	return get_anon_bdev(&sb->s_dev);
}

int super_setup_bdi(struct super_block *sb)
{
	if (!sb->s_bdi)
		sb->s_bdi = calloc(1, sizeof(*sb->s_bdi));
	return 0;
}

/* Linux order: write dirty inodes, nonblocking sync, flush device, blocking sync. */
int sync_filesystem(struct super_block *sb)
{
	struct list_head *pos;
	int err = 0, e;

	if (sb_rdonly(sb))
		return 0;
	/* one pass collects the dirty inodes (it was a rescan per inode) */
	for (;;) {
		struct inode **dirty;
		size_t n = 0, cap = 0, k;
		mutex_lock(&sb->kshim_ilock);
		list_for_each(pos, &sb->kshim_inodes)
			cap++;
		dirty = malloc((cap + 1) * sizeof(*dirty));
		list_for_each(pos, &sb->kshim_inodes) {
			struct inode *i = container_of(pos, struct inode, kshim_sb_list);
			if ((i->i_state & I_DIRTY) && !(i->i_state & (I_FREEING | I_NEW)) && i->i_nlink) {
				atomic_inc(&i->i_count);
				lru_del(sb, i);
				dirty[n++] = i;
			}
		}
		mutex_unlock(&sb->kshim_ilock);
		for (k = 0; k < n; k++) {
			dirty[k]->i_state &= ~I_DIRTY;
			if (sb->s_op->write_inode) {
				e = sb->s_op->write_inode(dirty[k], NULL);
				if (e && !err)
					err = e;
			}
			iput(dirty[k]);
		}
		free(dirty);
		if (!n)
			break;   /* writing can dirty others; loop until clean */
	}
	if (sb->s_op->sync_fs) {
		e = sb->s_op->sync_fs(sb, 0);
		if (e && !err)
			err = e;
	}
	if (sb->s_bdev) {
		e = sync_blockdev(sb->s_bdev);
		if (e && !err)
			err = e;
	}
	if (sb->s_op->sync_fs) {
		e = sb->s_op->sync_fs(sb, 1);
		if (e && !err)
			err = e;
	}
	return err;
}

void generic_shutdown_super(struct super_block *sb)
{
	if (sb->s_root) {
		struct dentry *root = sb->s_root;
		sync_filesystem(sb);
		sb->s_flags &= ~SB_ACTIVE;   /* the eviction work stops; iput evicts at once */
		cancel_work_sync(&sb->kshim_evict_work);
		sb->s_root = NULL;
		dput(root);
		evict_inodes(sb);
		if (sb->s_op->put_super)
			sb->s_op->put_super(sb);
	}
	mutex_lock(&g_supers_lock);
	list_del_init(&sb->s_list);
	mutex_unlock(&g_supers_lock);
}

void kill_super_notify(struct super_block *sb)
{
}

void kill_anon_super(struct super_block *sb)
{
	generic_shutdown_super(sb);
	free_anon_bdev(sb->s_dev);
}

void kill_block_super(struct super_block *sb)
{
	generic_shutdown_super(sb);
}

void deactivate_locked_super(struct super_block *sb)
{
	up_write(&sb->s_umount);
	if (atomic_dec_and_test(&sb->s_active)) {
		if (sb->s_type->kill_sb)
			sb->s_type->kill_sb(sb);
		cancel_work_sync(&sb->kshim_evict_work);
		free(sb->kshim_evict_work.kshim);
		sb->kshim_evict_work.kshim = NULL;
		free(sb->kshim_icache);
		free(sb->s_bdi);
		free(sb);
	}
}

/* Drops a reference taken by mount (the counterpart of the returned dget(s_root)). */
void kshim_deactivate_super(struct super_block *sb)
{
	down_write(&sb->s_umount);
	deactivate_locked_super(sb);
}

struct dentry *mount_bdev(struct file_system_type *fs_type, int flags, const char *dev_name, void *data,
			  int (*fill_super)(struct super_block *, void *, int))
{
	return ERR_PTR(-ENOSYS);   /* the driver brings its own mount_bdev copy */
}

/* ------------------------------------------------------------ attributes */

void generic_fillattr(struct mnt_idmap *idmap, u32 request_mask, struct inode *i, struct kstat *stat)
{
	stat->dev = i->i_sb->s_dev;
	stat->ino = i->i_ino;
	stat->mode = i->i_mode;
	stat->nlink = i->i_nlink;
	stat->uid = i->i_uid;
	stat->gid = i->i_gid;
	stat->rdev = i->i_rdev;
	stat->size = i_size_read(i);
	stat->atime = i->__i_atime;
	stat->mtime = i->__i_mtime;
	stat->ctime = i->__i_ctime;
	stat->blksize = 1u << i->i_blkbits;
	stat->blocks = i->i_blocks;
	stat->result_mask |= STATX_BASIC_STATS;
}

int setattr_prepare(struct mnt_idmap *idmap, struct dentry *d, struct iattr *a)
{
	if (a->ia_valid & ATTR_SIZE)
		return inode_newsize_ok(d->d_inode, a->ia_size);
	return 0;
}

int inode_newsize_ok(const struct inode *i, loff_t offset)
{
	if (offset < 0)
		return -EINVAL;
	if (offset > i->i_sb->s_maxbytes)
		return -EFBIG;
	return 0;
}

void setattr_copy(struct mnt_idmap *idmap, struct inode *i, const struct iattr *a)
{
	unsigned int v = a->ia_valid;
	if (v & ATTR_UID)
		i->i_uid = a->ia_uid;
	if (v & ATTR_GID)
		i->i_gid = a->ia_gid;
	if (v & ATTR_ATIME)
		i->__i_atime = a->ia_atime;
	if (v & ATTR_MTIME)
		i->__i_mtime = a->ia_mtime;
	if (v & ATTR_CTIME)
		i->__i_ctime = a->ia_ctime;
	if (v & ATTR_MODE)
		i->i_mode = a->ia_mode;
}

void fileattr_fill_flags(struct fileattr *fa, u32 flags)
{
	memset(fa, 0, sizeof(*fa));
	fa->flags_valid = true;
	fa->flags = flags;
	if (flags & FS_IMMUTABLE_FL)
		fa->fsx_xflags |= FS_XFLAG_IMMUTABLE;
	if (flags & FS_APPEND_FL)
		fa->fsx_xflags |= FS_XFLAG_APPEND;
	if (flags & FS_NODUMP_FL)
		fa->fsx_xflags |= FS_XFLAG_NODUMP;
	if (flags & FS_NOATIME_FL)
		fa->fsx_xflags |= FS_XFLAG_NOATIME;
}

/* ------------------------------------------------------------ files */

int generic_file_open(struct inode *i, struct file *f)
{
	return 0;
}

int mnt_want_write_file(struct file *f)
{
	return sb_rdonly(file_inode(f)->i_sb) ? -EROFS : 0;
}

void mnt_drop_write_file(struct file *f)
{
}

bool dir_emit_dots(struct file *file, struct dir_context *ctx)
{
	if (ctx->pos == 0) {
		if (!ctx->actor(ctx, ".", 1, ctx->pos, file_inode(file)->i_ino, DT_DIR))
			return false;
		ctx->pos = 1;
	}
	if (ctx->pos == 1) {
		if (!ctx->actor(ctx, "..", 2, ctx->pos, 0, DT_DIR))
			return false;
		ctx->pos = 2;
	}
	return true;
}

/*
 * Generic file operations named in the driver's tables. winapfs drives the
 * address_space operations directly, so these are never called through the
 * tables; they exist so the tables link.
 */
loff_t generic_file_llseek(struct file *f, loff_t off, int whence) { return -EOPNOTSUPP; }
ssize_t generic_file_read_iter(struct kiocb *iocb, struct iov_iter *iter) { return -EOPNOTSUPP; }
ssize_t generic_file_write_iter(struct kiocb *iocb, struct iov_iter *from) { return -EOPNOTSUPP; }
ssize_t generic_read_dir(struct file *f, char __user *buf, size_t n, loff_t *pos) { return -EISDIR; }
ssize_t filemap_splice_read(struct file *in, loff_t *ppos, struct pipe_inode_info *pipe, size_t len, unsigned int flags) { return -EOPNOTSUPP; }
ssize_t iter_file_splice_write(struct pipe_inode_info *pipe, struct file *out, loff_t *ppos, size_t len, unsigned int flags) { return -EOPNOTSUPP; }
ssize_t splice_copy_file_range(struct file *in, loff_t pos_in, struct file *out, loff_t pos_out, size_t len) { return -EOPNOTSUPP; }
ssize_t generic_copy_file_range(struct file *in, loff_t pos_in, struct file *out, loff_t pos_out, size_t len, unsigned int flags) { return -EOPNOTSUPP; }
int generic_file_mmap(struct file *f, struct vm_area_struct *vma) { return -EOPNOTSUPP; }
int generic_remap_file_range_prep(struct file *in, loff_t pos_in, struct file *out, loff_t pos_out, loff_t *count, unsigned int remap_flags) { return 0; }
vm_fault_t filemap_fault(struct vm_fault *vmf) { return VM_FAULT_SIGBUS; }
vm_fault_t filemap_map_pages(struct vm_fault *vmf, pgoff_t start, pgoff_t end) { return 0; }
vm_fault_t vmf_fs_error(int err) { return VM_FAULT_SIGBUS; }
int generic_readlink(struct dentry *d, char __user *buf, int len) { return -EOPNOTSUPP; }
const char *page_get_link(struct dentry *d, struct inode *i, struct delayed_call *c) { return ERR_PTR(-EOPNOTSUPP); }
void kfree_link(void *p) { kfree(p); }

int fiemap_prep(struct inode *i, struct fiemap_extent_info *fi, u64 start, u64 *len, u32 supported_flags)
{
	return 0;
}

int fiemap_fill_next_extent(struct fiemap_extent_info *fi, u64 logical, u64 phys, u64 len, u32 flags)
{
	fi->fi_extents_mapped++;
	return (flags & FIEMAP_EXTENT_LAST) ? 1 : 0;
}
