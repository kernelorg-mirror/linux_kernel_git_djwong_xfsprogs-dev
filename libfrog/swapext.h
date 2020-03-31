/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2020 Oracle, Inc.
 * All Rights Reserved.
 */
#ifndef __LIBFROG_SWAPEXT_H__
#define __LIBFROG_SWAPEXT_H__

int xfrog_swapext_prep(struct xfs_fd *file2, uint64_t flags,
		int64_t file2_offset, int file1_fd, int64_t file1_offset,
		int64_t length, struct file_swap_range *req);
int xfrog_swapext(struct xfs_fd *xfd, struct file_swap_range *req);

#endif	/* __LIBFROG_SWAPEXT_H__ */
