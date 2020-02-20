// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2001,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "libxfs.h"
#include "avl.h"
#include "globals.h"
#include "agheader.h"
#include "incore.h"
#include "protos.h"
#include "err_protos.h"
#include "dinode.h"
#include "rt.h"
#include "versions.h"
#include "threads.h"
#include "progress.h"
#include "slab.h"
#include "rmap.h"
#include "bload.h"

/* Context for rebuilding a per-AG btree. */
struct bt_rebuild {
	/* Fake root for staging and space preallocations. */
	struct xrep_newbt	newbt;

	/* Geometry of the new btree. */
	struct xfs_btree_bload	bload;

	/* Staging btree cursor for the new tree. */
	struct xfs_btree_cur	*cur;

	/* Tree-specific data. */
	union {
		struct xfs_slab_cursor	*slab_cursor;
		struct {
			struct extent_tree_node	*bno_rec;
			xfs_agblock_t		*freeblks;
		};
		struct {
			struct ino_tree_node	*ino_rec;
			struct agi_stat		*agi_stat;
		};
	};
};

struct lost_fsb {
	xfs_fsblock_t		fsbno;
	xfs_extlen_t		len;
};


/*
 * extra metadata for the agi
 */
struct agi_stat {
	xfs_agino_t		first_agino;
	xfs_agino_t		count;
	xfs_agino_t		freecount;
};

static uint64_t	*sb_icount_ag;		/* allocated inodes per ag */
static uint64_t	*sb_ifree_ag;		/* free inodes per ag */
static uint64_t	*sb_fdblocks_ag;	/* free data blocks per ag */

static int
mk_incore_fstree(
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno,
	unsigned int		*num_freeblocks)
{
	int			in_extent;
	int			num_extents;
	xfs_agblock_t		extent_start;
	xfs_extlen_t		extent_len;
	xfs_agblock_t		agbno;
	xfs_agblock_t		ag_end;
	uint			free_blocks;
	xfs_extlen_t		blen;
	int			bstate;

	*num_freeblocks = 0;

	/*
	 * scan the bitmap for the ag looking for continuous
	 * extents of free blocks.  At this point, we know
	 * that blocks in the bitmap are either set to an
	 * "in use" state or set to unknown (0) since the
	 * bmaps were zero'ed in phase 4 and only blocks
	 * being used by inodes, inode bmaps, ag headers,
	 * and the files themselves were put into the bitmap.
	 *
	 */
	ASSERT(agno < mp->m_sb.sb_agcount);

	extent_start = extent_len = 0;
	in_extent = 0;
	num_extents = free_blocks = 0;

	if (agno < mp->m_sb.sb_agcount - 1)
		ag_end = mp->m_sb.sb_agblocks;
	else
		ag_end = mp->m_sb.sb_dblocks -
			(xfs_rfsblock_t)mp->m_sb.sb_agblocks *
                       (mp->m_sb.sb_agcount - 1);

	/*
	 * ok, now find the number of extents, keep track of the
	 * largest extent.
	 */
	for (agbno = 0; agbno < ag_end; agbno += blen) {
		bstate = get_bmap_ext(agno, agbno, ag_end, &blen);
		if (bstate < XR_E_INUSE)  {
			free_blocks += blen;
			if (in_extent == 0)  {
				/*
				 * found the start of a free extent
				 */
				in_extent = 1;
				num_extents++;
				extent_start = agbno;
				extent_len = blen;
			} else  {
				extent_len += blen;
			}
		} else   {
			if (in_extent)  {
				/*
				 * free extent ends here, add extent to the
				 * 2 incore extent (avl-to-be-B+) trees
				 */
				in_extent = 0;
#if defined(XR_BLD_FREE_TRACE) && defined(XR_BLD_ADD_EXTENT)
				fprintf(stderr, "adding extent %u [%u %u]\n",
					agno, extent_start, extent_len);
#endif
				add_bno_extent(agno, extent_start, extent_len);
				add_bcnt_extent(agno, extent_start, extent_len);
				*num_freeblocks += extent_len;
			}
		}
	}
	if (in_extent)  {
		/*
		 * free extent ends here
		 */
#if defined(XR_BLD_FREE_TRACE) && defined(XR_BLD_ADD_EXTENT)
		fprintf(stderr, "adding extent %u [%u %u]\n",
			agno, extent_start, extent_len);
#endif
		add_bno_extent(agno, extent_start, extent_len);
		add_bcnt_extent(agno, extent_start, extent_len);
		*num_freeblocks += extent_len;
	}

	return(num_extents);
}

/*
 * Estimate proper slack values for a btree that's being reloaded.
 *
 * Under most circumstances, we'll take whatever default loading value the
 * btree bulk loading code calculates for us.  However, there are some
 * exceptions to this rule:
 *
 * (1) If someone turned one of the debug knobs.
 * (2) The AG has less than ~9% space free.
 *
 * Note that we actually use 3/32 for the comparison to avoid division.
 */
static void
estimate_ag_bload_slack(
	struct repair_ctx	*sc,
	struct xfs_btree_bload	*bload,
	unsigned int		free)
{
	/*
	 * The global values are set to -1 (i.e. take the bload defaults)
	 * unless someone has set them otherwise, so we just pull the values
	 * here.
	 */
	bload->leaf_slack = bload_leaf_slack;
	bload->node_slack = bload_node_slack;

	/* No further changes if there's more than 3/32ths space left. */
	if (free >= ((sc->mp->m_sb.sb_agblocks * 3) >> 5))
		return;

	/* We're low on space; load the btrees as tightly as possible. */
	if (bload->leaf_slack < 0)
		bload->leaf_slack = 0;
	if (bload->node_slack < 0)
		bload->node_slack = 0;
}

/* Initialize a btree rebuild context. */
static void
init_rebuild(
	struct repair_ctx		*sc,
	const struct xfs_owner_info	*oinfo,
	xfs_agblock_t			free_space,
	struct bt_rebuild		*btr)
{
	memset(btr, 0, sizeof(struct bt_rebuild));

	xrep_newbt_init_bare(&btr->newbt, sc);
	btr->newbt.oinfo = *oinfo; /* struct copy */
	estimate_ag_bload_slack(sc, &btr->bload, free_space);
}

