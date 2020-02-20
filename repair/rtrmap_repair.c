// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2020 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include <libxfs.h>
#include "btree.h"
#include "err_protos.h"
#include "libxlog.h"
#include "incore.h"
#include "globals.h"
#include "dinode.h"
#include "slab.h"
#include "rmap.h"
#include "bulkload.h"

/* Ported routines from fs/xfs/scrub/rtrmap_repair.c */

/*
 * Realtime Reverse Mapping (RTRMAPBT) Repair
 * ==========================================
 *
 * Gather all the rmap records for the inode and fork we're fixing, reset the
 * incore fork, then recreate the btree.
 */
struct xrep_rtrmap {
	/* rtrmapbt slab cursor */
	struct xfs_slab_cursor	*slab_cursor;

	/* New fork. */
	struct bulkload		new_fork_info;
	struct xfs_btree_bload	rtrmap_bload;

	struct repair_ctx	*sc;
};

/* Retrieve rtrmap data for bulk load. */
STATIC int
xrep_rtrmap_get_record(
	struct xfs_btree_cur	*cur,
	void			*priv)
{
	struct xfs_rmap_irec	*rec;
	struct xrep_rtrmap	*rr = priv;

	rec = pop_slab_cursor(rr->slab_cursor);
	memcpy(&cur->bc_rec.r, rec, sizeof(struct xfs_rmap_irec));
	return 0;
}

/* Feed one of the new btree blocks to the bulk loader. */
STATIC int
xrep_rtrmap_claim_block(
	struct xfs_btree_cur	*cur,
	union xfs_btree_ptr	*ptr,
	void			*priv)
{
	struct xrep_rtrmap	*rr = priv;

	return bulkload_claim_block(cur, &rr->new_fork_info, ptr);
}

/* Figure out how much space we need to create the incore btree root block. */
STATIC size_t
xrep_rtrmap_iroot_size(
	struct xfs_btree_cur	*cur,
	unsigned int		nr_this_level,
	void			*priv)
{
	return XFS_RTRMAP_BROOT_SPACE_CALC(nr_this_level, cur->bc_nlevels - 1);
}

/* Reserve new btree blocks and bulk load all the rtrmap records. */
STATIC int
xrep_rtrmap_btree_load(
	struct xrep_rtrmap	*rr,
	struct xfs_btree_cur	*rtrmap_cur)
{
	struct repair_ctx	*sc = rr->sc;
	int			error;

	rr->rtrmap_bload.get_record = xrep_rtrmap_get_record;
	rr->rtrmap_bload.claim_block = xrep_rtrmap_claim_block;
	rr->rtrmap_bload.iroot_size = xrep_rtrmap_iroot_size;
	bulkload_estimate_inode_slack(sc->mp, &rr->rtrmap_bload);

	/* Compute how many blocks we'll need. */
	error = -libxfs_btree_bload_compute_geometry(rtrmap_cur,
			&rr->rtrmap_bload,
			rmap_record_count(sc->mp, NULLAGNUMBER));
	if (error)
		return error;

	/*
	 * Guess how many blocks we're going to need to rebuild an entire rtrmap
	 * from the number of extents we found, and pump up our transaction to
	 * have sufficient block reservation.
	 */
	error = -libxfs_trans_reserve_more(sc->tp, rr->rtrmap_bload.nr_blocks,
			0);
	if (error)
		return error;

	/*
	 * Reserve the space we'll need for the new btree.  Drop the cursor
	 * while we do this because that can roll the transaction and cursors
	 * can't handle that.
	 */
	error = bulkload_alloc_blocks(&rr->new_fork_info,
			rr->rtrmap_bload.nr_blocks);
	if (error)
		return error;

	/* Add all observed rtrmap records. */
	error = rmap_init_cursor(NULLAGNUMBER, &rr->slab_cursor);
	if (error)
		return error;
	error = -libxfs_btree_bload(rtrmap_cur, &rr->rtrmap_bload, rr);
	free_slab_cursor(&rr->slab_cursor);
	return error;
}

