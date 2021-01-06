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

struct repair_item {
	/*
	 * Information we need to call the repair ioctl.  Per-AG items should
	 * set the ino/gen fields to -1; per-inode items should set rpi_agno to
	 * -1; and per-fs items should set all three fields to -1.  Or use the
	 * macros below.
	 */
	__u64			rpi_ino;
	__u32			rpi_gen;
	__u32			rpi_agno;

	/*
	 * We save some of the XFS_SCRUB_OFLAG_* state from the scrub call.
	 * Specifically, we want to remember if the object was corrupt, if the
	 * cross-referencing revealed inconsistencies (xcorrupt), if the cross
	 * referencing itself failed (xfail) or if the object is correct but
	 * could be optimised (preen).  Each array element corresponds to one
	 * of the XFS_SCRUB_TYPE_* values.  We use a u8 here to save space.
	 */
	__u8			rpi_oflags[XFS_SCRUB_TYPE_NR];
};

static inline void
repair_item_init_ag(struct repair_item *rpi, xfs_agnumber_t agno)
{
	memset(rpi, 0, sizeof(*rpi));
	rpi->rpi_agno = agno;
	rpi->rpi_ino = -1ULL;
	rpi->rpi_gen = -1U;
}

static inline void
repair_item_init_fs(struct repair_item *rpi)
{
	memset(rpi, 0, sizeof(*rpi));
	rpi->rpi_agno = -1U;
	rpi->rpi_ino = -1ULL;
	rpi->rpi_gen = -1U;
}

static inline void
repair_item_init_file(struct repair_item *rpi, struct xfs_bulkstat *bstat)
{
	memset(rpi, 0, sizeof(*rpi));
	rpi->rpi_agno = -1U;
	rpi->rpi_ino = bstat->bs_ino;
	rpi->rpi_gen = bstat->bs_gen;
}

/* All of the OFLAGS that we need to prioritize repair work. */
#define REPAIR_CLASS_ANY (XFS_SCRUB_OFLAG_CORRUPT | \
			  XFS_SCRUB_OFLAG_PREEN | \
			  XFS_SCRUB_OFLAG_XFAIL | \
			  XFS_SCRUB_OFLAG_XCORRUPT)

static inline void
repair_item_save_state(
	struct repair_item		*rpi,
	const struct xfs_scrub_metadata	*meta)
{
	rpi->rpi_oflags[meta->sm_type] = meta->sm_flags & REPAIR_CLASS_ANY;
}

static inline void
repair_item_clean_state(
	struct repair_item		*rpi,
	const struct xfs_scrub_metadata	*meta)
{
	rpi->rpi_oflags[meta->sm_type] = 0;
}
bool repair_item_is_clean(const struct repair_item *rpi);

void scrub_report_preen_triggers(struct scrub_ctx *ctx);
int scrub_ag_headers(struct scrub_ctx *ctx, struct repair_item *rpi);
int scrub_ag_metadata(struct scrub_ctx *ctx, struct repair_item *rpi);
int scrub_fs_metadata(struct scrub_ctx *ctx, struct repair_item *rpi);
int scrub_summary(struct scrub_ctx *ctx, struct repair_item *rpi);
int scrub_meta_type(struct scrub_ctx *ctx, unsigned int type,
		struct repair_item *rpi);

bool can_scrub_fs_metadata(struct scrub_ctx *ctx);
bool can_scrub_inode(struct scrub_ctx *ctx);
bool can_scrub_bmap(struct scrub_ctx *ctx);
bool can_scrub_dir(struct scrub_ctx *ctx);
bool can_scrub_attr(struct scrub_ctx *ctx);
bool can_scrub_symlink(struct scrub_ctx *ctx);
bool can_scrub_parent(struct scrub_ctx *ctx);
bool xfs_can_repair(struct scrub_ctx *ctx);

int scrub_file(struct scrub_ctx *ctx, const struct xfs_bulkstat *bstat,
		unsigned int type, struct repair_item *rpi);

/*
 * Only ask the kernel to repair this object if the kernel directly told us it
 * was corrupt.  Objects that are only flagged as having cross-referencing
 * errors or flagged as eligible for optimization are left for later.
 */
#define XRM_REPAIR_ONLY		(1U << 0)

/* Complain if still broken even after fix. */
#define XRM_COMPLAIN_IF_UNFIXED	(1U << 1)

enum check_outcome xfs_repair_metadata(struct scrub_ctx *ctx, int fd,
		unsigned int scrub_type, struct repair_item *rpi,
		unsigned int repair_flags);

#endif /* XFS_SCRUB_SCRUB_H_ */