/* Reserve blocks for the new btree. */
static void
setup_rebuild(
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno,
	struct bt_rebuild	*btr,
	uint32_t		nr_blocks)
{
	struct extent_tree_node	*ext_ptr;
	struct extent_tree_node	*bno_ext_ptr;
	uint32_t		blocks_allocated = 0;
	int			error;

	/*
	 * grab the smallest extent and use it up, then get the
	 * next smallest.  This mimics the init_*_cursor code.
	 */
	ext_ptr =  findfirst_bcnt_extent(agno);

	/*
	 * set up the free block array
	 */
	while (blocks_allocated < nr_blocks)  {
		uint64_t	len;
		xfs_agblock_t	new_start;
		xfs_extlen_t	new_len;

		if (!ext_ptr)
			do_error(
_("error - not enough free space in filesystem\n"));

		/* Use up the extent we've got. */
		len = min(ext_ptr->ex_blockcount,
				btr->bload.nr_blocks - blocks_allocated);
		error = xrep_newbt_add_reservation(&btr->newbt,
				XFS_AGB_TO_FSB(mp, agno,
					       ext_ptr->ex_startblock),
				len, NULL);
		if (error)
			do_error(_("could not set up btree reservation: %s\n"),
				strerror(-error));
		blocks_allocated += len;

		error = rmap_add_ag_rec(mp, agno, ext_ptr->ex_startblock, len,
				btr->newbt.oinfo.oi_owner);
		if (error)
			do_error(_("could not set up btree rmaps: %s\n"),
				strerror(-error));

		/* Figure out if we're putting anything back. */
		new_start = ext_ptr->ex_startblock + len;
		new_len = ext_ptr->ex_blockcount - len;

		/* Delete the used-up extent from both extent trees. */
#ifdef XR_BLD_FREE_TRACE
		fprintf(stderr, "releasing extent: %u [%u %u]\n",
			agno, ext_ptr->ex_startblock, ext_ptr->ex_blockcount);
#endif
		bno_ext_ptr = find_bno_extent(agno, ext_ptr->ex_startblock);
		ASSERT(bno_ext_ptr != NULL);
		get_bno_extent(agno, bno_ext_ptr);
		release_extent_tree_node(bno_ext_ptr);

		ext_ptr = get_bcnt_extent(agno, ext_ptr->ex_startblock,
				ext_ptr->ex_blockcount);
		ASSERT(ext_ptr != NULL);
		release_extent_tree_node(ext_ptr);

		/*
		 * If we only used part of this last extent, then we need only
		 * to reinsert the extent in the extent trees and we're done.
		 */
		if (new_len > 0) {
			add_bno_extent(agno, new_start, new_len);
			add_bcnt_extent(agno, new_start, new_len);
			break;
		}

		/* Otherwise, find the next biggest extent. */
		ext_ptr = findfirst_bcnt_extent(agno);
	}
#ifdef XR_BLD_FREE_TRACE
	fprintf(stderr, "blocks_allocated = %d\n",
		blocks_allocated);
#endif
}

/* Feed one of the new btree blocks to the bulk loader. */
static int
rebuild_claim_block(
	struct xfs_btree_cur	*cur,
	union xfs_btree_ptr	*ptr,
	void			*priv)
{
	struct bt_rebuild	*btr = priv;

	return xrep_newbt_claim_block(cur, &btr->newbt, ptr);
}

/*
 * Scoop up leftovers from a rebuild cursor for later freeing, then free the
 * rebuild context.
 */
static void
finish_rebuild(
	struct xfs_mount	*mp,
	struct bt_rebuild	*btr,
	struct xfs_slab		*lost_fsbs)
{
	struct xrep_newbt_resv	*resv, *n;

	for_each_xrep_newbt_reservation(&btr->newbt, resv, n) {
		struct lost_fsb	lost;
		int		error;

		if (resv->used == resv->len)
			continue;

		lost.fsbno = resv->fsbno + resv->used;
		lost.len = resv->len - resv->used;
		error = slab_add(lost_fsbs, &lost);
		if (error)
			do_error(
_("Insufficient memory saving lost blocks.\n"));
		resv->used = resv->len;
	}

	xrep_newbt_destroy(&btr->newbt, 0);
}

/*
 * Free Space Btrees
 *
 * We need to leave some free records in the tree for the corner case of
 * setting up the AGFL. This may require allocation of blocks, and as
 * such can require insertion of new records into the tree (e.g. moving
 * a record in the by-count tree when a long extent is shortened). If we
 * pack the records into the leaves with no slack space, this requires a
 * leaf split to occur and a block to be allocated from the free list.
 * If we don't have any blocks on the free list (because we are setting
 * it up!), then we fail, and the filesystem will fail with the same
 * failure at runtime. Hence leave a couple of records slack space in
 * each block to allow immediate modification of the tree without
 * requiring splits to be done.
 */

static void
init_freespace_cursors(
	struct repair_ctx	*sc,
	xfs_agnumber_t		agno,
	unsigned int		free_space,
	unsigned int		*nr_extents,
	int			*extra_blocks,
	struct bt_rebuild	*btr_bno,
	struct bt_rebuild	*btr_cnt)
{
	unsigned int		bno_blocks;
	unsigned int		cnt_blocks;
	int			error;

	init_rebuild(sc, &XFS_RMAP_OINFO_AG, free_space, btr_bno);
	init_rebuild(sc, &XFS_RMAP_OINFO_AG, free_space, btr_cnt);

	btr_bno->cur = libxfs_allocbt_stage_cursor(sc->mp,
			&btr_bno->newbt.afake, agno, XFS_BTNUM_BNO);
	btr_cnt->cur = libxfs_allocbt_stage_cursor(sc->mp,
			&btr_cnt->newbt.afake, agno, XFS_BTNUM_CNT);

	/*
	 * Now we need to allocate blocks for the free space btrees using the
	 * free space records we're about to put in them.  Every record we use
	 * can change the shape of the free space trees, so we recompute the
	 * btree shape until we stop needing /more/ blocks.  If we have any
	 * left over we'll stash them in the AGFL when we're done.
	 */
	do {
		unsigned int	num_freeblocks;

		bno_blocks = btr_bno->bload.nr_blocks;
		cnt_blocks = btr_cnt->bload.nr_blocks;

		/* Compute how many bnobt blocks we'll need. */
		error = -libxfs_btree_bload_compute_geometry(btr_bno->cur,
				&btr_bno->bload, *nr_extents);
		if (error)
			do_error(
_("Unable to compute free space by block btree geometry, error %d.\n"), -error);

		/* Compute how many cntbt blocks we'll need. */
		error = -libxfs_btree_bload_compute_geometry(btr_bno->cur,
				&btr_cnt->bload, *nr_extents);
		if (error)
			do_error(
_("Unable to compute free space by length btree geometry, error %d.\n"), -error);

		/* We don't need any more blocks, so we're done. */
		if (bno_blocks >= btr_bno->bload.nr_blocks &&
		    cnt_blocks >= btr_cnt->bload.nr_blocks)
			break;

		/* Allocate however many more blocks we need this time. */
		if (bno_blocks < btr_bno->bload.nr_blocks)
			setup_rebuild(sc->mp, agno, btr_bno,
					btr_bno->bload.nr_blocks - bno_blocks);
		if (cnt_blocks < btr_cnt->bload.nr_blocks)
			setup_rebuild(sc->mp, agno, btr_cnt,
					btr_cnt->bload.nr_blocks - cnt_blocks);

		/* Ok, now how many free space records do we have? */
		*nr_extents = count_bno_extents_blocks(agno, &num_freeblocks);
	} while (1);

	*extra_blocks = (bno_blocks - btr_bno->bload.nr_blocks) +
			(cnt_blocks - btr_cnt->bload.nr_blocks);
}

static void
get_freesp_data(
	struct xfs_btree_cur		*cur,
	struct extent_tree_node		*bno_rec,
	xfs_agblock_t			*freeblks)
{
	struct xfs_alloc_rec_incore	*arec = &cur->bc_rec.a;

	arec->ar_startblock = bno_rec->ex_startblock;
	arec->ar_blockcount = bno_rec->ex_blockcount;
	if (freeblks)
		*freeblks += bno_rec->ex_blockcount;
}

