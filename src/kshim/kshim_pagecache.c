/*
 * winapfs - read-only APFS for Windows
 * Copyright (C) 2026 winapfs contributors
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * kshim: page cache and buffer heads.
 *
 * Every address space (a file's data, or a block device) maps page indexes
 * to folios. A folio is exactly one 4 KiB page here; its data is page-aligned
 * so raw-disk I/O stays aligned. Buffer heads hang off a folio as a ring
 * (b_this_page), as in Linux: block-device buffers are created by
 * __getblk(), file buffers by create_empty_buffers() and mapped to disk by
 * the driver's get_block callback.
 *
 * I/O is synchronous: submit_bh() performs the read or write and calls
 * b_end_io before returning.
 *
 * Reclaim: once the cache holds more than a set number of folios, clean
 * unreferenced folios whose buffers are idle are dropped (oldest mappings
 * first). There is no writeback thread; dirty data is only written when the
 * driver asks (transaction commit, sync_dirty_buffer, filemap_write_and_wait).
 */
#include <kshim.h>
#include "kshim_internal.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <malloc.h>

#define RECLAIM_THRESHOLD	(64 * 1024)	/* folios (256 MiB) */
#define RECLAIM_TARGET		(48 * 1024)
#define RECLAIM_BACKOFF		4096		/* new folios before another pass that fell short */

#define KSHIM_IO_BATCH_BYTES	(4u << 20)	/* 4 MiB aligned I/O batch size */
#define BOUNCE_POOL_MAX		4

static SRWLOCK g_bounce_lock = SRWLOCK_INIT;
static void *g_bounce_pool[BOUNCE_POOL_MAX];
static size_t g_bounce_pool_n;

/* Reuses 4 MiB page-aligned bounce buffers across I/O batches to avoid per-batch VirtualAlloc/VirtualFree TLB shootdowns. */
static void *kshim_bounce_alloc(size_t bytes)
{
	if (bytes <= KSHIM_IO_BATCH_BYTES) {
		void *buf = NULL;
		AcquireSRWLockExclusive(&g_bounce_lock);
		if (g_bounce_pool_n > 0)
			buf = g_bounce_pool[--g_bounce_pool_n];
		ReleaseSRWLockExclusive(&g_bounce_lock);
		if (buf)
			return buf;
		return _aligned_malloc(KSHIM_IO_BATCH_BYTES, PAGE_SIZE);
	}
	return _aligned_malloc(bytes, PAGE_SIZE);
}

static void kshim_bounce_free(void *buf, size_t bytes)
{
	if (!buf)
		return;
	if (bytes <= KSHIM_IO_BATCH_BYTES) {
		AcquireSRWLockExclusive(&g_bounce_lock);
		if (g_bounce_pool_n < BOUNCE_POOL_MAX) {
			g_bounce_pool[g_bounce_pool_n++] = buf;
			ReleaseSRWLockExclusive(&g_bounce_lock);
			return;
		}
		ReleaseSRWLockExclusive(&g_bounce_lock);
	}
	_aligned_free(buf);
}

/* ------------------------------------------------------------ folio index (hash map) */

/*
 * Separate chaining (folio->kshim_hnext), at most one folio per bucket on
 * average. This was open addressing with linear probing and tombstones that
 * never counted towards the load: a long copy (pages created and reclaimed
 * in one mapping for gigabytes) filled every free slot with tombstones, so
 * lookups of uncached pages got slower and slower and finally looped forever
 * under the mapping lock (user report 2026-11-17: Explorer copies slowed to
 * ~0, then failed after ~3.7 GB through one file). Reclaim drops folios in
 * table order, which also left long tombstone runs. Chaining has neither.
 */
struct pmap {
	size_t cap;     /* buckets, power of two */
	size_t count;   /* folios */
	struct folio **slots;
};

static size_t phash(pgoff_t idx, size_t cap)
{
	u64 h = (u64)idx;   /* fmix64 */
	h ^= h >> 33;
	h *= 0xff51afd7ed558ccdULL;
	h ^= h >> 33;
	h *= 0xc4ceb9fe1a85ec53ULL;
	h ^= h >> 33;
	return (size_t)h & (cap - 1);
}

/* Every folio of a map; the body may detach (free) f. */
#define PMAP_FOR_EACH(pm, i, f, next) \
	for ((i) = 0; (i) < (pm)->cap; (i)++) \
		for ((f) = (pm)->slots[i]; (f) && (((next) = (f)->kshim_hnext), 1); (f) = (next))

static struct folio *pmap_get(struct pmap *pm, pgoff_t idx)
{
	struct folio *f;
	if (!pm || !pm->cap)
		return NULL;
	for (f = pm->slots[phash(idx, pm->cap)]; f; f = f->kshim_hnext)
		if (f->page.index == idx)
			return f;
	return NULL;
}

static void pmap_put_raw(struct pmap *pm, struct folio *f)
{
	size_t b = phash(f->page.index, pm->cap);
	f->kshim_hnext = pm->slots[b];
	pm->slots[b] = f;
}

static void pmap_resize(struct pmap *pm, size_t cap)
{
	size_t old = pm->cap, i;
	struct folio **slots = pm->slots, *f, *next;
	pm->cap = cap;
	pm->slots = calloc(cap, sizeof(*pm->slots));
	for (i = 0; i < old; i++)
		for (f = slots[i]; f; f = next) {
			next = f->kshim_hnext;
			pmap_put_raw(pm, f);
		}
	free(slots);
}

/* Only inserts resize: iterations (PMAP_FOR_EACH) remove but never insert. */
static void pmap_insert(struct pmap *pm, struct folio *f)
{
	if (!pm->cap)
		pmap_resize(pm, 64);
	else if (pm->count + 1 > pm->cap)
		pmap_resize(pm, pm->cap * 2);
	else if (pm->cap > 1024 && (pm->count + 1) * 8 < pm->cap)
		pmap_resize(pm, pm->cap / 4);   /* a big file's pages were reclaimed */
	pmap_put_raw(pm, f);
	pm->count++;
}

static void pmap_remove(struct pmap *pm, struct folio *f)
{
	struct folio **pp;
	if (!pm->cap)
		return;
	for (pp = &pm->slots[phash(f->page.index, pm->cap)]; *pp; pp = &(*pp)->kshim_hnext)
		if (*pp == f) {
			*pp = f->kshim_hnext;
			f->kshim_hnext = NULL;
			pm->count--;
			return;
		}
}

/* ------------------------------------------------------------ mappings */

static SRWLOCK g_mappings_lock = SRWLOCK_INIT;
static LIST_HEAD(g_mappings);
static volatile long g_nr_folios;
static volatile long g_reclaim_at = RECLAIM_THRESHOLD;

