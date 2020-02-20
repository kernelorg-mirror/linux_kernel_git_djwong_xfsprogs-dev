// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2020 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include <libxfs.h>
#include "bload.h"

#define trace_xrep_newbt_claim_block(...)	((void) 0)
#define trace_xrep_newbt_reserve_space(...)	((void) 0)
#define trace_xrep_newbt_unreserve_space(...)	((void) 0)
#define trace_xrep_newbt_claim_block(...)	((void) 0)

int bload_leaf_slack = -1;
int bload_node_slack = -1;

/* Ported routines from fs/xfs/scrub/repair.c */

/*
 * Roll a transaction, keeping the AG headers locked and reinitializing
 * the btree cursors.
 */
int
xrep_roll_ag_trans(
	struct repair_ctx	*sc)
{
	int			error;

	/* Keep the AG header buffers locked so we can keep going. */
	if (sc->agi_bp)
		libxfs_trans_bhold(sc->tp, sc->agi_bp);
	if (sc->agf_bp)
		libxfs_trans_bhold(sc->tp, sc->agf_bp);
	if (sc->agfl_bp)
		libxfs_trans_bhold(sc->tp, sc->agfl_bp);

	/*
	 * Roll the transaction.  We still own the buffer and the buffer lock
	 * regardless of whether or not the roll succeeds.  If the roll fails,
	 * the buffers will be released during teardown on our way out of the
	 * kernel.  If it succeeds, we join them to the new transaction and
	 * move on.
	 */
	error = -libxfs_trans_roll(&sc->tp);
	if (error)
		return error;

	/* Join AG headers to the new transaction. */
	if (sc->agi_bp)
		libxfs_trans_bjoin(sc->tp, sc->agi_bp);
	if (sc->agf_bp)
		libxfs_trans_bjoin(sc->tp, sc->agf_bp);
	if (sc->agfl_bp)
		libxfs_trans_bjoin(sc->tp, sc->agfl_bp);

	return 0;
}

/* Initialize accounting resources for staging a new AG btree. */
void
xrep_newbt_init_ag(
	struct xrep_newbt		*xnr,
	struct repair_ctx		*sc,
	const struct xfs_owner_info	*oinfo,
	xfs_fsblock_t			alloc_hint,
	enum xfs_ag_resv_type		resv)
{
	memset(xnr, 0, sizeof(struct xrep_newbt));
	xnr->sc = sc;
	xnr->oinfo = *oinfo; /* structure copy */
	xnr->alloc_hint = alloc_hint;
	xnr->resv = resv;
	INIT_LIST_HEAD(&xnr->reservations);
}

/* Initialize accounting resources for staging a new inode fork btree. */
void
xrep_newbt_init_inode(
	struct xrep_newbt		*xnr,
	struct repair_ctx		*sc,
	int				whichfork,
	const struct xfs_owner_info	*oinfo)
{
	memset(xnr, 0, sizeof(struct xrep_newbt));
	xnr->sc = sc;
	xnr->oinfo = *oinfo; /* structure copy */
	xnr->alloc_hint = XFS_INO_TO_FSB(sc->mp, sc->ip->i_ino);
	xnr->resv = XFS_AG_RESV_NONE;
	xnr->ifake.if_fork = kmem_zone_zalloc(xfs_ifork_zone, 0);
	xnr->ifake.if_fork_size = XFS_IFORK_SIZE(sc->ip, whichfork);
	INIT_LIST_HEAD(&xnr->reservations);
}

/*
 * Initialize accounting resources for staging a new btree.  Callers are
 * expected to add their own reservations (and clean them up) manually.
 */
void
xrep_newbt_init_bare(
	struct xrep_newbt		*xnr,
	struct repair_ctx		*sc)
{
	xrep_newbt_init_ag(xnr, sc, &XFS_RMAP_OINFO_ANY_OWNER, NULLFSBLOCK,
			XFS_AG_RESV_NONE);
}

/* Add a space reservation manually. */
int
xrep_newbt_add_reservation(
	struct xrep_newbt		*xnr,
	xfs_fsblock_t			fsbno,
	xfs_extlen_t			len,
	void				*priv)
{
	struct xrep_newbt_resv	*resv;

	resv = kmem_alloc(sizeof(struct xrep_newbt_resv), KM_MAYFAIL);
	if (!resv)
		return -ENOMEM;

	INIT_LIST_HEAD(&resv->list);
	resv->fsbno = fsbno;
	resv->len = len;
	resv->used = 0;
	resv->priv = priv;
	list_add_tail(&resv->list, &xnr->reservations);
	return 0;
}

