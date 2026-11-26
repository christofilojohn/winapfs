/*
 * winapfs - read-only APFS for Windows
 * Copyright (C) 2026 winapfs contributors
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Shared between the apfsrw API files (apfsrw_api.c, apfsrw_write.c).
 */
#ifndef APFSRW_INTERNAL_H
#define APFSRW_INTERNAL_H

#include <kshim.h>
#include "kshim_internal.h"
#include <apfsrw.h>
#include "apfs.h"   /* driver internals: APFS_I(), apfs_iget() */

struct apfsrw_file {
	struct file f;
	struct mutex ra_lock;	/* protects readahead hints (ra_next / ra_end) */
	u64 ra_next;	/* where a sequential reader continues */
	u64 ra_end;	/* bytes before this were read ahead already */
};

/* A node is a referenced dentry. */
static inline struct dentry *D(apfsrw_node *n) { return (struct dentry *)n; }
static inline apfsrw_node *N(struct dentry *d) { return (apfsrw_node *)d; }

/* kshim */
struct dentry *kshim_d_alloc(struct dentry *parent, const char *name, size_t len);

#endif