/* Grab one bnobt record. */
static int
get_bnobt_record(
	struct xfs_btree_cur		*cur,
	void				*priv)
{
	struct bt_rebuild		*btr = priv;

	get_freesp_data(cur, btr->bno_rec, btr->freeblks);
	btr->bno_rec = findnext_bno_extent(btr->bno_rec);
	return 0;
}

/* Rebuild a free space by block number btree. */
static void
build_bnobt(
	struct repair_ctx	*sc,
	xfs_agnumber_t		agno,
	struct bt_rebuild	*btr_bno,
	xfs_agblock_t		*freeblks)
{
	int			error;

	*freeblks = 0;
	btr_bno->bload.get_record = get_bnobt_record;
	btr_bno->bload.claim_block = rebuild_claim_block;
	btr_bno->bno_rec = findfirst_bno_extent(agno);
	btr_bno->freeblks = freeblks;

	error = -libxfs_trans_alloc_empty(sc->mp, &sc->tp);
	if (error)
		do_error(
_("Insufficient memory to construct bnobt rebuild transaction.\n"));

	/* Add all observed bnobt records. */
	error = -libxfs_btree_bload(btr_bno->cur, &btr_bno->bload, btr_bno);
	if (error)
		do_error(
_("Error %d while creating bnobt btree for AG %u.\n"), error, agno);

	/* Since we're not writing the AGF yet, no need to commit the cursor */
	libxfs_btree_del_cursor(btr_bno->cur, 0);
	error = -libxfs_trans_commit(sc->tp);
	if (error)
		do_error(
_("Error %d while writing bnobt btree for AG %u.\n"), error, agno);
	sc->tp = NULL;
}

/* Grab one cntbt record. */
static int
get_cntbt_record(
	struct xfs_btree_cur		*cur,
	void				*priv)
{
	struct bt_rebuild		*btr = priv;

	get_freesp_data(cur, btr->bno_rec, btr->freeblks);
	btr->bno_rec = findnext_bcnt_extent(cur->bc_ag.agno, btr->bno_rec);
	return 0;
}

/* Rebuild a freespace by count btree. */
static void
build_cntbt(
	struct repair_ctx	*sc,
	xfs_agnumber_t		agno,
	struct bt_rebuild	*btr_cnt,
	xfs_agblock_t		*freeblks)
{
	int			error;

	*freeblks = 0;
	btr_cnt->bload.get_record = get_cntbt_record;
	btr_cnt->bload.claim_block = rebuild_claim_block;
	btr_cnt->bno_rec = findfirst_bcnt_extent(agno);
	btr_cnt->freeblks = freeblks;

	error = -libxfs_trans_alloc_empty(sc->mp, &sc->tp);
	if (error)
		do_error(
_("Insufficient memory to construct cntbt rebuild transaction.\n"));

	/* Add all observed cntbt records. */
	error = -libxfs_btree_bload(btr_cnt->cur, &btr_cnt->bload, btr_cnt);
	if (error)
		do_error(
_("Error %d while creating cntbt btree for AG %u.\n"), error, agno);

	/* Since we're not writing the AGF yet, no need to commit the cursor */
	libxfs_btree_del_cursor(btr_cnt->cur, 0);
	error = -libxfs_trans_commit(sc->tp);
	if (error)
		do_error(
_("Error %d while writing cntbt btree for AG %u.\n"), error, agno);
	sc->tp = NULL;
}

/* Inode Btrees */

/* Initialize both inode btree cursors as needed. */
static void
init_ino_cursors(
	struct repair_ctx	*sc,
	xfs_agnumber_t		agno,
	unsigned int		free_space,
	uint64_t		*num_inos,
	uint64_t		*num_free_inos,
	struct bt_rebuild	*btr_ino,
	struct bt_rebuild	*btr_fino)
{
	struct ino_tree_node	*ino_rec;
	unsigned int		ino_recs = 0;
	unsigned int		fino_recs = 0;
	bool			finobt;
	int			error;

	finobt = xfs_sb_version_hasfinobt(&sc->mp->m_sb);
	init_rebuild(sc, &XFS_RMAP_OINFO_INOBT, free_space, btr_ino);

	/* Compute inode statistics. */
	*num_free_inos = 0;
	*num_inos = 0;
	for (ino_rec = findfirst_inode_rec(agno);
	     ino_rec != NULL;
	     ino_rec = next_ino_rec(ino_rec))  {
		unsigned int	rec_ninos = 0;
		unsigned int	rec_nfinos = 0;
		int		i;

		for (i = 0; i < XFS_INODES_PER_CHUNK; i++)  {
			ASSERT(is_inode_confirmed(ino_rec, i));
			/*
			 * sparse inodes are not factored into superblock (free)
			 * inode counts
			 */
			if (is_inode_sparse(ino_rec, i))
				continue;
			if (is_inode_free(ino_rec, i))
				rec_nfinos++;
			rec_ninos++;
		}

		*num_free_inos += rec_nfinos;
		*num_inos += rec_ninos;
		ino_recs++;

		/* finobt only considers records with free inodes */
		if (rec_nfinos)
			fino_recs++;
	}

	btr_ino->cur = libxfs_inobt_stage_cursor(sc->mp, &btr_ino->newbt.afake,
			agno, XFS_BTNUM_INO);

	/* Compute how many inobt blocks we'll need. */
	error = -libxfs_btree_bload_compute_geometry(btr_ino->cur,
			&btr_ino->bload, ino_recs);
	if (error)
		do_error(
_("Unable to compute inode btree geometry, error %d.\n"), error);

	setup_rebuild(sc->mp, agno, btr_ino, btr_ino->bload.nr_blocks);

	if (!finobt)
		return;

	init_rebuild(sc, &XFS_RMAP_OINFO_INOBT, free_space, btr_fino);
	btr_fino->cur = libxfs_inobt_stage_cursor(sc->mp,
			&btr_fino->newbt.afake, agno, XFS_BTNUM_FINO);

	/* Compute how many finobt blocks we'll need. */
	error = -libxfs_btree_bload_compute_geometry(btr_fino->cur,
			&btr_fino->bload, fino_recs);
	if (error)
		do_error(
_("Unable to compute free inode btree geometry, error %d.\n"), error);

	setup_rebuild(sc->mp, agno, btr_fino, btr_fino->bload.nr_blocks);
}

/* Copy one incore inode record into the inobt cursor. */
static void
get_inode_data(
	struct xfs_btree_cur		*cur,
	struct ino_tree_node		*ino_rec,
	struct agi_stat			*agi_stat)
{
	struct xfs_inobt_rec_incore	*irec = &cur->bc_rec.i;
	int				inocnt = 0;
	int				finocnt = 0;
	int				k;

	irec->ir_startino = ino_rec->ino_startnum;
	irec->ir_free = ino_rec->ir_free;

