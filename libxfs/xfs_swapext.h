// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2020 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef __XFS_SWAPEXT_H_
#define __XFS_SWAPEXT_H_ 1

/*
 * In-core information about an extent swap request between ranges of two
 * inodes.
 */
struct xfs_swapext_intent {
	struct list_head	si_list;
	union {
		struct xfs_inode *si_inode1;
		xfs_ino_t	si_inode1_ino;
	};
	union {
		struct xfs_inode *si_inode2;
		xfs_ino_t	si_inode2_ino;
	};
	xfs_fileoff_t		si_startoff1;
	xfs_fileoff_t		si_startoff2;
	xfs_filblks_t		si_blockcount;
	int			si_whichfork;
};

/* Information to help us reset reflink flag / CoW fork state after a swap. */
struct xfs_swapext_reflink_state {
	/* Is this a data fork swap? */
	unsigned int	datafork:1;

	/* Should we swap the inode reflink flags afterwards? */
	unsigned int	swap_flags:1;

	/* Was the reflink flag set on either inode beforehand? */
	unsigned int	ip1_reflink:1;
	unsigned int	ip2_reflink:1;
};

void xfs_swapext_reflink_prep(struct xfs_inode *ip1, struct xfs_inode *ip2,
		int whichfork, xfs_fileoff_t startoff1,
		xfs_fileoff_t startoff2, xfs_filblks_t blockcount,
		struct xfs_swapext_reflink_state *rs);
void xfs_swapext_reflink_finish(struct xfs_trans *tp, struct xfs_inode *ip1,
		struct xfs_inode *ip2, struct xfs_swapext_reflink_state *rs);

void xfs_swapext_schedule(struct xfs_trans *tpp, struct xfs_inode *ip1,
		struct xfs_inode *ip2, int whichfork, xfs_fileoff_t startoff1,
		xfs_fileoff_t startoff2, xfs_filblks_t blockcount);
int xfs_swapext_finish_one(struct xfs_trans *tp, struct xfs_inode *ip1,
		struct xfs_inode *ip2, int whichfork, xfs_fileoff_t *startoff1,
		xfs_fileoff_t *startoff2, xfs_filblks_t *blockcount);
int xfs_swapext_atomic(struct xfs_trans **tpp, struct xfs_inode *ip1,
		struct xfs_inode *ip2, int whichfork, xfs_fileoff_t startoff1,
		xfs_fileoff_t startoff2, xfs_filblks_t blockcount);

int xfs_swapext_deferred_bmap(struct xfs_trans **tpp, struct xfs_inode *ip1,
		struct xfs_inode *ip2, int whichfork, xfs_fileoff_t startoff1,
		xfs_fileoff_t startoff2, xfs_filblks_t blockcount);

#endif /* __XFS_SWAPEXT_H_ */
