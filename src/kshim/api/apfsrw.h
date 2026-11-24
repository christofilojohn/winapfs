/*
 * winapfs - read-only APFS for Windows
 * Copyright (C) 2026 winapfs contributors
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * apfsrw: C API over the linux-apfs-rw engine running on kshim. This is what
 * the winapfs front-ends call; it hides every kernel type.
 *
 * Errors are negative errno values. Names are raw on-disk UTF-8 bytes.
 * A node is a referenced dentry+inode; release it with apfsrw_put().
 * Thread safety: the engine serializes metadata operations internally (one
 * container-wide semaphore), so calls may come from several threads.
 */
#ifndef APFSRW_H
#define APFSRW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct apfsrw_mount apfsrw_mount;
typedef struct apfsrw_node apfsrw_node;
typedef struct apfsrw_file apfsrw_file;

typedef struct apfsrw_stat {
	uint64_t ino;
	uint32_t mode;          /* st_mode incl. S_IFMT */
	uint32_t nlink;
	uint32_t uid, gid;
	uint64_t size;
	uint64_t blocks;        /* 512-byte units */
	int64_t atime_ns, mtime_ns, ctime_ns, btime_ns;
	uint32_t rdev;
	uint64_t attributes;    /* STATX_ATTR_* (compressed 0x4, immutable 0x10, append 0x20, nodump 0x40) */
	uint32_t bsd_flags;      /* APFS BSD flags (UF_HIDDEN, UF_IMMUTABLE, UF_COMPRESSED, ...) */
	uint64_t parent_ino;     /* primary parent directory (a hard link's first name) */
} apfsrw_stat;

typedef struct apfsrw_statfs {
	uint64_t block_size;
	uint64_t blocks;
	uint64_t blocks_free;
	uint64_t blocks_avail;
	uint64_t files;
	uint64_t files_free;
} apfsrw_statfs;

/* d_type values passed to readdir callbacks */
#define APFSRW_DT_FIFO	1
#define APFSRW_DT_CHR	2
#define APFSRW_DT_DIR	4
#define APFSRW_DT_BLK	6
#define APFSRW_DT_REG	8
#define APFSRW_DT_LNK	10
#define APFSRW_DT_SOCK	12

typedef void (*apfsrw_log_fn)(int level, const char *msg);
void apfsrw_set_log(apfsrw_log_fn fn);

/*
 * Mount volume vol_index (index in the container's volume array) of the APFS
 * container stored at [offset, offset+size) of path (image file or raw disk;
 * size 0 = to the end). readwrite needs the device to be writable.
 * snapshot: snapshot label to mount read-only, or NULL.
 */
int apfsrw_mount_volume(const wchar_t *path, uint64_t offset, uint64_t size, unsigned vol_index, bool readwrite,
			const char *snapshot, apfsrw_mount **out);
/* Commits everything, writes the unmount time, and releases the device. */
int apfsrw_unmount(apfsrw_mount *m);
int apfsrw_sync(apfsrw_mount *m);
int apfsrw_get_statfs(apfsrw_mount *m, apfsrw_statfs *st);
bool apfsrw_is_readwrite(apfsrw_mount *m);
bool apfsrw_io_failed(apfsrw_mount *m);

int apfsrw_root(apfsrw_mount *m, apfsrw_node **out);
/* -ENOENT if the name does not exist */
int apfsrw_lookup(apfsrw_node *dir, const char *name, size_t len, apfsrw_node **out);
/* Like apfsrw_lookup, but also returns the name as stored on disk (its case and
 * normalization may differ from name on insensitive volumes). */
int apfsrw_lookup_stored(apfsrw_node *dir, const char *name, size_t len, apfsrw_node **out, char *stored, size_t stored_size);
/* A node for an inode number (no name: usable for everything but apfsrw_name). */
int apfsrw_iget(apfsrw_mount *m, uint64_t ino, apfsrw_node **out);
apfsrw_node *apfsrw_get(apfsrw_node *n);
void apfsrw_put(apfsrw_node *n);
int apfsrw_getattr(apfsrw_node *n, apfsrw_stat *st);
const char *apfsrw_name(apfsrw_node *n, size_t *len);   /* stored name as found by lookup */

/* Calls cb for each entry except "." and ".."; cb returns nonzero to stop. */
typedef int (*apfsrw_dir_cb)(void *ctx, const char *name, size_t len, uint64_t ino, unsigned dtype);
int apfsrw_readdir(apfsrw_node *dir, apfsrw_dir_cb cb, void *ctx);

int apfsrw_open(apfsrw_node *n, bool write, apfsrw_file **out);
int apfsrw_read(apfsrw_file *f, uint64_t off, void *buf, size_t len, size_t *got);
int apfsrw_release(apfsrw_file *f);
uint64_t apfsrw_file_size(apfsrw_file *f);   /* current size (changes as the file is written) */

/* Returns the target length, or a negative errno; buf gets a NUL-terminated prefix. */
int apfsrw_readlink(apfsrw_node *n, char *buf, size_t size);

/* xattr names are on-disk names (no "osx." prefix). Return length or -errno.
 * With size 0, return the required size. listxattr fills NUL-separated names. */
