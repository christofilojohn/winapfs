/*
 * winapfs - read-only APFS for Windows
 * Copyright (C) 2026 winapfs contributors
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * kshim: block devices. winapfs registers a device (an image file or a raw
 * disk, plus the byte range of the APFS partition) and gets back a name of
 * the form "kshim:N". The driver opens that name with
 * bdev_file_open_by_path(). Offsets the driver uses are relative to the
 * partition start.
 *
 * I/O is positional (OVERLAPPED offsets) so it is safe from any thread.
 * Buffers come from the page cache and are page-aligned; offsets are
 * multiples of the block size, so raw-disk sector alignment holds as long as
 * the partition itself is sector-aligned (GPT guarantees that).
 */
#include <kshim.h>
#include "kshim_internal.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <winioctl.h>

#define MAX_BDEVS	64

struct kshim_bdev {
	bool used;
	int regs;		/* registrations: volume mounts sharing this device */
	wchar_t path[MAX_PATH];
	HANDLE h;
	u64 offset;
	u64 size;
	bool writable;
	volatile LONG io_failed; /* latched fatal write/flush error */
	struct block_device bdev;
	struct gendisk disk;
	struct backing_dev_info bdi;
	struct address_space mapping;
	int opens;
};

static struct kshim_bdev g_bdevs[MAX_BDEVS];

/* KSHIM_IOSTATS=1: device I/O counts, printed when a device is released */
static volatile LONG64 g_reads, g_read_bytes, g_writes, g_write_bytes;
extern volatile LONG64 g_ra_pieces;   /* kshim_pagecache.c */
static SRWLOCK g_bdev_lock = SRWLOCK_INIT;

static int win32_err_to_errno(DWORD e)
{
	switch (e) {
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
		return -ENOENT;
	case ERROR_ACCESS_DENIED:
		return -EACCES;
	case ERROR_SHARING_VIOLATION:
	case ERROR_LOCK_VIOLATION:
		return -EBUSY;
	case ERROR_WRITE_PROTECT:
		return -EROFS;
	case ERROR_NOT_ENOUGH_MEMORY:
	case ERROR_OUTOFMEMORY:
		return -ENOMEM;
	default:
		return -EIO;
	}
}