void kshim_mapping_init(struct address_space *m, struct inode *host)
{
	m->host = host;
	m->nrpages = 0;
	m->gfp_mask = GFP_KERNEL;
	m->kshim_pages = calloc(1, sizeof(struct pmap));
	mutex_init(&m->kshim_lock);
	INIT_LIST_HEAD(&m->i_private_list);
	AcquireSRWLockExclusive(&g_mappings_lock);
	list_add_tail(&m->i_private_list, &g_mappings);
	ReleaseSRWLockExclusive(&g_mappings_lock);
}

/*
 * Folio lifetime, as in Linux: the mapping holds one reference, and a ring of
 * buffer heads holds another (folio_attach_private takes it). Truncation and
 * eviction only remove a folio from its mapping; buffers that are still busy
 * (queued in a transaction, dirty, locked or referenced) keep it alive until
 * try_to_free_buffers() releases them, typically after the commit that writes
 * them. Freeing busy buffers with the folio left dangling pointers on the
 * driver's transaction list.
 */
static bool buffers_idle(struct folio *f);

/* Free the ring of buffer heads; no reference change. */
static void free_buffer_ring(struct folio *f)
{
	struct buffer_head *head = folio_buffers(f), *bh, *next;
	if (!head)
		return;
	bh = head;
	do {
		next = bh->b_this_page;
		free(bh);
		bh = next;
	} while (bh != head);
	f->page.private = NULL;
}

/*
 * Folio data pages: 4 KiB from 2 MiB VirtualAlloc blocks, freed pages kept on
 * a lock-free list for reuse. _aligned_malloc(4096, 4096) costs about 8 KiB
 * per page (twice the data). The pool keeps its peak size (bounded by the
 * page-cache reclaim) for the life of the process.
 */
static SLIST_HEADER g_free_pages;
static INIT_ONCE g_page_once = INIT_ONCE_STATIC_INIT;
static SRWLOCK g_page_lock = SRWLOCK_INIT;
static char *g_page_block;
static size_t g_page_block_left;

static BOOL CALLBACK page_pool_init(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
	(void)once; (void)param; (void)ctx;
	InitializeSListHead(&g_free_pages);
	return TRUE;
}

