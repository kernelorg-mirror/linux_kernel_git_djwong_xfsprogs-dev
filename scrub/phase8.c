// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "xfs.h"
#include <stdint.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/statvfs.h>
#include "list.h"
#include "libfrog/paths.h"
#include "libfrog/workqueue.h"
#include "xfs_scrub.h"
#include "common.h"
#include "progress.h"
#include "scrub.h"
#include "repair.h"
#include "vfs.h"
#include "atomic.h"
#include "disk.h"

/* Phase 8: Trim filesystem. */

static inline bool
fstrim_ok(
	struct scrub_ctx	*ctx)
{
	/*
	 * If errors remain on the filesystem, do not trim anything.  We don't
	 * have any threads running, so it's ok to skip the ctx lock here.
	 */
	if (!action_list_empty(ctx->fs_repair_list))
		return false;
	if (!action_list_empty(ctx->file_repair_list))
		return false;

	if (ctx->corruptions_found != 0)
		return false;
	if (ctx->unfixable_errors != 0)
		return false;

	if (ctx->runtime_errors != 0)
		return false;

	return true;
}

struct trim_ctl {
	uint64_t	datadev_end_pos;
	uint64_t	rtdev_end_pos;
	bool		aborted;
};

/* Trim each AG. */
static void
trim_ag(
	struct workqueue	*wq,
	xfs_agnumber_t		agno,
	void			*arg)
{
	struct scrub_ctx	*ctx = (struct scrub_ctx *)wq->wq_ctx;
	struct trim_ctl		*tctl = arg;
	uint64_t		pos, len, eoag_pos;
	int			error;

	pos = cvt_agbno_to_b(&ctx->mnt, agno, 0);
	eoag_pos = cvt_agbno_to_b(&ctx->mnt, agno, ctx->mnt.fsgeom.agblocks);
	len = min(tctl->datadev_end_pos, eoag_pos) - pos;

	error = fstrim(ctx, pos, len);
	if (error) {
		char		descr[DESCR_BUFSZ];

		snprintf(descr, sizeof(descr) - 1, _("fstrim agno %u"), agno);
		str_liberror(ctx, error, descr);
		tctl->aborted = true;
		return;
	}

	progress_add(1);
}

/* Trim each rt group. */
static void
trim_rtgroup(
	struct workqueue	*wq,
	xfs_agnumber_t		rgno,
	void			*arg)
{
	struct scrub_ctx	*ctx = (struct scrub_ctx *)wq->wq_ctx;
	struct trim_ctl		*tctl = arg;
	uint64_t		pos, len, eortg_pos;
	int			error;

	pos = cvt_rgbno_to_b(&ctx->mnt, rgno, 0);
	eortg_pos = cvt_rgbno_to_b(&ctx->mnt, rgno, ctx->mnt.fsgeom.rgblocks);
	len = min(tctl->rtdev_end_pos, eortg_pos) - pos;

	error = fstrim(ctx, pos + tctl->datadev_end_pos, len);
	if (error) {
		char		descr[DESCR_BUFSZ];

		snprintf(descr, sizeof(descr) - 1, _("fstrim rgno %u"), rgno);
		str_liberror(ctx, error, descr);
		tctl->aborted = true;
		return;
	}

	progress_add(1);
}

/* Trim the filesystem, if desired. */
int
phase8_func(
	struct scrub_ctx	*ctx)
{
	struct workqueue	wq;
	struct trim_ctl		tctl = {
		.aborted	= false,
	};
	xfs_agnumber_t		agno;
	int			error, err2;

	if (!fstrim_ok(ctx))
		return 0;

	tctl.datadev_end_pos = cvt_off_fsb_to_b(&ctx->mnt,
			ctx->mnt.fsgeom.datablocks);
	tctl.rtdev_end_pos = cvt_off_fsb_to_b(&ctx->mnt,
			ctx->mnt.fsgeom.rtblocks);

	error = -workqueue_create(&wq, (struct xfs_mount *)ctx,
			disk_heads(ctx->datadev));
	if (error) {
		str_liberror(ctx, error, _("creating fstrim workqueue"));
		return error;
	}

	/* Trim each AG in parallel. */
	for (agno = 0;
	     agno < ctx->mnt.fsgeom.agcount && !tctl.aborted;
	     agno++) {
		error = -workqueue_add(&wq, trim_ag, agno, &tctl);
		if (error) {
			str_liberror(ctx, error,
					_("queueing per-AG fstrim work"));
			goto out_wq;
		}
	}

	/* Trim each rtgroup in parallel. */
	for (agno = 0;
	     agno < ctx->mnt.fsgeom.rgcount && !tctl.aborted;
	     agno++) {
		error = -workqueue_add(&wq, trim_rtgroup, agno, &tctl);
		if (error) {
			str_liberror(ctx, error,
					_("queueing per-rtgroup fstrim work"));
			goto out_wq;
		}
	}

out_wq:
	err2 = -workqueue_terminate(&wq);
	if (err2) {
		str_liberror(ctx, err2, _("finishing fstrim work"));
		if (!error && err2)
			error = err2;
	}
	workqueue_destroy(&wq);

	if (!error && tctl.aborted)
		return ECANCELED;
	return error;
}

/* Estimate how much work we're going to do. */
int
phase8_estimate(
	struct scrub_ctx	*ctx,
	uint64_t		*items,
	unsigned int		*nr_threads,
	int			*rshift)
{
	*items = 0;

	if (fstrim_ok(ctx))
		*items = ctx->mnt.fsgeom.agcount + ctx->mnt.fsgeom.rgcount;

	*nr_threads = disk_heads(ctx->datadev);
	*rshift = 0;
	return 0;
}
