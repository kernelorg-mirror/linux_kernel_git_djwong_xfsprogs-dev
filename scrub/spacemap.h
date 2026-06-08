// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2018-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef XFS_SCRUB_SPACEMAP_H_
#define XFS_SCRUB_SPACEMAP_H_

/*
 * Visit each space mapping in the filesystem.  Return 0 to continue iteration
 * or a positive error code to stop iterating and return to the caller.
 */
typedef int (*scrub_fsmap_iter_fn)(struct scrub_ctx *ctx,
		struct fsmap *fsr, void *arg);

int scrub_iterate_fsmap(struct scrub_ctx *ctx, struct fsmap *keys,
		scrub_fsmap_iter_fn fn, void *arg);
int scrub_scan_all_spacemaps(struct scrub_ctx *ctx, scrub_fsmap_iter_fn fn,
		void *arg);

static inline unsigned int scrub_scan_spacemaps_nproc(struct scrub_ctx *ctx)
{
	return scrub_nproc(ctx);
}

/* Return XFS device index from fsmap device. */
static inline enum xfs_device
from_fsmap_dev(
	struct scrub_ctx	*ctx,
	dev_t			dev)
{
	if (ctx->mnt.fsgeom.rtstart) {
		if (dev < XFS_DEV_DATA || dev > XFS_DEV_RT)
			abort();
		return dev;
	}

	if (dev == ctx->fsinfo.fs_datadev)
		return XFS_DEV_DATA;
	if (dev == ctx->fsinfo.fs_logdev)
		return XFS_DEV_LOG;
	if (dev == ctx->fsinfo.fs_rtdev)
		return XFS_DEV_RT;
	abort();
}

/* Return fsmap device for XFS device index. */
static inline uint32_t
to_fsmap_dev(
	struct scrub_ctx	*ctx,
	enum xfs_device		dev)
{
	if (ctx->mnt.fsgeom.rtstart)
		return dev;

	switch (dev) {
	case XFS_DEV_DATA:
		return ctx->fsinfo.fs_datadev;
	case XFS_DEV_LOG:
		return ctx->fsinfo.fs_logdev;
	case XFS_DEV_RT:
		return ctx->fsinfo.fs_rtdev;
	default:
		abort();
	}
}

#endif /* XFS_SCRUB_SPACEMAP_H_ */