/* Update the inode counters. */
STATIC int
xrep_rtrmap_reset_counters(
	struct xrep_rtrmap	*rr)
{
	struct repair_ctx	*sc = rr->sc;

	/*
	 * Update the inode block counts to reflect the btree we just
	 * generated.
	 */
	sc->ip->i_d.di_nblocks = rr->new_fork_info.ifake.if_blocks;
	libxfs_trans_log_inode(sc->tp, sc->ip, XFS_ILOG_CORE);

	/* Quotas don't exist so we're done. */
	return 0;
}

/*
 * Use the collected rmap information to stage a new rt rmap btree.  If this is
 * successful we'll return with the new btree root information logged to the
 * repair transaction but not yet committed.
 */
static int
xrep_rtrmap_build_new_tree(
	struct xrep_rtrmap	*rr)
{
	struct xfs_owner_info	oinfo;
	struct xfs_btree_cur	*cur;
	struct repair_ctx	*sc = rr->sc;
	struct xbtree_ifakeroot	*ifake = &rr->new_fork_info.ifake;
	int			error;

	/*
	 * Prepare to construct the new fork by initializing the new btree
	 * structure and creating a fake ifork in the ifakeroot structure.
	 */
	libxfs_rmap_ino_bmbt_owner(&oinfo, sc->ip->i_ino, XFS_DATA_FORK);
	bulkload_init_inode(&rr->new_fork_info, sc, XFS_DATA_FORK, &oinfo);
	cur = libxfs_rtrmapbt_stage_cursor(sc->mp, sc->ip, ifake);

	/*
	 * Figure out the size and format of the new fork, then fill it with
	 * all the rtrmap records we've found.  Join the inode to the
	 * transaction so that we can roll the transaction while holding the
	 * inode locked.
	 */
	libxfs_trans_ijoin(sc->tp, sc->ip, 0);
	ifake->if_format = XFS_DINODE_FMT_RMAP;
	error = xrep_rtrmap_btree_load(rr, cur);
	if (error)
		goto err_cur;

	/*
	 * Install the new fork in the inode.  After this point the old mapping
	 * data are no longer accessible and the new tree is live.  We delete
	 * the cursor immediately after committing the staged root because the
	 * staged fork might be in extents format.
	 */
	libxfs_rtrmapbt_commit_staged_btree(cur, sc->tp);
	libxfs_btree_del_cursor(cur, 0);

	/* Reset the inode counters now that we've changed the fork. */
	error = xrep_rtrmap_reset_counters(rr);
	if (error)
		goto err_newbt;

	/* Dispose of any unused blocks and the accounting infomation. */
	bulkload_destroy(&rr->new_fork_info, error);

	return -libxfs_trans_roll_inode(&sc->tp, sc->ip);
err_cur:
	if (cur)
		libxfs_btree_del_cursor(cur, error);
err_newbt:
	bulkload_destroy(&rr->new_fork_info, error);
	return error;
}

/* Store the realtime reverse-mappings in the rtrmapbt. */
int
rmap_populate_realtime_rmapbt(
	struct xfs_mount	*mp)
{
	struct repair_ctx	sc = {
		.mp		= mp,
	};
	struct xrep_rtrmap	rr = {
		.sc		= &sc,
	};
	xfs_ino_t		ino;
	int			error;

	if (!xfs_sb_version_hasrtrmapbt(&mp->m_sb))
		return 0;

	error = -libxfs_imeta_lookup(mp, &XFS_IMETA_RTRMAPBT, &ino);
	if (error)
		return error;

	error = -libxfs_imeta_iget(mp, ino, XFS_DIR3_FT_REG_FILE, &sc.ip);
	if (error)
		return error;

	error = -libxfs_trans_alloc(mp, &M_RES(mp)->tr_itruncate, 0, 0, 0,
			&sc.tp);
	if (error)
		goto out_rele;

	error = xrep_rtrmap_build_new_tree(&rr);
	if (error)
		goto out_cancel;

	error = -libxfs_trans_commit(sc.tp);

out_cancel:
	libxfs_trans_cancel(sc.tp);
out_rele:
	libxfs_imeta_irele(sc.ip);
	return error;
}