	for (k = 0; k < sizeof(xfs_inofree_t) * NBBY; k++)  {
		ASSERT(is_inode_confirmed(ino_rec, k));

		if (is_inode_sparse(ino_rec, k))
			continue;
		if (is_inode_free(ino_rec, k))
			finocnt++;
		inocnt++;
	}

	irec->ir_count = inocnt;
	irec->ir_freecount = finocnt;

	if (xfs_sb_version_hassparseinodes(&cur->bc_mp->m_sb)) {
		uint64_t		sparse;
		int			spmask;
		uint16_t		holemask;

		/*
		 * Convert the 64-bit in-core sparse inode state to the
		 * 16-bit on-disk holemask.
		 */
		holemask = 0;
		spmask = (1 << XFS_INODES_PER_HOLEMASK_BIT) - 1;
		sparse = ino_rec->ir_sparse;
		for (k = 0; k < XFS_INOBT_HOLEMASK_BITS; k++) {
			if (sparse & spmask) {
				ASSERT((sparse & spmask) == spmask);
				holemask |= (1 << k);
			} else
				ASSERT((sparse & spmask) == 0);
			sparse >>= XFS_INODES_PER_HOLEMASK_BIT;
		}

		irec->ir_holemask = holemask;
	} else {
		irec->ir_holemask = 0;
	}

	if (!agi_stat)
		return;

	if (agi_stat->first_agino != NULLAGINO)
		agi_stat->first_agino = ino_rec->ino_startnum;
	agi_stat->freecount += finocnt;
	agi_stat->count += inocnt;
}

/* Grab one inobt record. */
static int
get_inobt_record(
	struct xfs_btree_cur		*cur,
	void				*priv)
{
	struct bt_rebuild		*rebuild = priv;

	get_inode_data(cur, rebuild->ino_rec, rebuild->agi_stat);
	rebuild->ino_rec = next_ino_rec(rebuild->ino_rec);
	return 0;
}

/* Rebuild a inobt btree. */
static void
build_inobt(
	struct repair_ctx	*sc,
	xfs_agnumber_t		agno,
	struct bt_rebuild	*btr_ino,
	struct agi_stat		*agi_stat)
{
	int			error;

	btr_ino->bload.get_record = get_inobt_record;
	btr_ino->bload.claim_block = rebuild_claim_block;
	agi_stat->count = agi_stat->freecount = 0;
	agi_stat->first_agino = NULLAGINO;
	btr_ino->agi_stat = agi_stat;
	btr_ino->ino_rec = findfirst_inode_rec(agno);

	error = -libxfs_trans_alloc_empty(sc->mp, &sc->tp);
	if (error)
		do_error(
_("Insufficient memory to construct inobt rebuild transaction.\n"));

	/* Add all observed inobt records. */
	error = -libxfs_btree_bload(btr_ino->cur, &btr_ino->bload, btr_ino);
	if (error)
		do_error(
_("Error %d while creating inobt btree for AG %u.\n"), error, agno);

	/* Since we're not writing the AGI yet, no need to commit the cursor */
	libxfs_btree_del_cursor(btr_ino->cur, 0);
	error = -libxfs_trans_commit(sc->tp);
	if (error)
		do_error(
_("Error %d while writing inobt btree for AG %u.\n"), error, agno);
	sc->tp = NULL;
}

/* Grab one finobt record. */
static int
get_finobt_record(
	struct xfs_btree_cur		*cur,
	void				*priv)
{
	struct bt_rebuild		*rebuild = priv;

	get_inode_data(cur, rebuild->ino_rec, NULL);
	rebuild->ino_rec = next_free_ino_rec(rebuild->ino_rec);
	return 0;
}

/* Rebuild a finobt btree. */
static void
build_finobt(
	struct repair_ctx	*sc,
	xfs_agnumber_t		agno,
	struct bt_rebuild	*btr_fino)
{
	int			error;

	btr_fino->bload.get_record = get_finobt_record;
	btr_fino->bload.claim_block = rebuild_claim_block;
	btr_fino->ino_rec = findfirst_free_inode_rec(agno);

	error = -libxfs_trans_alloc_empty(sc->mp, &sc->tp);
	if (error)
		do_error(
_("Insufficient memory to construct finobt rebuild transaction.\n"));

	/* Add all observed finobt records. */
	error = -libxfs_btree_bload(btr_fino->cur, &btr_fino->bload, btr_fino);
	if (error)
		do_error(
_("Error %d while creating finobt btree for AG %u.\n"), error, agno);

	/* Since we're not writing the AGI yet, no need to commit the cursor */
	libxfs_btree_del_cursor(btr_fino->cur, 0);
	error = -libxfs_trans_commit(sc->tp);
	if (error)
		do_error(
_("Error %d while writing finobt btree for AG %u.\n"), error, agno);
	sc->tp = NULL;
}

/*
 * XXX: yet more code that can be shared with mkfs, growfs.
 */
static void
build_agi(
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno,
	struct bt_rebuild	*btr_ino,
	struct bt_rebuild	*btr_fino,
	struct agi_stat		*agi_stat)
{
	struct xfs_buf		*agi_buf;
	struct xfs_agi		*agi;
	int			i;
	int			error;

	error = -libxfs_buf_get(mp->m_dev,
			XFS_AG_DADDR(mp, agno, XFS_AGI_DADDR(mp)),
			mp->m_sb.sb_sectsize / BBSIZE, &agi_buf);
	if (error)
		do_error(_("Cannot grab AG %u AGI buffer, err=%d"),
				agno, error);
	agi_buf->b_ops = &xfs_agi_buf_ops;
	agi = agi_buf->b_addr;
	memset(agi, 0, mp->m_sb.sb_sectsize);

	agi->agi_magicnum = cpu_to_be32(XFS_AGI_MAGIC);
	agi->agi_versionnum = cpu_to_be32(XFS_AGI_VERSION);
	agi->agi_seqno = cpu_to_be32(agno);
	if (agno < mp->m_sb.sb_agcount - 1)
		agi->agi_length = cpu_to_be32(mp->m_sb.sb_agblocks);
	else
		agi->agi_length = cpu_to_be32(mp->m_sb.sb_dblocks -
			(xfs_rfsblock_t) mp->m_sb.sb_agblocks * agno);
	agi->agi_count = cpu_to_be32(agi_stat->count);
	agi->agi_root = cpu_to_be32(btr_ino->newbt.afake.af_root);
	agi->agi_level = cpu_to_be32(btr_ino->newbt.afake.af_levels);
	agi->agi_freecount = cpu_to_be32(agi_stat->freecount);
	agi->agi_newino = cpu_to_be32(agi_stat->first_agino);
	agi->agi_dirino = cpu_to_be32(NULLAGINO);

	for (i = 0; i < XFS_AGI_UNLINKED_BUCKETS; i++)
		agi->agi_unlinked[i] = cpu_to_be32(NULLAGINO);

	if (xfs_sb_version_hascrc(&mp->m_sb))
		platform_uuid_copy(&agi->agi_uuid, &mp->m_sb.sb_meta_uuid);

