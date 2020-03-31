// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2020 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "libxfs_priv.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_defer.h"
#include "xfs_inode.h"
#include "xfs_trans.h"
#include "xfs_bmap.h"
#include "xfs_swapext.h"
#include "xfs_trace.h"

/* Prepare both inodes' reflink state for an extent swap. */
void
xfs_swapext_reflink_prep(
	struct xfs_inode	*ip1,
	struct xfs_inode	*ip2,
	int			whichfork,
	xfs_fileoff_t		startoff1,
	xfs_fileoff_t		startoff2,
	xfs_filblks_t		blockcount,
	struct xfs_swapext_reflink_state *rs)
{
	struct xfs_mount	*mp = ip1->i_mount;

	rs->datafork = whichfork == XFS_DATA_FORK;
	if (!rs->datafork)
		return;

	/*
	 * If either file has shared blocks and we're swapping data forks, we
	 * must flag the other file as having shared blocks so that we get the
	 * shared-block rmap functions if we need to fix up the rmaps.  The
	 * flags will be switched for real by xfs_swapext_reflink_finish.
	 */
	rs->swap_flags = 0;
	rs->ip1_reflink = xfs_is_reflink_inode(ip1);
	rs->ip2_reflink = xfs_is_reflink_inode(ip2);

	if (rs->ip1_reflink)
		ip2->i_d.di_flags2 |= XFS_DIFLAG2_REFLINK;
	if (rs->ip2_reflink)
		ip1->i_d.di_flags2 |= XFS_DIFLAG2_REFLINK;

	/*
	 * If either file had the reflink flag set before; and the two files'
	 * reflink state was different; and we're swapping the entirety of both
	 * files, then we can exchange the reflink flags at the end.
	 *
	 * Otherwise, we propagate the reflink flag from either file to the
	 * other.
	 */
	if (((rs->ip1_reflink || rs->ip2_reflink) &&
	      rs->ip1_reflink != rs->ip2_reflink) &&
	    startoff1 == 0 && startoff2 == 0 &&
	    blockcount == XFS_B_TO_FSB(mp, ip1->i_d.di_size) &&
	    blockcount == XFS_B_TO_FSB(mp, ip2->i_d.di_size))
		rs->swap_flags = 1;
}

/*
 * If the reflink flag is set on either inode, make sure it has an incore CoW
 * fork, since all reflink inodes must have them.  If there's a CoW fork and it
 * has extents in it, make sure the inodes are tagged appropriately so that
 * speculative preallocations can be GC'd if we run low of space.
 */
static inline void
xfs_swapext_ensure_cowfork(
	struct xfs_inode	*ip)
{
	struct xfs_ifork	*cfork;

	if (xfs_is_reflink_inode(ip))
		xfs_ifork_init_cow(ip);

	cfork = XFS_IFORK_PTR(ip, XFS_COW_FORK);
	if (!cfork)
		return;
	if (cfork->if_bytes > 0)
		xfs_inode_set_cowblocks_tag(ip);
	else
		xfs_inode_clear_cowblocks_tag(ip);
}

/*
 * Set both inodes' ondisk reflink flags to their final state and ensure that
 * the incore state is ready to go.
 */
void
xfs_swapext_reflink_finish(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip1,
	struct xfs_inode	*ip2,
	struct xfs_swapext_reflink_state *rs)
{
	if (!rs->datafork)
		return;

	if (rs->swap_flags) {
		/* Exchange the reflink inode flags and log them. */
		ip1->i_d.di_flags2 &= ~XFS_DIFLAG2_REFLINK;
		if (rs->ip2_reflink)
			ip1->i_d.di_flags2 |= XFS_DIFLAG2_REFLINK;

		ip2->i_d.di_flags2 &= ~XFS_DIFLAG2_REFLINK;
		if (rs->ip1_reflink)
			ip2->i_d.di_flags2 |= XFS_DIFLAG2_REFLINK;

		xfs_trans_log_inode(tp, ip1, XFS_ILOG_CORE);
		xfs_trans_log_inode(tp, ip2, XFS_ILOG_CORE);
	}

	xfs_swapext_ensure_cowfork(ip1);
	xfs_swapext_ensure_cowfork(ip2);
}

