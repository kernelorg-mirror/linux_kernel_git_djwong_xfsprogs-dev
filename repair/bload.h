// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2020 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef __XFS_REPAIR_BLOAD_H__
#define __XFS_REPAIR_BLOAD_H__

extern int bload_leaf_slack;
extern int bload_node_slack;

struct repair_ctx {
	struct xfs_mount	*mp;
	struct xfs_inode	*ip;
	struct xfs_trans	*tp;

	struct xfs_buf		*agi_bp;
	struct xfs_buf		*agf_bp;
	struct xfs_buf		*agfl_bp;
};

struct xrep_newbt_resv {
	/* Link to list of extents that we've reserved. */
	struct list_head	list;

	/* FSB of the block we reserved. */
	xfs_fsblock_t		fsbno;

	/* Length of the reservation. */
	xfs_extlen_t		len;

	/* How much of this reservation we've used. */
	xfs_extlen_t		used;
};

struct xrep_newbt {
	struct repair_ctx	*sc;

	/* List of extents that we've reserved. */
	struct list_head	resv_list;

	/* Fake root for new btree. */
	union {
		struct xbtree_afakeroot	afake;
		struct xbtree_ifakeroot	ifake;
	};

	/* rmap owner of these blocks */
	struct xfs_owner_info	oinfo;

	/* The last reservation we allocated from. */
	struct xrep_newbt_resv	*last_resv;

	/* Allocation hint */
	xfs_fsblock_t		alloc_hint;

	/* per-ag reservation type */
	enum xfs_ag_resv_type	resv;
};

#define for_each_xrep_newbt_reservation(xnr, resv, n)	\
	list_for_each_entry_safe((resv), (n), &(xnr)->resv_list, list)

void xrep_newbt_init_bare(struct xrep_newbt *xnr, struct repair_ctx *sc);
void xrep_newbt_init_ag(struct xrep_newbt *xnr, struct repair_ctx *sc,
		const struct xfs_owner_info *oinfo, xfs_fsblock_t alloc_hint,
		enum xfs_ag_resv_type resv);
void xrep_newbt_init_inode(struct xrep_newbt *xnr, struct repair_ctx *sc,
		int whichfork, const struct xfs_owner_info *oinfo);
int xrep_newbt_add_blocks(struct xrep_newbt *xnr, xfs_fsblock_t fsbno,
		xfs_extlen_t len);
int xrep_newbt_alloc_blocks(struct xrep_newbt *xnr, uint64_t nr_blocks);
void xrep_newbt_destroy(struct xrep_newbt *xnr, int error);
int xrep_newbt_claim_block(struct xfs_btree_cur *cur, struct xrep_newbt *xnr,
		union xfs_btree_ptr *ptr);

void estimate_inode_bload_slack(struct xfs_mount *mp,
		struct xfs_btree_bload *bload);

#endif /* __XFS_REPAIR_BLOAD_H__ */
