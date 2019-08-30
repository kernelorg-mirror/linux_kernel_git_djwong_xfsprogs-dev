// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef XFS_SCRUB_INODES_H_
#define XFS_SCRUB_INODES_H_

/*
 * Return codes for the inode iterator function are 0 to continue iterating,
 * and non-zero to stop iterating.  Any non-zero value will be passed up to the
 * iteration caller.  The special value ECANCELED can be used to stop
 * iteration, because the inode iteration function never generates that error
 * code on its own.
 */
typedef int (*scrub_inode_iter_fn)(struct scrub_ctx *ctx,
		struct xfs_handle *handle, struct xfs_bulkstat *bs, void *arg);

int scrub_scan_all_inodes(struct scrub_ctx *ctx, scrub_inode_iter_fn fn,
		void *arg);

int scrub_open_handle(struct xfs_handle *handle);

#endif /* XFS_SCRUB_INODES_H_ */