/* Schedule an atomic extent swap. */
void
xfs_swapext_schedule(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip1,
	struct xfs_inode	*ip2,
	int			whichfork,
	xfs_fileoff_t		startoff1,
	xfs_fileoff_t		startoff2,
	xfs_filblks_t		blockcount)
{
	struct xfs_swapext_intent *si;

	trace_xfs_swapext_defer(tp->t_mountp, ip1->i_ino, ip2->i_ino,
			whichfork, startoff1, startoff2, blockcount);

	si = kmem_alloc(sizeof(struct xfs_swapext_intent), KM_NOFS);
	INIT_LIST_HEAD(&si->si_list);
	si->si_inode1 = ip1;
	si->si_inode2 = ip2;
	si->si_whichfork = whichfork;
	si->si_startoff1 = startoff1;
	si->si_startoff2 = startoff2;
	si->si_blockcount = blockcount;

	xfs_defer_add(tp, XFS_DEFER_OPS_TYPE_SWAPEXT, &si->si_list);
}

/* Finish one extent swap, possibly log more. */
int
xfs_swapext_finish_one(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip1,
	struct xfs_inode	*ip2,
	int			whichfork,
	xfs_fileoff_t		*startoff1,
	xfs_fileoff_t		*startoff2,
	xfs_filblks_t		*blockcount)
{
	struct xfs_bmbt_irec	irec1, irec2;
	int			nimaps;
	int			bmapi_flags = xfs_bmapi_aflag(whichfork);
	int			error;

	while (*blockcount > 0) {
		/* Read extent from the first file */
		nimaps = 1;
		error = xfs_bmapi_read(ip1, *startoff1, *blockcount, &irec1,
				&nimaps, bmapi_flags);
		if (error)
			return error;
		if (nimaps != 1 ||
		    irec1.br_startblock == DELAYSTARTBLOCK ||
		    irec1.br_startoff != *startoff1) {
			/*
			 * We should never get no mapping or a delalloc extent
			 * or something that doesn't match what we asked for,
			 * since the caller flushed both inodes and we hold the
			 * ILOCKs for both inodes.
			 */
			ASSERT(0);
			return -EINVAL;
		}

		/* Read extent from the second file */
		nimaps = 1;
		error = xfs_bmapi_read(ip2, *startoff2, irec1.br_blockcount,
				&irec2, &nimaps, bmapi_flags);
		if (error)
			return error;;
		if (nimaps != 1 ||
		    irec2.br_startblock == DELAYSTARTBLOCK ||
		    irec2.br_startoff != *startoff2) {
			/*
			 * We should never get no mapping or a delalloc extent
			 * or something that doesn't match what we asked for,
			 * since the caller flushed both inodes and we hold the
			 * ILOCKs for both inodes.
			 */
			ASSERT(0);
			return -EINVAL;
		}

		/*
		 * We can only swap as many blocks as the smaller of the two
		 * extent maps.
		 */
		irec1.br_blockcount = min(irec1.br_blockcount,
					  irec2.br_blockcount);

		trace_xfs_swapext_extent1(ip1, &irec1);
		trace_xfs_swapext_extent2(ip2, &irec2);

		/*
		 * Two extents mapped to the same physical block must not have
		 * different states; that's filesystem corruption.  Move on to
		 * the next extent if they're both holes or both the same
		 * physical extent.
		 */
		if (irec1.br_startblock == irec2.br_startblock) {
			if (irec1.br_state != irec2.br_state)
				return -EFSCORRUPTED;

			*startoff1 += irec1.br_blockcount;
			*startoff2 += irec1.br_blockcount;
			*blockcount -= irec1.br_blockcount;
			continue;
		}

		/* Update quota accounting. */
		if (xfs_bmap_is_real_extent(&irec1)) {
			xfs_trans_mod_dquot_byino(tp, ip1, XFS_TRANS_DQ_BCOUNT,
					-irec1.br_blockcount);
			xfs_trans_mod_dquot_byino(tp, ip2, XFS_TRANS_DQ_BCOUNT,
					irec1.br_blockcount);
		}
		if (xfs_bmap_is_real_extent(&irec2)) {
			xfs_trans_mod_dquot_byino(tp, ip2, XFS_TRANS_DQ_BCOUNT,
				-irec2.br_blockcount);
			xfs_trans_mod_dquot_byino(tp, ip1, XFS_TRANS_DQ_BCOUNT,
				irec2.br_blockcount);
		}

		/* Remove both mappings. */
		xfs_bmap_unmap_extent(tp, ip1, whichfork, &irec1);
		xfs_bmap_unmap_extent(tp, ip2, whichfork, &irec2);

		/*
		 * Re-add both mappings.  We swap the file offsets between the
		 * two maps and add the opposite map, which has the effect of
		 * filling the logical offsets we just unmapped, but with with
		 * the physical mapping information swapped.
		 */
		swap(irec1.br_startoff, irec2.br_startoff);
		xfs_bmap_map_extent(tp, ip1, whichfork, &irec2);
		xfs_bmap_map_extent(tp, ip2, whichfork, &irec1);

		/*
		 * Advance our cursor and exit.   The caller (either defer ops
		 * or log recovery) will log the SXD item, and if *blockcount
		 * is nonzero, it will log a new SXI item for the remainder
		 * and call us back.
		 */
		*startoff1 += irec1.br_blockcount;
		*startoff2 += irec1.br_blockcount;
		*blockcount -= irec1.br_blockcount;
		break;
	}

	return 0;
}