	if (xfs_sb_version_hasfinobt(&mp->m_sb)) {
		agi->agi_free_root =
				cpu_to_be32(btr_fino->newbt.afake.af_root);
		agi->agi_free_level =
				cpu_to_be32(btr_fino->newbt.afake.af_levels);
	}

	if (xfs_sb_version_hasinobtcounts(&mp->m_sb)) {
		agi->agi_iblocks = cpu_to_be32(btr_ino->newbt.afake.af_blocks);
		agi->agi_fblocks = cpu_to_be32(btr_fino->newbt.afake.af_blocks);
	}

	libxfs_buf_mark_dirty(agi_buf);
	libxfs_buf_relse(agi_buf);
}

/* rebuild the rmap tree */

/* Set up the rmap rebuild parameters. */
static void
init_rmapbt_cursor(
	struct repair_ctx	*sc,
	xfs_agnumber_t		agno,
	unsigned int		free_space,
	struct bt_rebuild	*btr)
{
	int			error;

	init_rebuild(sc, &XFS_RMAP_OINFO_AG, free_space, btr);
	btr->cur = libxfs_rmapbt_stage_cursor(sc->mp, &btr->newbt.afake, agno);

	/* Compute how many blocks we'll need. */
	error = -libxfs_btree_bload_compute_geometry(btr->cur, &btr->bload,
			rmap_record_count(sc->mp, agno));
	if (error)
		do_error(
_("Unable to compute rmap btree geometry, error %d.\n"), error);

	setup_rebuild(sc->mp, agno, btr, btr->bload.nr_blocks);
}

/* Grab one rmap record. */
static int
get_rmapbt_record(
	struct xfs_btree_cur		*cur,
	void				*priv)
{
	struct xfs_rmap_irec		*rec;
	struct bt_rebuild		*btr = priv;

	rec = pop_slab_cursor(btr->slab_cursor);
	memcpy(&cur->bc_rec.r, rec, sizeof(struct xfs_rmap_irec));
	return 0;
}

/* Rebuild a rmap btree. */
static void
build_rmap_tree(
	struct repair_ctx	*sc,
	xfs_agnumber_t		agno,
	struct bt_rebuild	*btr)
{
	int			error;

	btr->bload.get_record = get_rmapbt_record;
	btr->bload.claim_block = rebuild_claim_block;

	error = -libxfs_trans_alloc_empty(sc->mp, &sc->tp);
	if (error)
		do_error(
_("Insufficient memory to construct rmap rebuild transaction.\n"));

	error = rmap_init_cursor(agno, &btr->slab_cursor);
	if (error)
		do_error(
_("Insufficient memory to construct rmap cursor.\n"));

	/* Add all observed rmap records. */
	error = -libxfs_btree_bload(btr->cur, &btr->bload, btr);
	if (error)
		do_error(
_("Error %d while creating rmap btree for AG %u.\n"), error, agno);

	/* Since we're not writing the AGF yet, no need to commit the cursor */
	libxfs_btree_del_cursor(btr->cur, 0);
	free_slab_cursor(&btr->slab_cursor);
	error = -libxfs_trans_commit(sc->tp);
	if (error)
		do_error(
_("Error %d while writing rmap btree for AG %u.\n"), error, agno);
	sc->tp = NULL;
}

/* rebuild the refcount tree */

/* Set up the refcount rebuild parameters. */
static void
init_refc_cursor(
	struct repair_ctx	*sc,
	xfs_agnumber_t		agno,
	unsigned int		free_space,
	struct bt_rebuild	*btr)
{
	int			error;

	init_rebuild(sc, &XFS_RMAP_OINFO_REFC, free_space, btr);
	btr->cur = libxfs_refcountbt_stage_cursor(sc->mp, &btr->newbt.afake,
			agno);

	/* Compute how many blocks we'll need. */
	error = -libxfs_btree_bload_compute_geometry(btr->cur, &btr->bload,
			refcount_record_count(sc->mp, agno));
	if (error)
		do_error(
_("Unable to compute refcount btree geometry, error %d.\n"), error);

	setup_rebuild(sc->mp, agno, btr, btr->bload.nr_blocks);
}

/* Grab one refcount record. */
static int
get_refcountbt_record(
	struct xfs_btree_cur		*cur,
	void				*priv)
{
	struct xfs_refcount_irec	*rec;
	struct bt_rebuild		*btr = priv;

	rec = pop_slab_cursor(btr->slab_cursor);
	memcpy(&cur->bc_rec.rc, rec, sizeof(struct xfs_refcount_irec));
	return 0;
}

/* Rebuild a refcount btree. */
static void
build_refcount_tree(
	struct repair_ctx	*sc,
	xfs_agnumber_t		agno,
	struct bt_rebuild	*btr)
{
	int			error;

	btr->bload.get_record = get_refcountbt_record;
	btr->bload.claim_block = rebuild_claim_block;

	error = -libxfs_trans_alloc_empty(sc->mp, &sc->tp);
	if (error)
		do_error(
_("Insufficient memory to construct refcount rebuild transaction.\n"));

	error = init_refcount_cursor(agno, &btr->slab_cursor);
	if (error)
		do_error(
_("Insufficient memory to construct refcount cursor.\n"));

	/* Add all observed refcount records. */
	error = -libxfs_btree_bload(btr->cur, &btr->bload, btr);
	if (error)
		do_error(
_("Error %d while creating refcount btree for AG %u.\n"), error, agno);

	/* Since we're not writing the AGF yet, no need to commit the cursor */
	libxfs_btree_del_cursor(btr->cur, 0);
	free_slab_cursor(&btr->slab_cursor);
	error = -libxfs_trans_commit(sc->tp);
	if (error)
		do_error(
_("Error %d while writing refcount btree for AG %u.\n"), error, agno);
	sc->tp = NULL;
}

/* Fill the AGFL with any leftover bnobt rebuilder blocks. */
static void
fill_agfl(
	struct bt_rebuild	*btr,
	__be32			*agfl_bnos,
	int			*i)
{
	struct xrep_newbt_resv	*resv, *n;
	struct xfs_mount	*mp = btr->newbt.sc->mp;

	for_each_xrep_newbt_reservation(&btr->newbt, resv, n) {
		xfs_agblock_t	bno;

		bno = XFS_FSB_TO_AGBNO(mp, resv->fsbno + resv->used);
		while (resv->used < resv->len && (*i) < libxfs_agfl_size(mp)) {
			agfl_bnos[(*i)++] = cpu_to_be32(bno++);
			resv->used++;
		}
	}
}

/*
 * build both the agf and the agfl for an agno given both
 * btree cursors.
 *
 * XXX: yet more common code that can be shared with mkfs/growfs.
 */
static void
build_agf_agfl(
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno,
	struct bt_rebuild	*btr_bno,
	struct bt_rebuild	*btr_cnt,
	xfs_extlen_t		freeblks,	/* # free blocks in tree */
	int			lostblocks,	/* # blocks that will be lost */
	struct bt_rebuild	*btr_rmap,
	struct bt_rebuild	*btr_refc,
	struct xfs_slab		*lost_fsbs)
{
	struct extent_tree_node	*ext_ptr;
	struct xfs_buf		*agf_buf, *agfl_buf;
	int			i;
	struct xfs_agfl		*agfl;
	struct xfs_agf		*agf;
	__be32			*freelist;
	int			error;

