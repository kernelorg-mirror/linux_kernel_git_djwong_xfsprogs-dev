/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2020-2024 Oracle.  All rights reserved.
 * All Rights Reserved.
 */
#ifndef __LIBFROG_FILE_EXCHANGE_H__
#define __LIBFROG_FILE_EXCHANGE_H__

int xfrog_commitrange_prep(struct xfs_commit_range *xcr, struct xfs_fd *file2,
		int64_t file2_offset, int file1_fd, int64_t file1_offset,
		int64_t length, const struct xfs_bulkstat *file2_stat);
int xfrog_commitrange(int file2_fd, struct xfs_commit_range *xcr,
		uint64_t flags);

void xfrog_defragrange_prep(struct xfs_commit_range *xdf,
		int file1_fd, const struct xfs_bulkstat *file2_stat);
int xfrog_defragrange(int file2_fd, struct xfs_commit_range *xdf);

void xfrog_exchrange_prep(struct xfs_exch_range *fxr, int64_t file2_offset,
		int file1_fd, int64_t file1_offset, int64_t length);
int xfrog_exchrange(int file2_fd, struct xfs_exch_range *fxr, uint64_t flags);

#endif	/* __LIBFROG_FILE_EXCHANGE_H__ */
