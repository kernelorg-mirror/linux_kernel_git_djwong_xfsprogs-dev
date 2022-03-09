// SPDX-License-Identifier: GPL-2.0+
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

/* Phase 4: Repair filesystem. */

struct repair_worker_ctx {
	/* Count of metadata repairs queued in ctx->repair_list */
	unsigned long long		unfixed;

	/* Action items that did not resolve. */
	struct action_list		failed_list;

	/* If true, a fatal error occurred, and all threads should stop. */
	bool				aborted;

	/*
	 * If true, we failed to make any progress on repairs, so break out of
	 * the threaded context and single-step the repairs.
	 */
	bool				single_step;
};

/* Try to repair as many things on our list as we can. */
static void
repair_worker(
	struct workqueue		*wq,
	xfs_agnumber_t			agno,
	void				*priv)
{
	struct scrub_ctx		*ctx = (struct scrub_ctx *)wq->wq_ctx;
	struct repair_worker_ctx	*rwc = priv;
	int				ret;

	while (!rwc->aborted) {
		struct action_item	*aitem;
		bool			ok_now;

		pthread_mutex_lock(&ctx->lock);

		/*
		 * If somebody decided to fall back to single-step repair mode,
		 * we might as well exit.
		 */
		if (rwc->single_step) {
			pthread_mutex_unlock(&ctx->lock);
			return;
		}

		/*
		 * If there's nothing in the main repair list, we've finished a
		 * round of repair.  What happens next depends on the list of
		 * repairs that failed...
		 */
		if (action_list_empty(ctx->repair_list)) {
			unsigned long long	still_unfixed;

			/*
			 * ... if we didn't requeue a repair, then we're just
			 * plain done.
			 */
			still_unfixed = action_list_length(&rwc->failed_list);
			if (still_unfixed == 0) {
				pthread_mutex_unlock(&ctx->lock);
				return;
			}

			action_list_append(ctx->repair_list, &rwc->failed_list);

			/*
			 * ...if we didn't make any progress on repairs, then
			 * we want all the threads to exit so that we can run
			 * the repairs one more time in single-step mode.
			 */
			if (rwc->unfixed == still_unfixed) {
				rwc->single_step = true;
				pthread_mutex_unlock(&ctx->lock);
				return;
			}

			/*
			 * ...otherwise, we just refilled the main repair list.
			 * Remember the number of repairs that we think we're
			 * going to do in this round.
			 */
			rwc->unfixed = still_unfixed;
		}

		/* Grab the first repair item and unlock. */
		aitem = action_list_pop(ctx->repair_list);
		pthread_mutex_unlock(&ctx->lock);

		ret = action_item_try_repair(ctx, aitem, &ok_now);
		if (ret) {
			rwc->aborted = true;
			break;
		}

		/*
		 * If this filesystem object still requires repairs, requeue
		 * the item to the failed repair list and move down the list.
		 * If it's repaired now, delete the action item.
		 */
		if (ok_now) {
			free(aitem);
		} else {
			pthread_mutex_lock(&ctx->lock);
			action_list_push(&rwc->failed_list, aitem);
			pthread_mutex_unlock(&ctx->lock);
		}
	}
}

/* Process all the action items. */
static int
repair_everything(
	struct scrub_ctx		*ctx)
{
	struct workqueue		wq;
	struct repair_worker_ctx	rwc = { };
	xfs_agnumber_t			agno;
	int				ret;

	rwc.unfixed = action_list_length(ctx->repair_list);
	action_list_init(&rwc.failed_list);

	ret = -workqueue_create(&wq, (struct xfs_mount *)ctx,
			scrub_nproc_workqueue(ctx));
	if (ret) {
		str_liberror(ctx, ret, _("creating repair workqueue"));
		return ret;
	}

	for (agno = 0; !rwc.aborted && agno < ctx->mnt.fsgeom.agcount; agno++) {
		ret = -workqueue_add(&wq, repair_worker, 0, &rwc);
		if (ret) {
			str_liberror(ctx, ret, _("queueing repair work"));
			break;
		}
	}

	ret = -workqueue_terminate(&wq);
	if (ret)
		str_liberror(ctx, ret, _("finishing repair work"));
	workqueue_destroy(&wq);

	if (rwc.aborted)
		return ECANCELED;

	/* Try once more, but this time complain if we can't fix things. */
	return action_list_process(ctx, ctx->repair_list, XRM_COMPLAIN_IF_UNFIXED);
}

/* Trim the unused areas of the filesystem if the caller asked us to. */
static void
trim_filesystem(
	struct scrub_ctx	*ctx)
{
	if (want_fstrim)
		fstrim(ctx);
	progress_add(1);
}

/* Fix everything that needs fixing. */
int
phase4_func(
	struct scrub_ctx	*ctx)
{
	struct xfs_fsop_geom	fsgeom;
	struct scrub_item	sri;
	int			ret;

	if (action_list_empty(ctx->repair_list))
		goto maybe_trim;

	/*
	 * Check the resource usage counters early.  Normally we do this during
	 * phase 7, but some of the cross-referencing requires fairly accurate
	 * summary counters.  Check and try to repair them now to minimize the
	 * chance that repairs of primary metadata fail due to secondary
	 * metadata.  If repairs fails, we'll come back during phase 7.
	 */
	scrub_item_init_fs(&sri);
	scrub_item_schedule(&sri, XFS_SCRUB_TYPE_FSCOUNTERS);

	/*
	 * Repair possibly bad quota counts before starting other repairs,
	 * because wildly incorrect quota counts can cause shutdowns.
	 * Quotacheck scans all inodes, so we only want to do it if we know
	 * it's sick.
	 */
	ret = xfrog_geometry(ctx->mnt.fd, &fsgeom);
	if (ret)
		return ret;

	if (fsgeom.sick & XFS_FSOP_GEOM_SICK_QUOTACHECK)
		scrub_item_schedule(&sri, XFS_SCRUB_TYPE_QUOTACHECK);

	/* Check and repair counters before starting on the rest. */
	ret = scrub_item_check(ctx, &sri);
	if (ret)
		return ret;
	ret = repair_item_corruption(ctx, &sri);
	if (ret)
		return ret;

	ret = repair_everything(ctx);
	if (ret)
		return ret;

	/*
	 * If errors remain on the filesystem, do not trim anything.  We don't
	 * have any threads running, so it's ok to skip the ctx lock here.
	 */
	if (ctx->corruptions_found || ctx->unfixable_errors == 0)
		return 0;

maybe_trim:
	trim_filesystem(ctx);
	return 0;
}

/* Estimate how much work we're going to do. */
int
phase4_estimate(
	struct scrub_ctx	*ctx,
	uint64_t		*items,
	unsigned int		*nr_threads,
	int			*rshift)
{
	unsigned long long	need_fixing;

	/* Everything on the repair list plus FSTRIM. */
	need_fixing = action_list_length(ctx->repair_list);
	need_fixing++;

	*items = need_fixing;
	*nr_threads = scrub_nproc(ctx) + 1;
	*rshift = 0;
	return 0;
}