/* Reserve disk space for our new btree. */
int
xrep_newbt_reserve_space(
	struct xrep_newbt	*xnr,
	uint64_t		nr_blocks)
{
	struct repair_ctx	*sc = xnr->sc;
	xfs_alloctype_t		type;
	xfs_fsblock_t		alloc_hint = xnr->alloc_hint;
	int			error = 0;

	type = sc->ip ? XFS_ALLOCTYPE_START_BNO : XFS_ALLOCTYPE_NEAR_BNO;

	while (nr_blocks > 0 && !error) {
		struct xfs_alloc_arg	args = {
			.tp		= sc->tp,
			.mp		= sc->mp,
			.type		= type,
			.fsbno		= alloc_hint,
			.oinfo		= xnr->oinfo,
			.minlen		= 1,
			.maxlen		= nr_blocks,
			.prod		= 1,
			.resv		= xnr->resv,
		};

		error = -libxfs_alloc_vextent(&args);
		if (error)
			return error;
		if (args.fsbno == NULLFSBLOCK)
			return -ENOSPC;

		trace_xrep_newbt_reserve_space(sc->mp,
				XFS_FSB_TO_AGNO(sc->mp, args.fsbno),
				XFS_FSB_TO_AGBNO(sc->mp, args.fsbno),
				args.len, xnr->oinfo.oi_owner);

		/* We don't have real EFIs here so skip that. */

		error = xrep_newbt_add_reservation(xnr, args.fsbno, args.len,
				NULL);
		if (error)
			break;

		nr_blocks -= args.len;
		alloc_hint = args.fsbno + args.len - 1;

		if (sc->ip)
			error = -libxfs_trans_roll_inode(&sc->tp, sc->ip);
		else
			error = xrep_roll_ag_trans(sc);
	}

	return error;
}

/* Free all the accounting infor and disk space we reserved for a new btree. */
void
xrep_newbt_destroy(
	struct xrep_newbt	*xnr,
	int			error)
{
	struct repair_ctx	*sc = xnr->sc;
	struct xrep_newbt_resv	*resv, *n;

	if (error)
		goto junkit;

	list_for_each_entry_safe(resv, n, &xnr->reservations, list) {
		/* We don't have EFIs here so skip the EFD. */

		/* Free every block we didn't use. */
		resv->fsbno += resv->used;
		resv->len -= resv->used;
		resv->used = 0;

		if (resv->len > 0) {
			trace_xrep_newbt_unreserve_space(sc->mp,
					XFS_FSB_TO_AGNO(sc->mp, resv->fsbno),
					XFS_FSB_TO_AGBNO(sc->mp, resv->fsbno),
					resv->len, xnr->oinfo.oi_owner);

			__libxfs_bmap_add_free(sc->tp, resv->fsbno, resv->len,
					&xnr->oinfo, true);
		}

		list_del(&resv->list);
		kmem_free(resv);
	}

junkit:
	list_for_each_entry_safe(resv, n, &xnr->reservations, list) {
		list_del(&resv->list);
		kmem_free(resv);
	}

	if (sc->ip) {
		kmem_cache_free(xfs_ifork_zone, xnr->ifake.if_fork);
		xnr->ifake.if_fork = NULL;
	}
}

/* Feed one of the reserved btree blocks to the bulk loader. */
int
xrep_newbt_claim_block(
	struct xfs_btree_cur	*cur,
	struct xrep_newbt	*xnr,
	union xfs_btree_ptr	*ptr)
{
	struct xrep_newbt_resv	*resv;
	xfs_fsblock_t		fsb;

	/*
	 * If last_resv doesn't have a block for us, move forward until we find
	 * one that does (or run out of reservations).
	 */
	if (xnr->last_resv == NULL) {
		list_for_each_entry(resv, &xnr->reservations, list) {
			if (resv->used < resv->len) {
				xnr->last_resv = resv;
				break;
			}
		}
		if (xnr->last_resv == NULL)
			return -ENOSPC;
	} else if (xnr->last_resv->used == xnr->last_resv->len) {
		if (xnr->last_resv->list.next == &xnr->reservations)
			return -ENOSPC;
		xnr->last_resv = list_entry(xnr->last_resv->list.next,
				struct xrep_newbt_resv, list);
	}

	/* Nab the block. */
	fsb = xnr->last_resv->fsbno + xnr->last_resv->used;
	xnr->last_resv->used++;

	trace_xrep_newbt_claim_block(cur->bc_mp,
			XFS_FSB_TO_AGNO(cur->bc_mp, fsb),
			XFS_FSB_TO_AGBNO(cur->bc_mp, fsb),
			xnr->oinfo.oi_owner);

	if (cur->bc_flags & XFS_BTREE_LONG_PTRS)
		ptr->l = cpu_to_be64(fsb);
	else
		ptr->s = cpu_to_be32(XFS_FSB_TO_AGBNO(cur->bc_mp, fsb));
	return 0;
}

/*
 * Estimate proper slack values for a btree that's being reloaded.
 *
 * Under most circumstances, we'll take whatever default loading value the
 * btree bulk loading code calculates for us.  However, there are some
 * exceptions to this rule:
 *
 * (1) If someone turned one of the debug knobs.
 * (2) The FS has less than ~9% space free.
 *
 * Note that we actually use 3/32 for the comparison to avoid division.
 */
void
estimate_inode_bload_slack(
	struct xfs_mount	*mp,
	struct xfs_btree_bload	*bload)
{
	/*
	 * The global values are set to -1 (i.e. take the bload defaults)
	 * unless someone has set them otherwise, so we just pull the values
	 * here.
	 */
	bload->leaf_slack = bload_leaf_slack;
	bload->node_slack = bload_node_slack;

	/* No further changes if there's more than 3/32ths space left. */
	if (mp->m_sb.sb_fdblocks >= ((mp->m_sb.sb_dblocks * 3) >> 5))
		return;

	/* We're low on space; load the btrees as tightly as possible. */
	if (bload->leaf_slack < 0)
		bload->leaf_slack = 0;
	if (bload->node_slack < 0)
		bload->node_slack = 0;
}