int kshim_bdev_register(const wchar_t *path, u64 offset, u64 size, bool writable, char *name, size_t name_size)
{
	HANDLE h;
	int i;

	/*
	 * Volumes of one container share its device, so the driver shares one
	 * container instance among their mounts (as on Linux). Separate devices
	 * gave each volume its own space manager and object map: two read-write
	 * mounts of one container would have corrupted it.
	 */
	AcquireSRWLockExclusive(&g_bdev_lock);
	for (i = 0; i < MAX_BDEVS; i++) {
		struct kshim_bdev *b = &g_bdevs[i];
		if (b->used && b->offset == offset && !_wcsicmp(b->path, path)) {
			/*
			 * A read-only volume may share a writable device (the driver
			 * makes it read-only through its superblock); a read-write
			 * mount can't use a device opened read-only.
			 */
			if (writable && !b->writable) {
				ReleaseSRWLockExclusive(&g_bdev_lock);
				return -EBUSY;
			}
			b->regs++;
			snprintf(name, name_size, "kshim:%d", i);
			ReleaseSRWLockExclusive(&g_bdev_lock);
			return 0;
		}
	}
	ReleaseSRWLockExclusive(&g_bdev_lock);

	h = CreateFileW(path, GENERIC_READ | (writable ? GENERIC_WRITE : 0), FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | FILE_FLAG_RANDOM_ACCESS, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return win32_err_to_errno(GetLastError());

	if (!size) {
		LARGE_INTEGER sz;
		GET_LENGTH_INFORMATION li;
		DWORD got = 0;
		OVERLAPPED ov = { 0 };
		LONGLONG total = 0;
		if (GetFileSizeEx(h, &sz)) {
			total = sz.QuadPart;
		} else {
			/* a raw disk: ask the device (overlapped handle: wait for the result) */
			ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
			if (!ov.hEvent ||
			    (!DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &li, sizeof(li), NULL, &ov) &&
			     GetLastError() != ERROR_IO_PENDING) ||
			    !GetOverlappedResult(h, &ov, &got, TRUE)) {
				if (ov.hEvent)
					CloseHandle(ov.hEvent);
				CloseHandle(h);
				return -EINVAL;
			}
			CloseHandle(ov.hEvent);
			total = li.Length.QuadPart;
		}
		if (total <= 0 || (u64)total <= offset) {
			CloseHandle(h);
			return -EINVAL;
		}
		size = (u64)total - offset;
	}

	AcquireSRWLockExclusive(&g_bdev_lock);
	for (i = 0; i < MAX_BDEVS; i++) {
		struct kshim_bdev *existing = &g_bdevs[i];
		if (existing->used && existing->offset == offset && !_wcsicmp(existing->path, path)) {
			CloseHandle(h);
			if (writable && !existing->writable) {
				ReleaseSRWLockExclusive(&g_bdev_lock);
				return -EBUSY;
			}
			existing->regs++;
			snprintf(name, name_size, "kshim:%d", i);
			ReleaseSRWLockExclusive(&g_bdev_lock);
			return 0;
		}
	}
	for (i = 0; i < MAX_BDEVS && g_bdevs[i].used; i++)
		;
	if (i == MAX_BDEVS) {
		ReleaseSRWLockExclusive(&g_bdev_lock);
		CloseHandle(h);
		return -ENOSPC;
	}
	struct kshim_bdev *b = &g_bdevs[i];
	memset(b, 0, sizeof(*b));
	b->used = true;
	b->regs = 1;
	wcsncpy(b->path, path, MAX_PATH - 1);
	b->h = h;
	b->offset = offset;
	b->size = size;
	b->writable = writable;
	b->bdev.kshim_dev = b;
	b->bdev.bd_dev = (dev_t)(0x800000 + i);   /* fake major/minor, unique per device */
	b->bdev.bd_block_size = 4096;
	b->bdev.bd_size = size;
	b->bdev.bd_read_only = !writable;
	b->bdev.bd_disk = &b->disk;
	b->bdev.bd_mapping = &b->mapping;
	b->disk.bdi = &b->bdi;
	b->bdi.ra_pages = 1024;
	b->bdi.io_pages = 1024;
	snprintf(b->bdev.bd_name, sizeof(b->bdev.bd_name), "kshim%d", i);
	snprintf(b->disk.disk_name, sizeof(b->disk.disk_name), "kshim%d", i);
	kshim_mapping_init(&b->mapping, NULL);
	b->mapping.flags |= KSHIM_MAPPING_BDEV;
	ReleaseSRWLockExclusive(&g_bdev_lock);

	snprintf(name, name_size, "kshim:%d", i);
	return 0;
}

static struct kshim_bdev *lookup(const char *name)
{
	int i;
	if (strncmp(name, "kshim:", 6) != 0)
		return NULL;
	i = atoi(name + 6);
	if (i < 0 || i >= MAX_BDEVS || !g_bdevs[i].used)
		return NULL;
	return &g_bdevs[i];
}

void kshim_bdev_unregister(const char *name)
{
	struct kshim_bdev *b;
	AcquireSRWLockExclusive(&g_bdev_lock);
	b = lookup(name);
	if (b && b->regs > 0)
		b->regs--;
	if (b && b->regs == 0 && b->opens == 0) {
		const char *iostats = getenv("KSHIM_IOSTATS");
		if (iostats) {
			fprintf(stderr, "kshim %s: %lld reads (%lld KiB), %lld writes (%lld KiB)\n", name, (long long)g_reads,
				(long long)g_read_bytes / 1024, (long long)g_writes, (long long)g_write_bytes / 1024);
			fprintf(stderr, "kshim %s: readahead saw %lld extent pieces\n", name, (long long)g_ra_pieces);
		}
		/* nothing dirty should be left after the last close; never drop it silently */
		if (b->writable)
			sync_blockdev(&b->bdev);
		kshim_mapping_release_all(&b->mapping);
		CloseHandle(b->h);
		b->used = false;
	}
	ReleaseSRWLockExclusive(&g_bdev_lock);
}

struct kshim_bdev *kshim_bdev_of(struct block_device *bdev)
{
	return bdev->kshim_dev;
}

bool kshim_bdev_io_failed(struct block_device *bdev)
{
	struct kshim_bdev *b;
	if (!bdev || !bdev->kshim_dev)
		return false;
	b = bdev->kshim_dev;
	return InterlockedCompareExchange(&b->io_failed, 0, 0) != 0;
}

