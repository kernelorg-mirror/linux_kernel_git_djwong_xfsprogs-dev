// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef XFS_SCRUB_SCRUB_H_
#define XFS_SCRUB_SCRUB_H_

/* Online scrub and repair. */
enum check_outcome {
	CHECK_DONE,	/* no further processing needed */
	CHECK_REPAIR,	/* schedule this for repairs */
	CHECK_ABORT,	/* end program */
	CHECK_RETRY,	/* repair failed, try again later */
	CHECK_TOOSLOW,	/* skipped because freezes are not allowed */
};

/*
 * This flag boosts the repair priority of a scrub item when a dependent scrub
 * item is scheduled for repair.  Use a separate flag to preserve the
 * corruption state that we got from the kernel.  Priority boost is cleared the
 * next time xfs_repair_metadata is called.
 */
#define SCRUB_ITEM_BOOST_REPAIR	(1 << 0)

/*
 * These flags record the metadata object state that the kernel returned.
 * We want to remember if the object was corrupt, if the cross-referencing
 * revealed inconsistencies (xcorrupt), if the cross referencing itself failed
 * (xfail) or if the object is correct but could be optimised (preen).
 */
#define SCRUB_ITEM_CORRUPT	(XFS_SCRUB_OFLAG_CORRUPT)	/* (1 << 1) */
#define SCRUB_ITEM_PREEN	(XFS_SCRUB_OFLAG_PREEN)		/* (1 << 2) */
#define SCRUB_ITEM_XFAIL	(XFS_SCRUB_OFLAG_XFAIL)		/* (1 << 3) */
#define SCRUB_ITEM_XCORRUPT	(XFS_SCRUB_OFLAG_XCORRUPT)	/* (1 << 4) */

/* All of the state flags that we need to prioritize repair work. */
#define SCRUB_ITEM_REPAIR_ANY	(SCRUB_ITEM_CORRUPT | \
				 SCRUB_ITEM_PREEN | \
				 SCRUB_ITEM_XFAIL | \
				 SCRUB_ITEM_XCORRUPT)

/* Cross-referencing failures only. */
#define SCRUB_ITEM_REPAIR_XREF	(SCRUB_ITEM_XFAIL | \
				 SCRUB_ITEM_XCORRUPT)

/*
 * Special "repair class" mask that will trigger a repair for any scrub type
 * that was directly observed to be corrupt; or was observed to have some sort
 * of cross-referencing issue and there's a higher level metadata also needing
 * repair.
 */
#define SCRUB_ITEM_REPAIR_CORRUPT (SCRUB_ITEM_CORRUPT | \
				   SCRUB_ITEM_BOOST_REPAIR)

/* The kernel reported that it found something bad when scrubbing. */
#define SCRUB_ITEM_KERNEL_CORRUPT (SCRUB_ITEM_CORRUPT | \
				   SCRUB_ITEM_XFAIL | \
				   SCRUB_ITEM_XCORRUPT)

struct scrub_item {
	/*
	 * Information we need to call the scrub and repair ioctls.  Per-AG
	 * items should set the ino/gen fields to -1; per-inode items should
	 * set sri_agno to -1; and per-fs items should set all three fields to
	 * -1.  Or use the macros below.
	 */
	__u64			sri_ino;
	__u32			sri_gen;
	__u32			sri_agno;

	/* Scrub item state flags, one for each XFS_SCRUB_TYPE. */
	__u8			sri_state[XFS_SCRUB_TYPE_NR];
};

#define foreach_scrub_type(loopvar) \
	for ((loopvar) = 0; (loopvar) < XFS_SCRUB_TYPE_NR; (loopvar)++)

static inline void
scrub_item_init_ag(struct scrub_item *sri, xfs_agnumber_t agno)
{
	memset(sri, 0, sizeof(*sri));
	sri->sri_agno = agno;
	sri->sri_ino = -1ULL;
	sri->sri_gen = -1U;
}

static inline void
scrub_item_init_fs(struct scrub_item *sri)
{
	memset(sri, 0, sizeof(*sri));
	sri->sri_agno = -1U;
	sri->sri_ino = -1ULL;
	sri->sri_gen = -1U;
}

static inline void
scrub_item_init_file(struct scrub_item *sri, struct xfs_bulkstat *bstat)
{
	memset(sri, 0, sizeof(*sri));
	sri->sri_agno = -1U;
	sri->sri_ino = bstat->bs_ino;
	sri->sri_gen = bstat->bs_gen;
}

static inline void
scrub_item_save_state(
	struct scrub_item		*sri,
	unsigned  int			scrub_type,
	unsigned  int			scrub_flags)
{
	sri->sri_state[scrub_type] = scrub_flags & SCRUB_ITEM_REPAIR_ANY;
}

static inline void
scrub_item_clean_state(
	struct scrub_item		*sri,
	unsigned  int			scrub_type)
{
	sri->sri_state[scrub_type] = 0;
}
bool scrub_item_is_clean(const struct scrub_item *sri);

void scrub_report_preen_triggers(struct scrub_ctx *ctx);
int scrub_ag_headers(struct scrub_ctx *ctx, struct scrub_item *sri);
int scrub_ag_metadata(struct scrub_ctx *ctx, struct scrub_item *sri);
int scrub_fs_metadata(struct scrub_ctx *ctx, struct scrub_item *sri);
int scrub_summary(struct scrub_ctx *ctx, struct scrub_item *sri);
int scrub_meta_type(struct scrub_ctx *ctx, unsigned int type,
		struct scrub_item *sri);

bool can_scrub_fs_metadata(struct scrub_ctx *ctx);
bool can_scrub_inode(struct scrub_ctx *ctx);
bool can_scrub_bmap(struct scrub_ctx *ctx);
bool can_scrub_dir(struct scrub_ctx *ctx);
bool can_scrub_attr(struct scrub_ctx *ctx);
bool can_scrub_symlink(struct scrub_ctx *ctx);
bool can_scrub_parent(struct scrub_ctx *ctx);
bool xfs_can_repair(struct scrub_ctx *ctx);

int scrub_file(struct scrub_ctx *ctx, const struct xfs_bulkstat *bstat,
		unsigned int type, struct scrub_item *sri);

/*
 * Only ask the kernel to repair this object if the kernel directly told us it
 * was corrupt.  Objects that are only flagged as having cross-referencing
 * errors or flagged as eligible for optimization are left for later.
 */
#define XRM_REPAIR_ONLY		(1U << 0)

/* Complain if still broken even after fix. */
#define XRM_COMPLAIN_IF_UNFIXED	(1U << 1)

/* Don't call progress_add after repairing an item. */
#define XRM_NOPROGRESS		(1U << 2)

enum check_outcome xfs_repair_metadata(struct scrub_ctx *ctx, int fd,
		unsigned int scrub_type, struct scrub_item *sri,
		unsigned int repair_flags);

#endif /* XFS_SCRUB_SCRUB_H_ */