/*
 * Atomically swap a range of extents from one inode to another.
 *
 * The caller must ensure the inodes must be joined to the transaction and
 * ILOCKd; they will still be joined to the transaction at exit.
 */
int
xfs_swapext_atomic(
	struct xfs_trans	**tpp,
	struct xfs_inode	*ip1,
	struct xfs_inode	*ip2,
	int			whichfork,
	xfs_fileoff_t		startoff1,
	xfs_fileoff_t		startoff2,
	xfs_filblks_t		blockcount)
{
	struct xfs_swapext_reflink_state reflink_state = { 0 };
	int			error;

	ASSERT(xfs_isilocked(ip1, XFS_ILOCK_EXCL));
	ASSERT(xfs_isilocked(ip2, XFS_ILOCK_EXCL));
	ASSERT(whichfork != XFS_COW_FORK);

	xfs_swapext_reflink_prep(ip1, ip2, whichfork, startoff1, startoff2,
			blockcount, &reflink_state);

	xfs_swapext_schedule(*tpp, ip1, ip2, whichfork, startoff1,
			startoff2, blockcount);
	error = xfs_defer_finish(tpp);
	if (error)
		return error;

	xfs_swapext_reflink_finish(*tpp, ip1, ip2, &reflink_state);
	return 0;
}

/*
 * Swap a range of extents from one inode to another, non-atomically.
 *
 * Use deferred bmap log items swap a range of extents from one inode with
 * another.  Overall extent swap progress is /not/ tracked through the log,
 * which means that while log recovery can finish remapping a single extent,
 * it cannot finish the entire operation.
 */
int
xfs_swapext_deferred_bmap(
	struct xfs_trans	**tpp,
	struct xfs_inode	*ip1,
	struct xfs_inode	*ip2,
	int			whichfork,
	xfs_fileoff_t		startoff1,
	xfs_fileoff_t		startoff2,
	xfs_filblks_t		blockcount)
{
	struct xfs_swapext_reflink_state reflink_state = { 0 };
	int			error;

	ASSERT(xfs_isilocked(ip1, XFS_ILOCK_EXCL));
	ASSERT(xfs_isilocked(ip2, XFS_ILOCK_EXCL));
	ASSERT(whichfork != XFS_COW_FORK);

	xfs_swapext_reflink_prep(ip1, ip2, whichfork, startoff1, startoff2,
			blockcount, &reflink_state);

	while (blockcount > 0) {
		error = xfs_swapext_finish_one(*tpp, ip1, ip2, whichfork,
				&startoff1, &startoff2, &blockcount);
		if (error)
			return error;
		error = xfs_defer_finish(tpp);
		if (error)
			return error;
	}

	xfs_swapext_reflink_finish(*tpp, ip1, ip2, &reflink_state);
	return 0;
}