	error = -libxfs_buf_get(mp->m_dev,
			XFS_AG_DADDR(mp, agno, XFS_AGF_DADDR(mp)),
			mp->m_sb.sb_sectsize / BBSIZE, &agf_buf);
	if (error)
		do_error(_("Cannot grab AG %u AGF buffer, err=%d"),
				agno, error);
	agf_buf->b_ops = &xfs_agf_buf_ops;
	agf = agf_buf->b_addr;
	memset(agf, 0, mp->m_sb.sb_sectsize);

#ifdef XR_BLD_FREE_TRACE
	fprintf(stderr, "agf = %p, agf_buf->b_addr = %p\n",
		agf, agf_buf->b_addr);
#endif

	/*
	 * set up fixed part of agf
	 */
	agf->agf_magicnum = cpu_to_be32(XFS_AGF_MAGIC);
	agf->agf_versionnum = cpu_to_be32(XFS_AGF_VERSION);
	agf->agf_seqno = cpu_to_be32(agno);

	if (agno < mp->m_sb.sb_agcount - 1)
		agf->agf_length = cpu_to_be32(mp->m_sb.sb_agblocks);
	else
		agf->agf_length = cpu_to_be32(mp->m_sb.sb_dblocks -
			(xfs_rfsblock_t) mp->m_sb.sb_agblocks * agno);

	agf->agf_roots[XFS_BTNUM_BNO] =
			cpu_to_be32(btr_bno->newbt.afake.af_root);
	agf->agf_levels[XFS_BTNUM_BNO] =
			cpu_to_be32(btr_bno->newbt.afake.af_levels);
	agf->agf_roots[XFS_BTNUM_CNT] =
			cpu_to_be32(btr_cnt->newbt.afake.af_root);
	agf->agf_levels[XFS_BTNUM_CNT] =
			cpu_to_be32(btr_cnt->newbt.afake.af_levels);
	agf->agf_freeblks = cpu_to_be32(freeblks);

	if (xfs_sb_version_hasrmapbt(&mp->m_sb)) {
		agf->agf_roots[XFS_BTNUM_RMAP] =
				cpu_to_be32(btr_rmap->newbt.afake.af_root);
		agf->agf_levels[XFS_BTNUM_RMAP] =
				cpu_to_be32(btr_rmap->newbt.afake.af_levels);
		agf->agf_rmap_blocks =
				cpu_to_be32(btr_rmap->newbt.afake.af_blocks);
	}

	if (xfs_sb_version_hasreflink(&mp->m_sb)) {
		agf->agf_refcount_root =
				cpu_to_be32(btr_refc->newbt.afake.af_root);
		agf->agf_refcount_level =
				cpu_to_be32(btr_refc->newbt.afake.af_levels);
		agf->agf_refcount_blocks =
				cpu_to_be32(btr_refc->newbt.afake.af_blocks);
	}

	/*
	 * Count and record the number of btree blocks consumed if required.
	 */
	if (xfs_sb_version_haslazysbcount(&mp->m_sb)) {
		unsigned int blks;
		/*
		 * Don't count the root blocks as they are already
		 * accounted for.
		 */
		blks = btr_bno->newbt.afake.af_blocks +
			btr_cnt->newbt.afake.af_blocks - 2;
		if (xfs_sb_version_hasrmapbt(&mp->m_sb))
			blks += btr_rmap->newbt.afake.af_blocks - 1;
		agf->agf_btreeblks = cpu_to_be32(blks);
#ifdef XR_BLD_FREE_TRACE
		fprintf(stderr, "agf->agf_btreeblks = %u\n",
				be32_to_cpu(agf->agf_btreeblks));
#endif
	}

#ifdef XR_BLD_FREE_TRACE
	fprintf(stderr, "bno root = %u, bcnt root = %u, indices = %u %u\n",
			be32_to_cpu(agf->agf_roots[XFS_BTNUM_BNO]),
			be32_to_cpu(agf->agf_roots[XFS_BTNUM_CNT]),
			XFS_BTNUM_BNO,
			XFS_BTNUM_CNT);
#endif

	if (xfs_sb_version_hascrc(&mp->m_sb))
		platform_uuid_copy(&agf->agf_uuid, &mp->m_sb.sb_meta_uuid);

	/* initialise the AGFL, then fill it if there are blocks left over. */
	error = -libxfs_buf_get(mp->m_dev,
			XFS_AG_DADDR(mp, agno, XFS_AGFL_DADDR(mp)),
			mp->m_sb.sb_sectsize / BBSIZE, &agfl_buf);
	if (error)
		do_error(_("Cannot grab AG %u AGFL buffer, err=%d"),
				agno, error);
	agfl_buf->b_ops = &xfs_agfl_buf_ops;
	agfl = XFS_BUF_TO_AGFL(agfl_buf);

	/* setting to 0xff results in initialisation to NULLAGBLOCK */
	memset(agfl, 0xff, mp->m_sb.sb_sectsize);
	freelist = xfs_buf_to_agfl_bno(agfl_buf);
	if (xfs_sb_version_hascrc(&mp->m_sb)) {
		agfl->agfl_magicnum = cpu_to_be32(XFS_AGFL_MAGIC);
		agfl->agfl_seqno = cpu_to_be32(agno);
		platform_uuid_copy(&agfl->agfl_uuid, &mp->m_sb.sb_meta_uuid);
		for (i = 0; i < libxfs_agfl_size(mp); i++)
			freelist[i] = cpu_to_be32(NULLAGBLOCK);
	}

	/* Fill the AGFL with leftover blocks or save them for later. */
	i = 0;
	freelist = xfs_buf_to_agfl_bno(agfl_buf);
	fill_agfl(btr_bno, freelist, &i);
	fill_agfl(btr_cnt, freelist, &i);
	if (xfs_sb_version_hasrmapbt(&mp->m_sb))
		fill_agfl(btr_rmap, freelist, &i);

	/* Set the AGF counters for the AGFL. */
	if (i > 0) {
		agf->agf_flfirst = 0;
		agf->agf_fllast = cpu_to_be32(i - 1);
		agf->agf_flcount = cpu_to_be32(i);
		rmap_store_agflcount(mp, agno, i);

#ifdef XR_BLD_FREE_TRACE
		fprintf(stderr, "writing agfl for ag %u\n", agno);
#endif

	} else  {
		agf->agf_flfirst = 0;
		agf->agf_fllast = cpu_to_be32(libxfs_agfl_size(mp) - 1);
		agf->agf_flcount = 0;
	}

	libxfs_buf_mark_dirty(agfl_buf);
	libxfs_buf_relse(agfl_buf);

	ext_ptr = findbiggest_bcnt_extent(agno);
	agf->agf_longest = cpu_to_be32((ext_ptr != NULL) ?
						ext_ptr->ex_blockcount : 0);

