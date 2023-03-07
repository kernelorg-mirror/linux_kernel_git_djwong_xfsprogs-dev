/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2021-2023 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __LIBXFS_XFBTREE_H__
#define __LIBXFS_XFBTREE_H__

#ifdef CONFIG_XFS_BTREE_IN_XFILE

/* Root block for an in-memory btree. */
struct xfs_btree_mem_head {
	__be32				mh_magic;
	__be32				mh_nlevels;
	__be64				mh_owner;
	__be64				mh_root;
	uuid_t				mh_uuid;
};

#define XFS_BTREE_MEM_HEAD_MAGIC	0x4341544D	/* "CATM" */

/* in-memory btree header is always block 0 in the backing store */
#define XFS_BTREE_MEM_HEAD_DADDR	0

/* xfile-backed in-memory btrees */

struct xfbtree {
	struct xfs_buftarg		*target;

	/* Owner of this btree. */
	unsigned long long		owner;
};

#endif /* CONFIG_XFS_BTREE_IN_XFILE */

#endif /* __LIBXFS_XFBTREE_H__ */
