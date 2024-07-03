// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2018-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef XFS_SCRUB_INODES_H_
#define XFS_SCRUB_INODES_H_

/*
 * Visit each space mapping of an inode fork.  Return 0 to continue iteration
 * or a positive error code to interrupt iteraton.  If ESTALE is returned,
 * iteration will be restarted from the beginning of the inode allocation
 * group.  Any other non zero value will stop iteration.  The special return
 * value ECANCELED can be used to stop iteration, because the inode iteration
 * function never generates that error code on its own.
 */
typedef int (*scrub_inode_iter_fn)(struct scrub_ctx *ctx,
		struct xfs_handle *handle, struct xfs_bulkstat *bs, void *arg);

/* Return metadata directories too. */
#define SCRUB_SCAN_METADIR	(1 << 0)

int scrub_scan_all_inodes(struct scrub_ctx *ctx, scrub_inode_iter_fn fn,
		unsigned int flags, void *arg);

int scrub_open_handle(struct xfs_handle *handle);

/*
 * Might this be a file that's missing its fsverity metadata?  When this is the
 * case, an open() call will return ENODATA.
 */
static inline bool fsverity_meta_is_missing(int error)
{
	switch (error) {
	case ENODATA:
	case EMSGSIZE:
	case EINVAL:
	case EFSCORRUPTED:
	case EFBIG:
		/*
		 * The nonzero errno codes above are the error codes that can
		 * be returned from fsverity on metadata validation errors.
		 */
		return true;
	}

	return false;
}

#endif /* XFS_SCRUB_INODES_H_ */