	ASSERT(be32_to_cpu(agf->agf_roots[XFS_BTNUM_BNOi]) !=
		be32_to_cpu(agf->agf_roots[XFS_BTNUM_CNTi]));
	ASSERT(be32_to_cpu(agf->agf_refcount_root) !=
		be32_to_cpu(agf->agf_roots[XFS_BTNUM_BNOi]));
	ASSERT(be32_to_cpu(agf->agf_refcount_root) !=
		be32_to_cpu(agf->agf_roots[XFS_BTNUM_CNTi]));

	libxfs_buf_mark_dirty(agf_buf);
	libxfs_buf_relse(agf_buf);

	/*
	 * now fix up the free list appropriately
	 */
	fix_freelist(mp, agno, true);

#ifdef XR_BLD_FREE_TRACE
	fprintf(stderr, "wrote agf for ag %u\n", agno);
#endif
}

/*
 * update the superblock counters, sync the sb version numbers and
 * feature bits to the filesystem, and sync up the on-disk superblock
 * to match the incore superblock.
 */
static void
sync_sb(xfs_mount_t *mp)
{
	xfs_buf_t	*bp;

	bp = libxfs_getsb(mp);
	if (!bp)
		do_error(_("couldn't get superblock\n"));

	mp->m_sb.sb_icount = sb_icount;
	mp->m_sb.sb_ifree = sb_ifree;
	mp->m_sb.sb_fdblocks = sb_fdblocks;
	mp->m_sb.sb_frextents = sb_frextents;

	update_sb_version(mp);

	libxfs_sb_to_disk(bp->b_addr, &mp->m_sb);
	libxfs_buf_mark_dirty(bp);
	libxfs_buf_relse(bp);
}

/*
 * make sure the root and realtime inodes show up allocated
 * even if they've been freed.  they get reinitialized in phase6.
 */
static void
keep_fsinos(xfs_mount_t *mp)
{
	ino_tree_node_t		*irec;
	int			i;

	irec = find_inode_rec(mp, XFS_INO_TO_AGNO(mp, mp->m_sb.sb_rootino),
			XFS_INO_TO_AGINO(mp, mp->m_sb.sb_rootino));

	for (i = 0; i < 3; i++)
		set_inode_used(irec, i);
}

static void
phase5_func(
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno,
	struct xfs_slab		*lost_fsbs)
{
	struct repair_ctx	sc = { .mp = mp, };
	struct agi_stat		agi_stat = {0,};
	struct bt_rebuild	btr_bno;
	struct bt_rebuild	btr_cnt;
	struct bt_rebuild	btr_ino;
	struct bt_rebuild	btr_fino;
	struct bt_rebuild	btr_rmap;
	struct bt_rebuild	btr_refc;
	int			extra_blocks = 0;
	uint			num_freeblocks;
	xfs_extlen_t		freeblks1;
	xfs_extlen_t		freeblks2;
	xfs_agblock_t		num_extents;

	if (verbose)
		do_log(_("        - agno = %d\n"), agno);

	/*
	 * build up incore bno and bcnt extent btrees
	 */
	num_extents = mk_incore_fstree(mp, agno, &num_freeblocks);

#ifdef XR_BLD_FREE_TRACE
	fprintf(stderr, "# of bno extents is %d\n",
			count_bno_extents(agno));
#endif

	if (num_extents == 0)  {
		/*
		 * XXX - what we probably should do here is pick an
		 * inode for a regular file in the allocation group
		 * that has space allocated and shoot it by traversing
		 * the bmap list and putting all its extents on the
		 * incore freespace trees, clearing the inode,
		 * and clearing the in-use bit in the incore inode
		 * tree.  Then try mk_incore_fstree() again.
		 */
		do_error(_("unable to rebuild AG %u.  "
			  "Not enough free space in on-disk AG.\n"),
			agno);
	}

	init_ino_cursors(&sc, agno, num_freeblocks, &sb_icount_ag[agno],
			&sb_ifree_ag[agno], &btr_ino, &btr_fino);

	/*
	 * Set up the btree cursors for the on-disk rmap btrees, which includes
	 * pre-allocating all required blocks.  If rmap is disabled then the
	 * it's zeroed.
	 */
	if (xfs_sb_version_hasrmapbt(&mp->m_sb))
		init_rmapbt_cursor(&sc, agno, num_freeblocks, &btr_rmap);

	/*
	 * Set up the btree cursors for the on-disk refcount btrees,
	 * which includes pre-allocating all required blocks.
	 */
	if (xfs_sb_version_hasreflink(&mp->m_sb))
		init_refc_cursor(&sc, agno, num_freeblocks, &btr_refc);

	num_extents = count_bno_extents_blocks(agno, &num_freeblocks);
	/*
	 * lose two blocks per AG -- the space tree roots
	 * are counted as allocated since the space trees
	 * always have roots
	 */
	sb_fdblocks_ag[agno] += num_freeblocks - 2;

	if (num_extents == 0)  {
		/*
		 * XXX - what we probably should do here is pick an
		 * inode for a regular file in the allocation group
		 * that has space allocated and shoot it by traversing
		 * the bmap list and putting all its extents on the
		 * incore freespace trees, clearing the inode,
		 * and clearing the in-use bit in the incore inode
		 * tree.  Then try mk_incore_fstree() again.
		 */
		do_error(
		_("unable to rebuild AG %u.  No free space.\n"), agno);
	}

#ifdef XR_BLD_FREE_TRACE
	fprintf(stderr, "# of bno extents is %d\n", num_extents);
#endif

	/*
	 * track blocks that we might really lose
	 */
	init_freespace_cursors(&sc, agno, num_freeblocks, &num_extents,
			&extra_blocks, &btr_bno, &btr_cnt);

	/*
	 * freespace btrees live in the "free space" but
	 * the filesystem treats AGFL blocks as allocated
	 * since they aren't described by the freespace trees
	 */

	/*
	 * see if we can fit all the extra blocks into the AGFL
	 */
	extra_blocks = (extra_blocks - libxfs_agfl_size(mp) > 0)
			? extra_blocks - libxfs_agfl_size(mp)
			: 0;

	if (extra_blocks > 0)
		sb_fdblocks_ag[agno] -= extra_blocks;

#ifdef XR_BLD_FREE_TRACE
	fprintf(stderr, "# of bno extents is %d\n",
			count_bno_extents(agno));
	fprintf(stderr, "# of bcnt extents is %d\n",
			count_bcnt_extents(agno));
#endif

	/* Rebuild the freespace btrees. */
	build_bnobt(&sc, agno, &btr_bno, &freeblks1);
	build_cntbt(&sc, agno, &btr_cnt, &freeblks2);

#ifdef XR_BLD_FREE_TRACE
	fprintf(stderr, "# of free blocks == %d/%d\n", freeblks1, freeblks2);
#endif
	ASSERT(freeblks1 == freeblks2);

	if (xfs_sb_version_hasrmapbt(&mp->m_sb)) {
		build_rmap_tree(&sc, agno, &btr_rmap);
		sb_fdblocks_ag[agno] += btr_rmap.newbt.afake.af_blocks - 1;
	}

	if (xfs_sb_version_hasreflink(&mp->m_sb))
		build_refcount_tree(&sc, agno, &btr_refc);