static INIT_ONCE g_fls_once = INIT_ONCE_STATIC_INIT;
static DWORD g_event_fls = FLS_OUT_OF_INDEXES;

static VOID WINAPI kshim_fls_close_event(PVOID data)
{
	if (data)
		CloseHandle((HANDLE)data);
}

static BOOL CALLBACK kshim_init_event_fls(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
	(void)once;
	(void)param;
	(void)ctx;
	g_event_fls = FlsAlloc(kshim_fls_close_event);
	return g_event_fls != FLS_OUT_OF_INDEXES;
}

static HANDLE kshim_thread_event(void)
{
	HANDLE ev;
	if (!InitOnceExecuteOnce(&g_fls_once, kshim_init_event_fls, NULL, NULL))
		return NULL;
	ev = (HANDLE)FlsGetValue(g_event_fls);
	if (!ev) {
		ev = CreateEventW(NULL, TRUE, FALSE, NULL);
		if (!ev)
			return NULL;
		if (!FlsSetValue(g_event_fls, ev)) {
			CloseHandle(ev);
			return NULL;
		}
	}
	return ev;
}

static int io(struct block_device *bdev, bool write, u64 off, void *buf, size_t len)
{
	struct kshim_bdev *b = bdev->kshim_dev;
	u8 *p = buf;

	if (write && !b->writable)
		return -EROFS;
	if (write && InterlockedCompareExchange(&b->io_failed, 0, 0) != 0)
		return -EIO;
	if (off > b->size || len > b->size - off)
		return -EIO;
	off += b->offset;
	InterlockedIncrement64(write ? &g_writes : &g_reads);
	InterlockedAdd64(write ? &g_write_bytes : &g_read_bytes, (LONG64)len);

	/* one FLS event per thread, closed automatically when the thread exits */
	HANDLE t_event = kshim_thread_event();
	if (!t_event)
		return -ENOMEM;

	while (len > 0) {
		OVERLAPPED ov = { 0 };
		DWORD chunk = len > (1u << 30) ? (1u << 30) : (DWORD)len, done = 0;
		BOOL ok;

		ov.Offset = (DWORD)off;
		ov.OffsetHigh = (DWORD)(off >> 32);
		ov.hEvent = t_event;
		ok = write ? WriteFile(b->h, p, chunk, NULL, &ov) : ReadFile(b->h, p, chunk, NULL, &ov);
		if (!ok && GetLastError() != ERROR_IO_PENDING) {
			DWORD we = GetLastError();
			if (write && InterlockedExchange(&b->io_failed, 1) == 0)
				pr_err("kshim %s: write I/O error (win32 %lu), blocking further writes\n", b->bdev.bd_name, (unsigned long)we);
			return -EIO;
		}
		ok = GetOverlappedResult(b->h, &ov, &done, TRUE);
		if (!ok || done != chunk) {
			DWORD we = GetLastError();
			if (write && InterlockedExchange(&b->io_failed, 1) == 0)
				pr_err("kshim %s: write I/O error (win32 %lu), blocking further writes\n", b->bdev.bd_name, (unsigned long)we);
			return -EIO;
		}
		p += chunk;
		off += chunk;
		len -= chunk;
	}
	return 0;
}

/* ------------------------------------------------ crash simulation (tests) */

struct pending_write {
	struct block_device *bdev;
	u64 off;
	size_t len;
	u8 *data;
};

static struct {
	int how;			/* 0 = off, else APFSRW_CRASH_* */
	u64 at;
	u32 rng;
	u64 writes;
	struct pending_write *pend;	/* APFSRW_CRASH_POWER: writes since the last flush */
	size_t npend, cap;
	SRWLOCK lock;
} g_crash = { .lock = SRWLOCK_INIT };

#define CRASH_COUNT	1
#define CRASH_KILL	2
#define CRASH_POWER	3
#define CRASH_NOBARRIER	4	/* like POWER on a disk that ignores flushes: must corrupt */
#define HOLDS_WRITES(h)	((h) >= CRASH_POWER)

void apfsrw_test_crash(int how, u64 at, u32 seed)
{
	AcquireSRWLockExclusive(&g_crash.lock);
	g_crash.how = how;
	g_crash.at = at;
	g_crash.rng = seed * 2654435761u | 1;
	g_crash.writes = 0;
	ReleaseSRWLockExclusive(&g_crash.lock);
}