static void *page_alloc(void)
{
	PSLIST_ENTRY e;
	void *p = NULL;
	InitOnceExecuteOnce(&g_page_once, page_pool_init, NULL, NULL);
	e = InterlockedPopEntrySList(&g_free_pages);
	if (e)
		return e;
	AcquireSRWLockExclusive(&g_page_lock);
	if (!g_page_block_left) {
		g_page_block = VirtualAlloc(NULL, 2u << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		g_page_block_left = g_page_block ? (2u << 20) / PAGE_SIZE : 0;
	}
	if (g_page_block_left) {
		p = g_page_block;
		g_page_block += PAGE_SIZE;
		g_page_block_left--;
	}
	ReleaseSRWLockExclusive(&g_page_lock);
	if (!p)
		abort();   /* out of memory */
	return p;
}

static void page_free(void *p)
{
	if (p)
		InterlockedPushEntrySList(&g_free_pages, (PSLIST_ENTRY)p);
}

static void folio_free(struct folio *f)
{
	free_buffer_ring(f);   /* none left normally: buffers hold a reference */
	page_free(f->page.data);
	free(f);
	InterlockedDecrement(&g_nr_folios);
}

/* try_to_free_buffers(): release idle buffers and the reference they hold.
 * The caller holds its own reference (or the mapping's). */
static bool release_buffers(struct folio *f)
{
	if (!folio_buffers(f))
		return true;
	if (!buffers_idle(f))
		return false;
	free_buffer_ring(f);
	folio_put(f);
	return true;
}

/* References a folio may have when nothing but the cache is using it. */
static long idle_refs(struct folio *f)
{
	return folio_buffers(f) ? 2 : 1;
}

/* Remove a folio from its mapping and drop the mapping's reference; busy
 * buffers keep it alive (caller holds m->kshim_lock). */
static void folio_detach(struct address_space *m, struct folio *f)
{
	pmap_remove(m->kshim_pages, f);
	m->nrpages--;
	f->page.mapping = NULL;
	release_buffers(f);
	if (atomic_dec_and_test(&f->page.refcount))
		folio_free(f);
}

void kshim_mapping_release_all(struct address_space *m)
{
	struct pmap *pm = m->kshim_pages;
	struct folio *f, *next;
	size_t i;
	if (!pm)
		return;
	mutex_lock(&m->kshim_lock);
	PMAP_FOR_EACH(pm, i, f, next)
		folio_detach(m, f);
	mutex_unlock(&m->kshim_lock);
	AcquireSRWLockExclusive(&g_mappings_lock);
	list_del(&m->i_private_list);
	ReleaseSRWLockExclusive(&g_mappings_lock);
	free(pm->slots);
	free(pm);
	m->kshim_pages = NULL;
}

static bool buffers_idle(struct folio *f)
{
	struct buffer_head *head = folio_buffers(f), *bh;
	if (!head)
		return true;
	bh = head;
	do {
		if (atomic_read(&bh->b_count) || buffer_dirty(bh) || buffer_locked(bh) || test_bit(BH_PrivateStart, &bh->b_state))
			return false;
		bh = bh->b_this_page;
	} while (bh != head);
	return true;
}

/* Drop clean, unreferenced folios of one mapping. Returns how many. */
static long drop_clean(struct address_space *m, long want)
{
	struct pmap *pm = m->kshim_pages;
	struct folio *f, *next;
	long dropped = 0;
	size_t i;
	if (!pm)
		return 0;
	mutex_lock(&m->kshim_lock);
	PMAP_FOR_EACH(pm, i, f, next) {
		if (dropped >= want)
			break;   /* leaves this bucket; the outer loop checks again */
		if (atomic_read(&f->page.refcount) != idle_refs(f) || folio_test_locked(f) || !buffers_idle(f))
			continue;
		/*
		 * Dirty with no dirty or in-transaction buffers: the transaction
		 * commit wrote the buffers (and freed those of file data) but left
		 * the folio flag set. Linux's background writeback would find
		 * nothing to write and clean it; there is none here, so every
		 * written page stayed "dirty" and the cache grew without bound.
		 */
		if (folio_test_dirty(f))
			folio_clear_dirty_for_io(f);
		folio_detach(m, f);
		dropped++;
	}
	mutex_unlock(&m->kshim_lock);
	return dropped;
}

void kshim_mapping_drop_clean(struct address_space *m)
{
	drop_clean(m, LONG_MAX);
}

/* Debugging: what the page cache holds and why reclaim can't drop it. */
void kshim_pagecache_stats(char *buf, size_t size)
{
	struct list_head *pos;
	long mappings = 0, total = 0, dirty = 0, locked = 0, refs = 0, busy = 0, droppable = 0, with_bufs = 0;
	AcquireSRWLockShared(&g_mappings_lock);
	list_for_each(pos, &g_mappings) {
		struct address_space *m = container_of(pos, struct address_space, i_private_list);
		struct pmap *pm = m->kshim_pages;
		struct folio *f, *next;
		size_t i;
		mappings++;
		if (!pm)
			continue;
		mutex_lock(&m->kshim_lock);
		PMAP_FOR_EACH(pm, i, f, next) {
			total++;
			with_bufs += folio_buffers(f) != NULL;
			if (folio_test_dirty(f))
				dirty++;
			else if (folio_test_locked(f))
				locked++;
			else if (atomic_read(&f->page.refcount) != idle_refs(f))
				refs++;
			else if (!buffers_idle(f))
				busy++;
			else
				droppable++;
		}
		mutex_unlock(&m->kshim_lock);
	}
	ReleaseSRWLockShared(&g_mappings_lock);
	snprintf(buf, size, "folios %ld (counter %ld): dirty %ld, locked %ld, extra refs %ld, busy buffers %ld, droppable %ld; "
		"with buffers %ld; mappings %ld", total, (long)g_nr_folios, dirty, locked, refs, busy, droppable, with_bufs, mappings);
}

void kshim_reclaim(void)
{
	struct list_head *pos;
	long excess = g_nr_folios - RECLAIM_TARGET;
	if (excess <= 0)
		return;
	AcquireSRWLockShared(&g_mappings_lock);
	list_for_each(pos, &g_mappings) {
		struct address_space *m = container_of(pos, struct address_space, i_private_list);
		if (excess <= 0)
			break;
		excess -= drop_clean(m, excess);
	}
	ReleaseSRWLockShared(&g_mappings_lock);
}

/* ------------------------------------------------------------ folios */

void folio_lock(struct folio *f)
{
	while (test_and_set_bit(PG_locked, &f->page.flags))
		SwitchToThread();
}

bool folio_trylock(struct folio *f)
{
	return !test_and_set_bit(PG_locked, &f->page.flags);
}

void folio_unlock(struct folio *f)
{
	clear_bit(PG_locked, &f->page.flags);
}

void folio_get(struct folio *f)
{
	atomic_inc(&f->page.refcount);
}

void folio_put(struct folio *f)
{
	if (atomic_dec_and_test(&f->page.refcount))
		folio_free(f);
}

bool folio_mark_dirty(struct folio *f)
{
	return !test_and_set_bit(PG_dirty, &f->page.flags);
}

bool folio_clear_dirty_for_io(struct folio *f)
{
	return test_and_clear_bit(PG_dirty, &f->page.flags);
}

int folio_mkclean(struct folio *f)
{
	return 0;   /* nothing is mmapped */
}

void folio_zero_segment(struct folio *f, size_t start, size_t end)
{
	if (end > start)
		memset((char *)f->page.data + start, 0, end - start);
}

void folio_zero_range(struct folio *f, size_t start, size_t len)
{
	memset((char *)f->page.data + start, 0, len);
}

/* Find or create; returns a referenced folio (locked if FGP_LOCK). */
struct folio *__filemap_get_folio(struct address_space *m, pgoff_t index, unsigned int fgp, gfp_t gfp)
{
	struct folio *f;
	bool created = false;

	mutex_lock(&m->kshim_lock);
	f = pmap_get(m->kshim_pages, index);
	if (!f && (fgp & FGP_CREAT)) {
		f = calloc(1, sizeof(*f));
		f->page.data = page_alloc();
		memset(f->page.data, 0, PAGE_SIZE);
		f->page.index = index;
		f->page.mapping = m;
		atomic_set(&f->page.refcount, 1);   /* the mapping's reference */
		pmap_insert(m->kshim_pages, f);
		m->nrpages++;
		created = true;
	}
	if (f)
		folio_get(f);
	mutex_unlock(&m->kshim_lock);

	if (!f)
		return ERR_PTR(-ENOENT);
	/*
	 * Reclaim when over the threshold. A pass can free little while most
	 * folios are dirty (a big write before its commit); without the
	 * back-off every new folio rescanned the whole cache (60% of the CPU
	 * time of a large write).
	 */
	if (created && InterlockedIncrement(&g_nr_folios) > g_reclaim_at) {
		long next;
		kshim_reclaim();
		next = g_nr_folios + RECLAIM_BACKOFF;
		InterlockedExchange(&g_reclaim_at, next > RECLAIM_THRESHOLD ? next : RECLAIM_THRESHOLD);
	}
	if (fgp & FGP_LOCK)
		folio_lock(f);
	return f;
}

struct folio *filemap_get_folio(struct address_space *m, pgoff_t index)
{
	return __filemap_get_folio(m, index, 0, 0);
}

struct page *pagecache_get_page(struct address_space *m, pgoff_t index, int fgp, gfp_t gfp)
{
	struct folio *f = __filemap_get_folio(m, index, (unsigned)fgp, gfp);
	return IS_ERR(f) ? NULL : &f->page;
}

struct page *grab_cache_page_write_begin(struct address_space *m, pgoff_t index)
{
	return pagecache_get_page(m, index, FGP_WRITEBEGIN, 0);
}

struct folio *read_mapping_folio(struct address_space *m, pgoff_t index, struct file *file)
{
	struct folio *f = __filemap_get_folio(m, index, FGP_CREAT, 0);
	int err;
	if (IS_ERR(f))
		return f;
	if (folio_test_uptodate(f))
		return f;
	folio_lock(f);
	if (folio_test_uptodate(f)) {
		folio_unlock(f);
		return f;
	}
	err = m->a_ops->read_folio(file, f);   /* unlocks */
	if (err) {
		folio_put(f);
		return ERR_PTR(err);
	}
	if (!folio_test_uptodate(f)) {
		folio_put(f);
		return ERR_PTR(-EIO);
	}
	return f;
}

struct page *read_mapping_page(struct address_space *m, pgoff_t index, struct file *file)
{
	struct folio *f = read_mapping_folio(m, index, file);
	return IS_ERR(f) ? (struct page *)f : &f->page;
}

struct folio *readahead_folio(struct readahead_control *rac)
{
	struct folio *f;
	if (!rac->_nr_pages)
		return NULL;
	f = __filemap_get_folio(rac->mapping, rac->_index, FGP_CREAT | FGP_LOCK, 0);
	rac->_index++;
	rac->_nr_pages--;
	if (IS_ERR(f))
		return NULL;
	folio_put(f);   /* readahead_folio returns the folio without an extra ref */
	return f;
}

/* ------------------------------------------------------------ buffer heads */

static struct buffer_head *alloc_bh(void)
{
	struct buffer_head *bh = calloc(1, sizeof(*bh));
	INIT_LIST_HEAD(&bh->b_assoc_buffers);
	return bh;
}

struct buffer_head *folio_create_empty_buffers(struct folio *f, unsigned long blocksize, unsigned long b_state)
{
	struct buffer_head *head = NULL, *tail = NULL;
	unsigned long off;

	for (off = 0; off < PAGE_SIZE; off += blocksize) {
		struct buffer_head *bh = alloc_bh();
		bh->b_folio = f;
		bh->b_size = blocksize;
		bh->b_data = (char *)f->page.data + off;
		bh->b_state = b_state;
		if (folio_test_uptodate(f))
			set_buffer_uptodate(bh);
		if (!head)
			head = bh;
		else
			tail->b_this_page = bh;
		tail = bh;
	}
	tail->b_this_page = head;
	f->page.private = head;
	folio_get(f);   /* folio_attach_private(): the buffers pin the folio */
	return head;
}

struct buffer_head *create_empty_buffers(struct folio *f, unsigned long blocksize, unsigned long b_state)
{
	return folio_create_empty_buffers(f, blocksize, b_state);
}

bool try_to_free_buffers(struct folio *f)
{
	return release_buffers(f);
}

/* Block-device buffer for (block, size); the folio stays in bd_mapping. */
static struct buffer_head *bdev_buffer(struct block_device *bdev, sector_t block, unsigned size)
{
	struct address_space *m = bdev->bd_mapping;
	u64 byte = (u64)block * size;
	pgoff_t index = (pgoff_t)(byte >> PAGE_SHIFT);
	unsigned off = (unsigned)(byte & (PAGE_SIZE - 1));
	struct folio *f;
	struct buffer_head *head, *bh;

	if (size < 512 || size > PAGE_SIZE || (size & (size - 1)))
		return NULL;
	f = __filemap_get_folio(m, index, FGP_CREAT | FGP_LOCK, 0);
	if (IS_ERR(f))
		return NULL;

	head = folio_buffers(f);
	if (head && head->b_size != size) {
		/* re-slice the page with a new block size (only before first use) */
		if (!release_buffers(f)) {
			folio_unlock(f);
			folio_put(f);
			return NULL;
		}
		head = NULL;
	}
	if (!head) {
		u64 first = (u64)index << PAGE_SHIFT;
		head = folio_create_empty_buffers(f, size, 0);
		bh = head;
		do {
			bh->b_bdev = bdev;
			bh->b_blocknr = (sector_t)((first + (u64)(bh->b_data - (char *)f->page.data)) / size);
			set_buffer_mapped(bh);
			bh = bh->b_this_page;
		} while (bh != head);
	}
	bh = head;
	while (bh->b_data != (char *)f->page.data + off)
		bh = bh->b_this_page;
	get_bh(bh);
	folio_unlock(f);
	folio_put(f);   /* the mapping keeps the folio; the bh reference pins it */
	return bh;
}

struct buffer_head *__getblk_gfp(struct block_device *bdev, sector_t block, unsigned size, gfp_t gfp)
{
	return bdev_buffer(bdev, block, size);
}

struct buffer_head *bdev_getblk(struct block_device *bdev, sector_t block, unsigned size, gfp_t gfp)
{
	return bdev_buffer(bdev, block, size);
}

struct buffer_head *__getblk(struct block_device *bdev, sector_t block, unsigned size)
{
	return bdev_buffer(bdev, block, size);
}

struct buffer_head *__bread_gfp(struct block_device *bdev, sector_t block, unsigned size, gfp_t gfp)
{
	struct buffer_head *bh = bdev_buffer(bdev, block, size);
	if (!bh)
		return NULL;
	if (!buffer_uptodate(bh)) {
		lock_buffer(bh);
		if (!buffer_uptodate(bh)) {
			if (kshim_bdev_read(bdev, (u64)block * size, bh->b_data, size)) {
				unlock_buffer(bh);
				brelse(bh);
				return NULL;
			}
			set_buffer_uptodate(bh);
		}
		unlock_buffer(bh);
	}
	return bh;
}

struct buffer_head *__bread(struct block_device *bdev, sector_t block, unsigned size)
{
	return __bread_gfp(bdev, block, size, 0);
}

struct buffer_head *sb_bread(struct super_block *sb, sector_t block)
{
	return __bread_gfp(sb->s_bdev, block, (unsigned)sb->s_blocksize, 0);
}

struct buffer_head *sb_getblk(struct super_block *sb, sector_t block)
{
	return bdev_buffer(sb->s_bdev, block, (unsigned)sb->s_blocksize);
}

void __brelse(struct buffer_head *bh)
{
	if (atomic_read(&bh->b_count) <= 0) {
		kshim_warn(__FILE__, __LINE__, "brelse of a free buffer");
		return;
	}
	put_bh(bh);
}

void brelse(struct buffer_head *bh)
{
	if (bh)
		__brelse(bh);
}

int trylock_buffer(struct buffer_head *bh)
{
	return !test_and_set_bit(BH_Lock, &bh->b_state);
}

void lock_buffer(struct buffer_head *bh)
{
	while (test_and_set_bit(BH_Lock, &bh->b_state))
		SwitchToThread();
}

void unlock_buffer(struct buffer_head *bh)
{
	clear_bit(BH_Lock, &bh->b_state);
}

void mark_buffer_dirty(struct buffer_head *bh)
{
	if (!test_set_buffer_dirty(bh) && bh->b_folio)
		folio_mark_dirty(bh->b_folio);
}

void mark_buffer_dirty_inode(struct buffer_head *bh, struct inode *inode)
{
	mark_buffer_dirty(bh);
}

void end_buffer_read_sync(struct buffer_head *bh, int uptodate)
{
	if (uptodate)
		set_buffer_uptodate(bh);
	else
		clear_buffer_uptodate(bh);
	unlock_buffer(bh);
	put_bh(bh);
}

void end_buffer_write_sync(struct buffer_head *bh, int uptodate)
{
	if (uptodate) {
		set_buffer_uptodate(bh);
	} else {
		set_buffer_write_io_error(bh);
		clear_buffer_uptodate(bh);
	}
	unlock_buffer(bh);
	put_bh(bh);
}

/*
 * Block plugging for writes. The driver's commit submits every transaction
 * buffer and then waits for each (wait_on_buffer); Linux's block layer merges
 * the adjacent bios. Writes from submit_bh are queued per thread and written
 * sorted and merged (≤ 4 MiB requests) when the thread waits on a buffer,
 * submits a read, issues a flush barrier, or has queued PLUG_MAX buffers.
 * Queued buffers are locked (under I/O) until their end_io runs.
 */
#define PLUG_MAX	8192
#define PLUG_BYTES	KSHIM_IO_BATCH_BYTES

static __declspec(thread) struct buffer_head **t_plug;
static __declspec(thread) size_t t_plug_n, t_plug_cap;

static int plug_cmp(const void *a, const void *b)
{
	const struct buffer_head *x = *(struct buffer_head *const *)a, *y = *(struct buffer_head *const *)b;
	if (x->b_bdev != y->b_bdev)
		return x->b_bdev < y->b_bdev ? -1 : 1;
	return x->b_blocknr < y->b_blocknr ? -1 : x->b_blocknr > y->b_blocknr;
}

void kshim_unplug(void)
{
	struct buffer_head **v = t_plug;
	size_t n = t_plug_n, i;
	u8 *bounce;

	if (!n)
		return;
	t_plug_n = 0;   /* end_io callbacks may submit again */
	qsort(v, n, sizeof(*v), plug_cmp);
	bounce = kshim_bounce_alloc(PLUG_BYTES);
	for (i = 0; i < n;) {
		struct buffer_head *first = v[i];
		size_t j = i + 1, bytes = first->b_size, k;
		int err;

		while (bounce && j < n && v[j]->b_bdev == first->b_bdev && v[j]->b_size == first->b_size &&
		       v[j]->b_blocknr == v[j - 1]->b_blocknr + 1 && bytes + v[j]->b_size <= PLUG_BYTES) {
			bytes += v[j]->b_size;
			j++;
		}
		if (j - i == 1) {
			err = kshim_bdev_write(first->b_bdev, (u64)first->b_blocknr * first->b_size, first->b_data, first->b_size);
		} else {
			size_t pos = 0;
			for (k = i; k < j; k++) {
				memcpy(bounce + pos, v[k]->b_data, v[k]->b_size);
				pos += v[k]->b_size;
			}
			err = kshim_bdev_write(first->b_bdev, (u64)first->b_blocknr * first->b_size, bounce, bytes);
		}
		for (k = i; k < j; k++)
			if (v[k]->b_end_io)
				v[k]->b_end_io(v[k], err == 0);
		i = j;
	}
	kshim_bounce_free(bounce, PLUG_BYTES);
}

void wait_on_buffer(struct buffer_head *bh)
{
	kshim_unplug();
	while (buffer_locked(bh))
		SwitchToThread();   /* under I/O in another thread's plug */
}

/* Writes are queued (see above); reads are synchronous. */
void submit_bh(unsigned int opf, struct buffer_head *bh)
{
	int err;
	u64 off = (u64)bh->b_blocknr * bh->b_size;

	if ((opf & REQ_OP_MASK) == REQ_OP_WRITE) {
		if (t_plug_n == t_plug_cap) {
			size_t nc = t_plug_cap ? t_plug_cap * 2 : 256;
			struct buffer_head **nv = realloc(t_plug, nc * sizeof(*nv));
			if (!nv) {
				err = kshim_bdev_write(bh->b_bdev, off, bh->b_data, bh->b_size);
				if (bh->b_end_io)
					bh->b_end_io(bh, err == 0);
				return;
			}
			t_plug = nv;
			t_plug_cap = nc;
		}
		t_plug[t_plug_n++] = bh;
		if (t_plug_n >= PLUG_MAX)
			kshim_unplug();
		return;
	}
	kshim_unplug();   /* keep reads after earlier writes */
	err = kshim_bdev_read(bh->b_bdev, off, bh->b_data, bh->b_size);
	if (bh->b_end_io)
		bh->b_end_io(bh, err == 0);
}

int __sync_dirty_buffer(struct buffer_head *bh, unsigned int op_flags)
{
	int err = 0;
	lock_buffer(bh);
	if (test_clear_buffer_dirty(bh)) {
		kshim_unplug();
		err = kshim_bdev_write(bh->b_bdev, (u64)bh->b_blocknr * bh->b_size, bh->b_data, bh->b_size);
		if (err) {
			set_buffer_write_io_error(bh);
			clear_buffer_uptodate(bh);
			err = -EIO;
		} else {
			set_buffer_uptodate(bh);
		}
	}
	unlock_buffer(bh);
	return err;
}

int sync_dirty_buffer(struct buffer_head *bh)
{
	return __sync_dirty_buffer(bh, REQ_SYNC);
}

void write_dirty_buffer(struct buffer_head *bh, unsigned int op_flags)
{
	__sync_dirty_buffer(bh, op_flags);
}

int bh_read(struct buffer_head *bh, unsigned int op_flags)
{
	if (buffer_uptodate(bh))
		return 1;
	lock_buffer(bh);
	if (!buffer_uptodate(bh)) {
		if (kshim_bdev_read(bh->b_bdev, (u64)bh->b_blocknr * bh->b_size, bh->b_data, bh->b_size)) {
			unlock_buffer(bh);
			return -EIO;
		}
		set_buffer_uptodate(bh);
	}
	unlock_buffer(bh);
	return 0;
}

void bh_read_nowait(struct buffer_head *bh, unsigned int op_flags)
{
	bh_read(bh, op_flags);
}

int bh_uptodate_or_lock(struct buffer_head *bh)
{
	if (buffer_uptodate(bh))
		return 1;
	lock_buffer(bh);
	if (buffer_uptodate(bh)) {
		unlock_buffer(bh);
		return 1;
	}
	return 0;
}

void ll_rw_block(unsigned int op, int nr, struct buffer_head *bhs[])
{
	int i;
	for (i = 0; i < nr; i++) {
		if ((op & REQ_OP_MASK) == REQ_OP_WRITE)
			__sync_dirty_buffer(bhs[i], op);
		else
			bh_read(bhs[i], op);
	}
}

void map_bh(struct buffer_head *bh, struct super_block *sb, sector_t block)
{
	set_buffer_mapped(bh);
	bh->b_bdev = sb->s_bdev;
	bh->b_blocknr = block;
	bh->b_size = sb->s_blocksize;
}

/*
 * A block that is now mapped by a file must not keep a stale copy in the
 * block device cache. Drop idle bdev buffers for the range.
 */
void clean_bdev_aliases(struct block_device *bdev, sector_t block, sector_t len)
{
	struct address_space *m = bdev->bd_mapping;
	sector_t b;
	unsigned size = bdev->bd_block_size;

	for (b = block; b < block + len; b++) {
		pgoff_t index = (pgoff_t)(((u64)b * size) >> PAGE_SHIFT);
		struct folio *f;
		mutex_lock(&m->kshim_lock);
		f = pmap_get(m->kshim_pages, index);
		if (f && atomic_read(&f->page.refcount) == idle_refs(f) && !folio_test_locked(f) && buffers_idle(f))
			folio_detach(m, f);
		mutex_unlock(&m->kshim_lock);
	}
}

/* ------------------------------------------------------------ writeback */

static int write_folio_buffers(struct folio *f)
{
	struct buffer_head *head = folio_buffers(f), *bh;
	int err = 0;
	if (!head)
		return 0;
	bh = head;
	do {
		if (buffer_mapped(bh) && buffer_dirty(bh)) {
			int e = __sync_dirty_buffer(bh, REQ_SYNC);
			if (e && !err)
				err = e;
		}
		bh = bh->b_this_page;
	} while (bh != head);
	return err;
}

/* ------------------------------------------------------------ batched writeback */

struct wb_ent {
	struct buffer_head *bh;
	struct folio *f;
};

static int wb_cmp(const void *a, const void *b)
{
	const struct buffer_head *x = ((const struct wb_ent *)a)->bh, *y = ((const struct wb_ent *)b)->bh;
	if (x->b_bdev != y->b_bdev)
		return x->b_bdev < y->b_bdev ? -1 : 1;
	return x->b_blocknr < y->b_blocknr ? -1 : x->b_blocknr > y->b_blocknr;
}

#define WB_MAX_BYTES	KSHIM_IO_BATCH_BYTES

/* Ends the I/O of ents[0..n) (buffers locked, dirty bit already cleared). */
static void wb_finish(struct wb_ent *ents, size_t n, int err)
{
	size_t i;
	for (i = 0; i < n; i++) {
		struct buffer_head *bh = ents[i].bh;
		if (err) {
			set_buffer_write_io_error(bh);
			mark_buffer_dirty(bh);   /* still to be written */
			folio_mark_dirty(ents[i].f);
		} else {
			set_buffer_uptodate(bh);
		}
		unlock_buffer(bh);
		put_bh(bh);
		folio_put(ents[i].f);
	}
}

/*
 * Writes every dirty buffer of a mapping, sorted by block and merged into
 * requests of up to 1 MiB (Linux builds bios the same way). Writing buffer by
 * buffer in hash order made each 4 KiB a separate device write. A buffer is
 * locked from collection to completion, like a buffer under I/O in Linux.
 */
int kshim_mapping_writeback(struct address_space *m)
{
	struct pmap *pm = m->kshim_pages;
	struct folio **list;
	struct wb_ent *ents = NULL;
	size_t i, n = 0, ne = 0, cap = 0;
	u8 *bounce = NULL;
	int err = 0;

	if (!pm)
		return 0;
	/* snapshot the dirty folios, then work without holding the map lock */
	mutex_lock(&m->kshim_lock);
	list = malloc((pm->count + 1) * sizeof(*list));
	{
		struct folio *f, *next;
		PMAP_FOR_EACH(pm, i, f, next)
			if (folio_test_dirty(f)) {
				folio_get(f);
				list[n++] = f;
			}
	}
	mutex_unlock(&m->kshim_lock);

	/* collect the dirty buffers; a buffer someone else holds is written alone below */
	for (i = 0; i < n; i++) {
		struct folio *f = list[i];
		struct buffer_head *head, *bh;
		bool skipped = false;

		folio_lock(f);
		folio_clear_dirty_for_io(f);
		head = folio_buffers(f);
		bh = head;
		while (bh) {
			if (buffer_mapped(bh) && buffer_dirty(bh)) {
				if (!trylock_buffer(bh)) {
					skipped = true;
				} else if (!test_clear_buffer_dirty(bh)) {
					unlock_buffer(bh);
				} else {
					if (ne == cap) {
						size_t nc = cap ? cap * 2 : 256;
						struct wb_ent *ne_ents = realloc(ents, nc * sizeof(*ents));
						if (!ne_ents)
							abort();
						ents = ne_ents;
						cap = nc;
					}
					get_bh(bh);
					folio_get(f);
					ents[ne].bh = bh;
					ents[ne].f = f;
					ne++;
				}
			}
			bh = bh->b_this_page;
			if (bh == head)
				break;
		}
		if (skipped) {
			int e = write_folio_buffers(f);   /* waits for those buffers */
			if (e && !err)
				err = e;
		}
		folio_unlock(f);
		folio_put(f);
	}
	free(list);

	qsort(ents, ne, sizeof(*ents), wb_cmp);
	if (ne)
		bounce = kshim_bounce_alloc(WB_MAX_BYTES);
	for (i = 0; i < ne;) {
		struct buffer_head *first = ents[i].bh;
		size_t j = i + 1, bytes = first->b_size;
		int e;

		while (bounce && j < ne && ents[j].bh->b_bdev == first->b_bdev &&
		       ents[j].bh->b_size == first->b_size &&
		       ents[j].bh->b_blocknr == ents[j - 1].bh->b_blocknr + 1 &&
		       bytes + ents[j].bh->b_size <= WB_MAX_BYTES) {
			bytes += ents[j].bh->b_size;
			j++;
		}
		if (j - i == 1 || !bounce) {
			e = kshim_bdev_write(first->b_bdev, (u64)first->b_blocknr * first->b_size, first->b_data, first->b_size);
			j = i + 1;
		} else {
			size_t k, pos = 0;
			for (k = i; k < j; k++) {
				memcpy(bounce + pos, ents[k].bh->b_data, ents[k].bh->b_size);
				pos += ents[k].bh->b_size;
			}
			e = kshim_bdev_write(first->b_bdev, (u64)first->b_blocknr * first->b_size, bounce, bytes);
		}
		if (e && !err)
			err = -EIO;
		wb_finish(ents + i, j - i, e);
		i = j;
	}
	kshim_bounce_free(bounce, WB_MAX_BYTES);
	free(ents);
	return err;
}

int filemap_fdatawrite(struct address_space *m)
{
	return kshim_mapping_writeback(m);
}

int filemap_fdatawait(struct address_space *m)
{
	return 0;
}

/* For a block device this is the commit barrier: flush to stable storage. */
int filemap_write_and_wait(struct address_space *m)
{
	int err = kshim_mapping_writeback(m);
	if (!err && (m->flags & KSHIM_MAPPING_BDEV)) {
		struct block_device *bdev = NULL;
		/* the mapping is embedded in the device record; find the bdev via a buffer-free path */
		extern struct block_device *kshim_bdev_by_mapping(struct address_space *m);
		bdev = kshim_bdev_by_mapping(m);
		if (bdev)
			err = kshim_bdev_flush(bdev);
	}
	return err;
}

int filemap_write_and_wait_range(struct address_space *m, loff_t start, loff_t end)
{
	return filemap_write_and_wait(m);
}

bool block_dirty_folio(struct address_space *m, struct folio *f)
{
	struct buffer_head *head = folio_buffers(f), *bh;
	if (head) {
		bh = head;
		do {
			set_buffer_dirty(bh);
			bh = bh->b_this_page;
		} while (bh != head);
	}
	return folio_mark_dirty(f);
}

bool filemap_dirty_folio(struct address_space *m, struct folio *f)
{
	return folio_mark_dirty(f);
}

bool __set_page_dirty_buffers(struct page *p)
{
	return block_dirty_folio(p->mapping, page_folio(p));
}

void block_invalidate_folio(struct folio *f, size_t offset, size_t length)
{
	if (offset == 0 && length >= PAGE_SIZE)
		try_to_free_buffers(f);
}

/* ------------------------------------------------------------ truncation */

void truncate_inode_pages(struct address_space *m, loff_t start)
{
	struct pmap *pm = m->kshim_pages;
	pgoff_t first = (pgoff_t)((start + PAGE_SIZE - 1) >> PAGE_SHIFT);
	struct folio *f, *next;
	size_t i;
	if (!pm)
		return;
	mutex_lock(&m->kshim_lock);
	PMAP_FOR_EACH(pm, i, f, next) {
		if (f->page.index >= first) {
			/*
			 * The driver's ->invalidate_folio is a no-op: buffers already
			 * in a transaction are written by its commit, so they must
			 * survive here (folio_detach keeps busy buffers alive).
			 */
			folio_clear_dirty(f);
			folio_detach(m, f);
		}
	}
	mutex_unlock(&m->kshim_lock);
	/* zero the tail of a partial last page */
	if (start & (PAGE_SIZE - 1)) {
		struct folio *f = filemap_get_folio(m, (pgoff_t)(start >> PAGE_SHIFT));
		if (!IS_ERR(f)) {
			folio_zero_segment(f, (size_t)(start & (PAGE_SIZE - 1)), PAGE_SIZE);
			folio_put(f);
		}
	}
}

void truncate_inode_pages_final(struct address_space *m)
{
	truncate_inode_pages(m, 0);
}

void truncate_pagecache(struct inode *inode, loff_t newsize)
{
	truncate_inode_pages(inode->i_mapping, newsize);
}

void truncate_setsize(struct inode *inode, loff_t newsize)
{
	i_size_write(inode, newsize);
	truncate_pagecache(inode, newsize);
}

void truncate_pagecache_range(struct inode *inode, loff_t start, loff_t end)
{
	struct address_space *m = inode->i_mapping;
	pgoff_t idx;
	for (idx = (pgoff_t)(start >> PAGE_SHIFT); (loff_t)idx << PAGE_SHIFT <= end; idx++) {
		struct folio *f = filemap_get_folio(m, idx);
		loff_t fs = (loff_t)idx << PAGE_SHIFT;
		size_t from, to;
		if (IS_ERR(f))
			continue;
		from = start > fs ? (size_t)(start - fs) : 0;
		to = end + 1 < fs + (loff_t)PAGE_SIZE ? (size_t)(end + 1 - fs) : PAGE_SIZE;
		folio_zero_segment(f, from, to);
		folio_put(f);
	}
}

void invalidate_mapping_pages(struct address_space *m, pgoff_t start, pgoff_t end)
{
	drop_clean(m, LONG_MAX);
}

void pagecache_isize_extended(struct inode *inode, loff_t from, loff_t to)
{
}

/* ------------------------------------------------------------ block-based file I/O */

/* Read every block of a locked folio through get_block; holes read as zeros. */
static int read_folio_blocks(struct folio *f, get_block_t get_block)
{
	struct inode *inode = f->page.mapping->host;
	unsigned blocksize = 1u << inode->i_blkbits;
	sector_t iblock = (sector_t)((u64)f->page.index << (PAGE_SHIFT - inode->i_blkbits));
	loff_t isize = i_size_read(inode);
	unsigned off;

	for (off = 0; off < PAGE_SIZE; off += blocksize, iblock++) {
		struct buffer_head tmp;
		int err;

		if (((loff_t)f->page.index << PAGE_SHIFT) + off >= isize) {
			memset((char *)f->page.data + off, 0, blocksize);
			continue;
		}
		memset(&tmp, 0, sizeof(tmp));
		tmp.b_size = blocksize;
		err = get_block(inode, iblock, &tmp, 0);
		if (err)
			return err;
		if (!buffer_mapped(&tmp)) {
			memset((char *)f->page.data + off, 0, blocksize);
			continue;
		}
		err = kshim_bdev_read(tmp.b_bdev ? tmp.b_bdev : inode->i_sb->s_bdev, (u64)tmp.b_blocknr * blocksize,
				      (char *)f->page.data + off, blocksize);
		if (err)
			return err;
	}
	return 0;
}

int mpage_read_folio(struct folio *f, get_block_t get_block)
{
	int err = read_folio_blocks(f, get_block);
	if (!err)
		folio_mark_uptodate(f);
	folio_unlock(f);
	return err;
}

/*
 * Fills a run of consecutive locked folios with as few device reads as the
 * extents allow: get_block maps up to the end of an extent (b_size in, mapped
 * length out), and each mapped piece is one read into a bounce buffer. This is
 * what Linux's mpage code does with bios; reading page by page made every
 * 4 KiB a separate (uncached, on raw disks) device round trip. On error the
 * folios stay !uptodate and read_folio retries them one by one.
 */
volatile LONG64 g_ra_pieces;	/* extent pieces seen by readahead (KSHIM_IOSTATS) */

static void read_folio_run(struct folio **fs, unsigned n, get_block_t get_block)
{
	struct inode *inode;
	unsigned bits;
	loff_t isize;
	u64 start;
	size_t total, pos = 0;
	u8 *buf;
	int err = 0;
	unsigned i;

	if (!n)
		return;
	inode = fs[0]->page.mapping->host;
	bits = inode->i_blkbits;
	isize = i_size_read(inode);
	start = (u64)fs[0]->page.index << PAGE_SHIFT;
	total = (size_t)n << PAGE_SHIFT;
	/* page-aligned: raw disk handles want sector-aligned buffers */
	buf = kshim_bounce_alloc(total);
	if (!buf)
		err = -ENOMEM;
	{
		/* pieces that are physically adjacent (separate extent records,
		 * contiguous blocks) are merged into one device read */
		struct block_device *pend_bdev = NULL;
		u64 pend_disk = 0;
		size_t pend_pos = 0, pend_len = 0;

		while (!err && pos < total) {
			u64 fpos = start + pos;
			struct buffer_head tmp;
			size_t len;

			if ((loff_t)fpos >= isize) {
				memset(buf + pos, 0, total - pos);
				break;
			}
			memset(&tmp, 0, sizeof(tmp));
			tmp.b_size = total - pos;
			err = get_block(inode, (sector_t)(fpos >> bits), &tmp, 0);
			if (err)
				break;
			InterlockedIncrement64(&g_ra_pieces);
			len = tmp.b_size;
			if (len < ((size_t)1 << bits) || len > total - pos)
				len = min(total - pos, (size_t)1 << bits);
			if (buffer_mapped(&tmp)) {
				struct block_device *bd = tmp.b_bdev ? tmp.b_bdev : inode->i_sb->s_bdev;
				u64 disk = (u64)tmp.b_blocknr << bits;
				if (pend_len && (bd != pend_bdev || disk != pend_disk + pend_len)) {
					err = kshim_bdev_read(pend_bdev, pend_disk, buf + pend_pos, pend_len);
					pend_len = 0;
				}
				if (!pend_len) {
					pend_bdev = bd;
					pend_disk = disk;
					pend_pos = pos;
				}
				pend_len += len;
			} else {
				memset(buf + pos, 0, len);   /* hole */
			}
			pos += len;
		}
		if (!err && pend_len)
			err = kshim_bdev_read(pend_bdev, pend_disk, buf + pend_pos, pend_len);
	}
	for (i = 0; i < n; i++) {
		if (!err) {
			memcpy(fs[i]->page.data, buf + ((size_t)i << PAGE_SHIFT), PAGE_SIZE);
			folio_mark_uptodate(fs[i]);
		}
		folio_unlock(fs[i]);
		folio_put(fs[i]);
	}
	kshim_bounce_free(buf, total);
}

#define KSHIM_RA_BATCH	1024	/* pages per device read at most (4 MiB) */

void mpage_readahead(struct readahead_control *rac, get_block_t get_block)
{
	struct folio *batch[KSHIM_RA_BATCH], *f;
	unsigned n = 0;

	while ((f = readahead_folio(rac)) != NULL) {
		if (folio_test_uptodate(f)) {
			folio_unlock(f);
			read_folio_run(batch, n, get_block);
			n = 0;
			continue;
		}
		if (n && (batch[n - 1]->page.index + 1 != f->page.index || n == KSHIM_RA_BATCH)) {
			read_folio_run(batch, n, get_block);
			n = 0;
		}
		folio_get(f);   /* held until filled: readahead_folio returns it without a reference */
		batch[n++] = f;
	}
	read_folio_run(batch, n, get_block);
}

/* Reads pages [index, index + nr) of a file through its readahead op, if any. */
void kshim_file_readahead(struct address_space *m, struct file *file, pgoff_t index, unsigned long nr)
{
	if (!m->a_ops || !m->a_ops->readahead)
		return;
	while (nr) {
		struct readahead_control rac;
		unsigned chunk = nr > KSHIM_RA_BATCH ? KSHIM_RA_BATCH : (unsigned)nr;
		memset(&rac, 0, sizeof(rac));
		rac.file = file;
		rac.mapping = m;
		rac._index = index;
		rac._nr_pages = chunk;
		m->a_ops->readahead(&rac);
		index += chunk;
		nr -= chunk;
	}
}

int block_read_full_folio(struct folio *f, get_block_t get_block)
{
	return mpage_read_folio(f, get_block);
}

int __block_write_begin(struct folio *f, loff_t pos, unsigned len, get_block_t *get_block)
{
	struct inode *inode = f->page.mapping->host;
	unsigned blocksize = 1u << inode->i_blkbits;
	unsigned from = (unsigned)(pos & (PAGE_SIZE - 1)), to = from + len;
	sector_t iblock = (sector_t)((u64)f->page.index << (PAGE_SHIFT - inode->i_blkbits));
	struct buffer_head *head, *bh;
	unsigned bstart;
	int err = 0;

	head = folio_buffers(f);
	if (!head)
		head = folio_create_empty_buffers(f, blocksize, 0);

	for (bh = head, bstart = 0; bstart < PAGE_SIZE; bh = bh->b_this_page, bstart += blocksize, iblock++) {
		unsigned bend = bstart + blocksize;
		if (bend <= from || bstart >= to) {
			if (folio_test_uptodate(f))
				set_buffer_uptodate(bh);
			continue;
		}
		clear_buffer_new(bh);
		if (!buffer_mapped(bh)) {
			err = get_block(inode, iblock, bh, 1);
			if (err)
				break;
			if (buffer_new(bh)) {
				clean_bdev_bh_alias(bh);
				if (folio_test_uptodate(f)) {
					clear_buffer_new(bh);
					set_buffer_uptodate(bh);
					mark_buffer_dirty(bh);   /* as fs/buffer.c does */
					continue;
				}
				/* fresh block: zero what this write doesn't cover */
				if (bend > to)
					memset(bh->b_data + (to - bstart), 0, bend - to);
				if (bstart < from)
					memset(bh->b_data, 0, from - bstart);
				continue;
			}
		}
		if (folio_test_uptodate(f)) {
			set_buffer_uptodate(bh);
			continue;
		}
		if (!buffer_uptodate(bh) && (bstart < from || bend > to)) {
			/* partial write over existing data: read it first */
			err = kshim_bdev_read(bh->b_bdev ? bh->b_bdev : inode->i_sb->s_bdev, (u64)bh->b_blocknr * blocksize, bh->b_data, blocksize);
			if (err)
				break;
			set_buffer_uptodate(bh);
		}
	}
	return err;
}

int block_write_begin(struct address_space *m, loff_t pos, unsigned len, struct folio **foliop, get_block_t *get_block)
{
	struct folio *f = __filemap_get_folio(m, (pgoff_t)(pos >> PAGE_SHIFT), FGP_WRITEBEGIN, 0);
	int err;
	if (IS_ERR(f))
		return (int)PTR_ERR(f);
	err = __block_write_begin(f, pos, len, get_block);
	if (err) {
		folio_unlock(f);
		folio_put(f);
		return err;
	}
	*foliop = f;
	return 0;
}

int block_write_end(struct file *file, struct address_space *m, loff_t pos, unsigned len, unsigned copied, struct folio *f, void *fsdata)
{
	struct inode *inode = m->host;
	unsigned blocksize = 1u << inode->i_blkbits;
	unsigned from = (unsigned)(pos & (PAGE_SIZE - 1)), to = from + copied;
	struct buffer_head *head = folio_buffers(f), *bh;
	unsigned bstart;
	bool all_uptodate = true;

	if (copied < len && !folio_test_uptodate(f))
		copied = 0;   /* short copy into a non-uptodate folio: redo it */
	for (bh = head, bstart = 0; bstart < PAGE_SIZE; bh = bh->b_this_page, bstart += blocksize) {
		if (bstart + blocksize > from && bstart < to) {
			set_buffer_uptodate(bh);
			mark_buffer_dirty(bh);
		}
		if (!buffer_uptodate(bh))
			all_uptodate = false;
		clear_buffer_new(bh);
	}
	if (all_uptodate)
		folio_mark_uptodate(f);
	return (int)copied;
}

int generic_write_end(struct file *file, struct address_space *m, loff_t pos, unsigned len, unsigned copied, struct folio *f, void *fsdata)
{
	struct inode *inode = m->host;
	int ret = block_write_end(file, m, pos, len, copied, f, fsdata);
	bool grew = false;

	if (ret > 0 && pos + ret > inode->i_size) {
		i_size_write(inode, pos + ret);
		grew = true;
	}
	folio_unlock(f);
	folio_put(f);
	if (grew)
		mark_inode_dirty(inode);
	return ret;
}

int block_truncate_page(struct address_space *m, loff_t from, get_block_t *get_block)
{
	return 0;
}

int block_page_mkwrite(struct vm_area_struct *vma, struct vm_fault *vmf, get_block_t get_block)
{
	return -EOPNOTSUPP;
}

sector_t generic_block_bmap(struct address_space *m, sector_t block, get_block_t *get_block)
{
	struct buffer_head tmp;
	memset(&tmp, 0, sizeof(tmp));
	tmp.b_size = (size_t)1 << m->host->i_blkbits;
	get_block(m->host, block, &tmp, 0);
	return tmp.b_blocknr;
}

int block_write_full_folio(struct folio *f, struct writeback_control *wbc, void *get_block)
{
	return write_folio_buffers(f);
}