	/*
	 * set up agf and agfl
	 */
	build_agf_agfl(mp, agno, &btr_bno, &btr_cnt, freeblks1, extra_blocks,
			&btr_rmap, &btr_refc, lost_fsbs);

	/*
	 * build inode allocation trees.
	 */
	build_inobt(&sc, agno, &btr_ino, &agi_stat);
	if (xfs_sb_version_hasfinobt(&mp->m_sb))
		build_finobt(&sc, agno, &btr_fino);

	/* build the agi */
	build_agi(mp, agno, &btr_ino, &btr_fino, &agi_stat);

	/*
	 * tear down cursors
	 */
	finish_rebuild(mp, &btr_bno, lost_fsbs);
	finish_rebuild(mp, &btr_cnt, lost_fsbs);
	finish_rebuild(mp, &btr_ino, lost_fsbs);
	if (xfs_sb_version_hasfinobt(&mp->m_sb))
		finish_rebuild(mp, &btr_fino, lost_fsbs);
	if (xfs_sb_version_hasrmapbt(&mp->m_sb))
		finish_rebuild(mp, &btr_rmap, lost_fsbs);
	if (xfs_sb_version_hasreflink(&mp->m_sb))
		finish_rebuild(mp, &btr_refc, lost_fsbs);

	/*
	 * release the incore per-AG bno/bcnt trees so
	 * the extent nodes can be recycled
	 */
	release_agbno_extent_tree(agno);
	release_agbcnt_extent_tree(agno);

	PROG_RPT_INC(prog_rpt_done[agno], 1);
}

/* Inject lost blocks back into the filesystem. */
static int
inject_lost_blocks(
	struct xfs_mount	*mp,
	struct xfs_slab		*lost_fsbs)
{
	struct xfs_trans	*tp = NULL;
	struct xfs_slab_cursor	*cur = NULL;
	struct lost_fsb		*lost;
	int			error;

	error = init_slab_cursor(lost_fsbs, NULL, &cur);
	if (error)
		return error;

	while ((lost = pop_slab_cursor(cur)) != NULL) {
		error = -libxfs_trans_alloc_rollable(mp, 16, &tp);
		if (error)
			goto out_cancel;

		error = -libxfs_free_extent(tp, lost->fsbno, lost->len,
				&XFS_RMAP_OINFO_ANY_OWNER, XFS_AG_RESV_NONE);
		if (error)
			goto out_cancel;

		error = -libxfs_trans_commit(tp);
		if (error)
			goto out_cancel;
		tp = NULL;
	}

out_cancel:
	if (tp)
		libxfs_trans_cancel(tp);
	free_slab_cursor(&cur);
	return error;
}

void
phase5(xfs_mount_t *mp)
{
	struct xfs_slab		*lost_fsbs;
	xfs_agnumber_t		agno;
	int			error;

	do_log(_("Phase 5 - rebuild AG headers and trees...\n"));
	set_progress_msg(PROG_FMT_REBUILD_AG, (uint64_t)glob_agcount);

#ifdef XR_BLD_FREE_TRACE
	fprintf(stderr, "inobt level 1, maxrec = %d, minrec = %d\n",
		libxfs_inobt_maxrecs(mp, mp->m_sb.sb_blocksize, 0),
		libxfs_inobt_maxrecs(mp, mp->m_sb.sb_blocksize, 0) / 2);
	fprintf(stderr, "inobt level 0 (leaf), maxrec = %d, minrec = %d\n",
		libxfs_inobt_maxrecs(mp, mp->m_sb.sb_blocksize, 1),
		libxfs_inobt_maxrecs(mp, mp->m_sb.sb_blocksize, 1) / 2);
	fprintf(stderr, "xr inobt level 0 (leaf), maxrec = %d\n",
		XR_INOBT_BLOCK_MAXRECS(mp, 0));
	fprintf(stderr, "xr inobt level 1 (int), maxrec = %d\n",
		XR_INOBT_BLOCK_MAXRECS(mp, 1));
	fprintf(stderr, "bnobt level 1, maxrec = %d, minrec = %d\n",
		libxfs_allocbt_maxrecs(mp, mp->m_sb.sb_blocksize, 0),
		libxfs_allocbt_maxrecs(mp, mp->m_sb.sb_blocksize, 0) / 2);
	fprintf(stderr, "bnobt level 0 (leaf), maxrec = %d, minrec = %d\n",
		libxfs_allocbt_maxrecs(mp, mp->m_sb.sb_blocksize, 1),
		libxfs_allocbt_maxrecs(mp, mp->m_sb.sb_blocksize, 1) / 2);
#endif
	/*
	 * make sure the root and realtime inodes show up allocated
	 */
	keep_fsinos(mp);

	/* allocate per ag counters */
	sb_icount_ag = calloc(mp->m_sb.sb_agcount, sizeof(uint64_t));
	if (sb_icount_ag == NULL)
		do_error(_("cannot alloc sb_icount_ag buffers\n"));

	sb_ifree_ag = calloc(mp->m_sb.sb_agcount, sizeof(uint64_t));
	if (sb_ifree_ag == NULL)
		do_error(_("cannot alloc sb_ifree_ag buffers\n"));

	sb_fdblocks_ag = calloc(mp->m_sb.sb_agcount, sizeof(uint64_t));
	if (sb_fdblocks_ag == NULL)
		do_error(_("cannot alloc sb_fdblocks_ag buffers\n"));

	error = init_slab(&lost_fsbs, sizeof(struct lost_fsb));
	if (error)
		do_error(_("cannot alloc lost block slab\n"));

	for (agno = 0; agno < mp->m_sb.sb_agcount; agno++)
		phase5_func(mp, agno, lost_fsbs);

	print_final_rpt();

	/* aggregate per ag counters */
	for (agno = 0; agno < mp->m_sb.sb_agcount; agno++)  {
		sb_icount += sb_icount_ag[agno];
		sb_ifree += sb_ifree_ag[agno];
		sb_fdblocks += sb_fdblocks_ag[agno];
	}
	free(sb_icount_ag);
	free(sb_ifree_ag);
	free(sb_fdblocks_ag);

	if (mp->m_sb.sb_rblocks)  {
		do_log(
		_("        - generate realtime summary info and bitmap...\n"));
		rtinit(mp);
		generate_rtinfo(mp, btmcompute, sumcompute);
	}

	do_log(_("        - reset superblock...\n"));

	/*
	 * sync superblock counter and set version bits correctly
	 */
	sync_sb(mp);

	/*
	 * Put the per-AG btree rmap data into the rmapbt now that we've reset
	 * the superblock counters.
	 */
	for (agno = 0; agno < mp->m_sb.sb_agcount; agno++) {
		error = rmap_store_ag_btree_rec(mp, agno);
		if (error)
			do_error(
_("unable to add AG %u reverse-mapping data to btree.\n"), agno);
	}

	/*
	 * Put blocks that were unnecessarily reserved for btree
	 * reconstruction back into the filesystem free space data.
	 */
	error = inject_lost_blocks(mp, lost_fsbs);
	if (error)
		do_error(_("Unable to reinsert lost blocks into filesystem.\n"));
	free_slab(&lost_fsbs);

	bad_ino_btree = 0;

}