u64 apfsrw_test_writes(void)
{
	u64 n;
	AcquireSRWLockShared(&g_crash.lock);
	n = g_crash.writes;
	ReleaseSRWLockShared(&g_crash.lock);
	return n;
}

static u32 crash_rand(void)
{
	u32 x = g_crash.rng;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return g_crash.rng = x;
}

/* Lock held. Writes pending entries of bdev (all of them, or a random subset) and drops them. */
static void crash_apply(struct block_device *bdev, bool subset)
{
	size_t i, keep = 0;
	for (i = 0; i < g_crash.npend; i++) {
		struct pending_write *p = &g_crash.pend[i];
		if (bdev && p->bdev != bdev) {
			g_crash.pend[keep++] = *p;
			continue;
		}
		if (!subset || (crash_rand() & 1))
			io(p->bdev, true, p->off, p->data, p->len);
		free(p->data);
	}
	g_crash.npend = keep;
}

static int crash_write(struct block_device *bdev, u64 off, const void *buf, size_t len)
{
	struct kshim_bdev *b = bdev->kshim_dev;
	int err = 0;

	AcquireSRWLockExclusive(&g_crash.lock);
	if (++g_crash.writes == g_crash.at && g_crash.how != CRASH_COUNT) {
		if (HOLDS_WRITES(g_crash.how))
			crash_apply(NULL, true);
		fflush(NULL);
		TerminateProcess(GetCurrentProcess(), 99);
	}
	if (!HOLDS_WRITES(g_crash.how)) {
		err = io(bdev, true, off, (void *)buf, len);
	} else if (!b->writable) {
		err = -EROFS;
	} else if (off > b->size || len > b->size - off) {
		err = -EIO;
	} else {
		struct pending_write p = { bdev, off, len, malloc(len) };
		if (g_crash.npend == g_crash.cap) {
			size_t cap = g_crash.cap ? g_crash.cap * 2 : 1024;
			void *n = realloc(g_crash.pend, cap * sizeof(*g_crash.pend));
			if (!n)
				abort();
			g_crash.pend = n;
			g_crash.cap = cap;
		}
		if (!p.data)
			abort();
		memcpy(p.data, buf, len);
		g_crash.pend[g_crash.npend++] = p;
	}
	ReleaseSRWLockExclusive(&g_crash.lock);
	return err;
}

/*
 * Reads see the held-back writes (in order, so the newest wins). The lock is
 * held across the device read so that a flush can't apply and drop an entry
 * in between.
 */
static int crash_read(struct block_device *bdev, u64 off, u8 *buf, size_t len)
{
	size_t i;
	int err;
	AcquireSRWLockShared(&g_crash.lock);
	err = io(bdev, false, off, buf, len);
	for (i = 0; !err && i < g_crash.npend; i++) {
		struct pending_write *p = &g_crash.pend[i];
		u64 s = max(off, p->off), e = min(off + len, p->off + p->len);
		if (p->bdev == bdev && s < e)
			memcpy(buf + (s - off), p->data + (s - p->off), (size_t)(e - s));
	}
	ReleaseSRWLockShared(&g_crash.lock);
	return err;
}

/* ------------------------------------------------------------ device I/O */

int kshim_bdev_read(struct block_device *bdev, u64 off, void *buf, size_t len)
{
	if (HOLDS_WRITES(g_crash.how))
		return crash_read(bdev, off, buf, len);
	return io(bdev, false, off, buf, len);
}

int kshim_bdev_write(struct block_device *bdev, u64 off, const void *buf, size_t len)
{
	if (g_crash.how)
		return crash_write(bdev, off, buf, len);
	return io(bdev, true, off, (void *)buf, len);
}

/* Durability barrier: everything written so far reaches stable storage. */
int kshim_bdev_flush(struct block_device *bdev)
{
	struct kshim_bdev *b = bdev->kshim_dev;
	if (!b->writable)
		return 0;
	if (InterlockedCompareExchange(&b->io_failed, 0, 0) != 0)
		return -EIO;
	kshim_unplug();   /* queued writes belong before the barrier */
	if (InterlockedCompareExchange(&b->io_failed, 0, 0) != 0)
		return -EIO;
	if (g_crash.how == CRASH_POWER) {
		AcquireSRWLockExclusive(&g_crash.lock);
		crash_apply(bdev, false);
		ReleaseSRWLockExclusive(&g_crash.lock);
	}
	if (!FlushFileBuffers(b->h)) {
		DWORD we = GetLastError();
		if (InterlockedExchange(&b->io_failed, 1) == 0)
			pr_err("kshim %s: flush barrier failed (win32 %lu), blocking further writes\n", b->bdev.bd_name, (unsigned long)we);
		return -EIO;
	}
	return 0;
}