long apfsrw_listxattr(apfsrw_node *n, char *buf, size_t size);
long apfsrw_getxattr(apfsrw_node *n, const char *name, void *buf, size_t size);

/* ------------------------------------------------------------ writing
 * Only on a mount made with readwrite=true. Namespace operations take the
 * parent directory and a name (never a cached node: hard links have several
 * names, and a rename changes them). The driver runs each operation in its
 * own transaction and commits in the background; apfsrw_sync() or unmount
 * makes everything durable.
 */
int apfsrw_create(apfsrw_node *dir, const char *name, size_t len, uint32_t mode, apfsrw_node **out);	/* -EEXIST */
int apfsrw_mkdir(apfsrw_node *dir, const char *name, size_t len, uint32_t mode, apfsrw_node **out);
int apfsrw_symlink(apfsrw_node *dir, const char *name, size_t len, const char *target, apfsrw_node **out);
int apfsrw_link(apfsrw_node *src, apfsrw_node *dir, const char *name, size_t len, apfsrw_node **out);
int apfsrw_unlink(apfsrw_node *dir, const char *name, size_t len);
int apfsrw_rmdir(apfsrw_node *dir, const char *name, size_t len);	/* -ENOTEMPTY */
#define APFSRW_RENAME_NOREPLACE	1
int apfsrw_rename(apfsrw_node *olddir, const char *oldname, size_t oldlen,
		  apfsrw_node *newdir, const char *newname, size_t newlen, unsigned flags);

/* Writes len bytes at off (extending the file as needed). */
int apfsrw_write(apfsrw_file *f, uint64_t off, const void *buf, size_t len, size_t *written);
int apfsrw_fsync(apfsrw_file *f);

#define APFSRW_SET_MODE		0x01
#define APFSRW_SET_UID		0x02
#define APFSRW_SET_GID		0x04
#define APFSRW_SET_SIZE		0x08
#define APFSRW_SET_ATIME	0x10
#define APFSRW_SET_MTIME	0x20
#define APFSRW_SET_CRTIME	0x40	/* creation time (no Linux equivalent) */
#define APFSRW_SET_FLAGS	0x80	/* BSD flags: bits in flags_mask become those of flags */
typedef struct apfsrw_setattr_t {
	uint32_t mask;
	uint32_t mode;          /* permission bits only */
	uint32_t uid, gid;
	uint64_t size;
	int64_t atime_ns, mtime_ns;
	int64_t crtime_ns;
	uint32_t flags, flags_mask;	/* only APFSRW_USER_FLAGS may change */
} apfsrw_setattr_t;
/* BSD flags a user may change: UF_NODUMP, UF_HIDDEN */
#define APFSRW_USER_FLAGS	0x00008001u

/*
 * Extended attributes (on-disk names, no "osx." prefix; at most 127 bytes).
 * value NULL removes the attribute. Names the file system owns
 * (com.apple.fs.*, com.apple.decmpfs, and com.apple.ResourceFork of a
 * compressed file) are refused with -EPERM.
 */
#define APFSRW_XATTR_CREATE	1	/* fail with -EEXIST if it exists */
#define APFSRW_XATTR_REPLACE	2	/* fail with -ENODATA if it doesn't */
int apfsrw_setxattr(apfsrw_node *n, const char *name, const void *value, size_t size, int flags);
int apfsrw_removexattr(apfsrw_node *n, const char *name);

/*
 * The driver can't write compressed (decmpfs) files: this replaces the
 * compressed file `name` in dir by an uncompressed copy with the same data,
 * xattrs, owner, mode, times and flags (atomically, by a rename). *out (may
 * be NULL) is the new file. -EINVAL if the file isn't compressed, -EMLINK if
 * it has more than one name.
 */
int apfsrw_decompress(apfsrw_node *dir, const char *name, size_t len, apfsrw_node **out);
/* ctime is always set to now, as the Linux VFS does. */
int apfsrw_setattr(apfsrw_node *n, const apfsrw_setattr_t *a);

/*
 * Crash simulation, for tests only. From now on device writes are counted
 * (starting at 1); on write number `at` the process dies with exit code 99:
 *   APFSRW_CRASH_COUNT  never crash, only count
 *   APFSRW_CRASH_KILL   the writes before `at` all reached the device (a
 *                       process crash; the OS keeps what was written)
 *   APFSRW_CRASH_POWER  writes since the last flush are held back, and a
 *                       random subset of them (seeded) reaches the device (a
 *                       power cut with a volatile disk cache)
 */
#define APFSRW_CRASH_COUNT	1
#define APFSRW_CRASH_KILL	2
#define APFSRW_CRASH_POWER	3
#define APFSRW_CRASH_NOBARRIER	4	/* POWER on a disk that ignores flushes (must corrupt) */
void apfsrw_test_crash(int how, uint64_t at, uint32_t seed);
/* Debugging: page-cache contents (kshim_pagecache_stats). */
void apfsrw_debug_stats(char *buf, size_t size);
uint64_t apfsrw_test_writes(void);

#ifdef __cplusplus
}
#endif

#endif