/* ------------------------------------------------------------ Linux API */

struct file *bdev_file_open_by_path(const char *path, blk_mode_t mode, void *holder, const void *hops)
{
	struct kshim_bdev *b;
	struct file *f;

	AcquireSRWLockExclusive(&g_bdev_lock);
	b = lookup(path);
	if (b && (mode & BLK_OPEN_WRITE) && !b->writable)
		b = NULL;
	if (b)
		b->opens++;
	ReleaseSRWLockExclusive(&g_bdev_lock);
	if (!b)
		return ERR_PTR(-ENODEV);

	f = calloc(1, sizeof(*f));
	f->kshim_bdev = &b->bdev;
	f->f_mapping = &b->mapping;
	f->f_mode = FMODE_READ | ((mode & BLK_OPEN_WRITE) ? FMODE_WRITE : 0);
	return f;
}

struct block_device *file_bdev(struct file *f)
{
	return f->kshim_bdev;
}

/*
 * Like blkdev_put(): the last close writes back every dirty buffer and
 * flushes. The driver relies on this for the final block-zero superblock
 * copy (apfs_make_super_copy), which it only marks dirty.
 */
void bdev_fput(struct file *f)
{
	struct kshim_bdev *b = f->kshim_bdev->kshim_dev;
	bool last;
	AcquireSRWLockExclusive(&g_bdev_lock);
	last = --b->opens == 0;
	ReleaseSRWLockExclusive(&g_bdev_lock);
	if (last && b->writable)
		sync_blockdev(&b->bdev);
	free(f);
}

void fput(struct file *f)
{
	if (f && f->kshim_bdev)
		bdev_fput(f);
	else
		free(f);
}

int lookup_bdev(const char *path, dev_t *dev)
{
	struct kshim_bdev *b;
	AcquireSRWLockShared(&g_bdev_lock);
	b = lookup(path);
	if (b)
		*dev = b->bdev.bd_dev;
	ReleaseSRWLockShared(&g_bdev_lock);
	return b ? 0 : -ENODEV;
}

int set_blocksize(struct file *f, int size)
{
	f->kshim_bdev->bd_block_size = (unsigned)size;
	return 0;
}

int sb_set_blocksize(struct super_block *sb, int size)
{
	if (size < 512 || size > (int)PAGE_SIZE || !is_power_of_2((u64)size))
		return 0;
	sb->s_blocksize = (unsigned long)size;
	sb->s_blocksize_bits = (unsigned char)blksize_bits((unsigned)size);
	if (sb->s_bdev)
		sb->s_bdev->bd_block_size = (unsigned)size;
	return size;
}

int sb_min_blocksize(struct super_block *sb, int size)
{
	return sb_set_blocksize(sb, size < 512 ? 512 : size);
}

int sync_blockdev(struct block_device *b)
{
	int err = kshim_mapping_writeback(b->bd_mapping);
	return err ? err : kshim_bdev_flush(b);
}

int blkdev_issue_flush(struct block_device *b)
{
	return kshim_bdev_flush(b);
}

void invalidate_bdev(struct block_device *bdev)
{
	kshim_mapping_drop_clean(bdev->bd_mapping);
}

static volatile long g_anon_dev = 0x900000;

int get_anon_bdev(dev_t *p)
{
	*p = (dev_t)InterlockedIncrement(&g_anon_dev);
	return 0;
}

void free_anon_bdev(dev_t d)
{
	(void)d;
}

struct block_device *kshim_bdev_by_mapping(struct address_space *m)
{
	int i;
	struct block_device *res = NULL;
	AcquireSRWLockShared(&g_bdev_lock);
	for (i = 0; i < MAX_BDEVS; i++) {
		if (g_bdevs[i].used && &g_bdevs[i].mapping == m) {
			res = &g_bdevs[i].bdev;
			break;
		}
	}
	ReleaseSRWLockShared(&g_bdev_lock);
	return res;
}
