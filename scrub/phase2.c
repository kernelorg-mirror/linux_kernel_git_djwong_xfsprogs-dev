// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "xfs.h"
#include <stdint.h>
#include <sys/types.h>
#include <sys/statvfs.h>
#include "list.h"
#include "libfrog/paths.h"
#include "libfrog/workqueue.h"
#include "libfrog/fsgeom.h"
#include "libfrog/scrub.h"
#include "xfs_scrub.h"
#include "common.h"
#include "scrub.h"
#include "repair.h"

/* Phase 2: Check internal metadata. */

/* Scrub each AG's metadata btrees. */
static void
scan_ag_metadata(
	struct workqueue		*wq,
	xfs_agnumber_t			agno,
	void				*arg)
{
	struct scrub_item		sri;
	struct scrub_item		fix_now;
	struct scrub_ctx		*ctx = (struct scrub_ctx *)wq->wq_ctx;
	bool				*aborted = arg;
	unsigned long long		broken_primaries;
	unsigned long long		broken_secondaries;
	char				descr[DESCR_BUFSZ];
	unsigned int			to_check;
	int				ret;

	if (*aborted)
		return;

	scrub_item_init_ag(&sri, agno);
	snprintf(descr, DESCR_BUFSZ, _("AG %u"), agno);

	/*
	 * First we scrub and fix the AG headers, because we need them to work
	 * well enough to check the AG btrees.  Then scrub the AG btrees.
	 */
	scrub_item_schedule_group(&sri, XFROG_SCRUB_GROUP_AGHEADER);
	scrub_item_schedule_group(&sri, XFROG_SCRUB_GROUP_PERAG);

	/*
	 * Try to check all of the metadata items that we just scheduled.  If
	 * we return with some types still needing a check, try repairing any
	 * damaged metadata that we've found so far, and try again.  Abort if
	 * we stop making forward progress.
	 */
	ret = scrub_item_check(ctx, &sri);
	if (ret)
		goto err;

	to_check = scrub_item_count_needscheck(&sri);
	while (to_check > 0) {
		unsigned int	nr;

		ret = repair_item_corruption(ctx, &sri);
		if (ret)
			goto err;

		ret = scrub_item_check(ctx, &sri);
		if (ret)
			goto err;

		nr = scrub_item_count_needscheck(&sri);
		if (nr == to_check) {
			str_corrupt(ctx, descr,
	_("Unable to make forward checking progress."));
			goto err;
		}
		to_check = nr;
	}

	/*
	 * Figure out if we need to perform early fixing.  The only
	 * reason we need to do this is if the inobt is broken, which
	 * prevents phase 3 (inode scan) from running.  We can rebuild
	 * the inobt from rmapbt data, but if the rmapbt is broken even
	 * at this early phase then we are sunk.
	 */
	repair_item_mustfix(&sri, &fix_now, &broken_primaries,
			&broken_secondaries);
	if (broken_secondaries && !debug_tweak_on("XFS_SCRUB_FORCE_REPAIR")) {
		if (broken_primaries)
			str_info(ctx, descr,
_("Corrupt primary and secondary block mapping metadata."));
		else
			str_info(ctx, descr,
_("Corrupt secondary block mapping metadata."));
		str_info(ctx, descr,
_("Filesystem might not be repairable."));
	}

	/* Repair (inode) btree damage. */
	ret = repair_item_corruption(ctx, &fix_now);
	if (ret)
		goto err;

	/* Everything else gets fixed during phase 4. */
	ret = repair_item_defer(ctx, &sri);
	if (ret)
		goto err;
	return;
err:
	*aborted = true;
}

/* Scrub whole-FS metadata btrees. */
static void
scan_fs_metadata(
	struct workqueue		*wq,
	xfs_agnumber_t			agno,
	void				*arg)
{
	struct scrub_item		sri;
	struct scrub_ctx		*ctx = (struct scrub_ctx *)wq->wq_ctx;
	bool				*aborted = arg;
	int				ret;

	if (*aborted)
		return;

	scrub_item_init_fs(&sri);
	scrub_item_schedule_group(&sri, XFROG_SCRUB_GROUP_FS);
	ret = scrub_item_check(ctx, &sri);
	if (ret) {
		*aborted = true;
		return;
	}

	ret = repair_item_defer(ctx, &sri);
	if (ret) {
		*aborted = true;
		return;
	}
}

/* Scan all filesystem metadata. */
int
phase2_func(
	struct scrub_ctx	*ctx)
{
	struct scrub_item	sri;
	struct workqueue	wq;
	xfs_agnumber_t		agno;
	bool			aborted = false;
	int			ret, ret2;

	ret = -workqueue_create(&wq, (struct xfs_mount *)ctx,
			scrub_nproc_workqueue(ctx));
	if (ret) {
		str_liberror(ctx, ret, _("creating scrub workqueue"));
		return ret;
	}

	/*
	 * Scrub primary superblock.  This will be useful if we ever need to
	 * hook a filesystem-wide pre-scrub activity (e.g. enable filesystem
	 * upgrades) off of the sb 0 scrubber (which currently does nothing).
	 * If errors occur, this function will log them and return nonzero.
	 */
	scrub_item_init_ag(&sri, 0);
	scrub_item_schedule(&sri, XFS_SCRUB_TYPE_SB);
	ret = scrub_item_check(ctx, &sri);
	if (ret)
		goto out;
	ret = repair_item_completely(ctx, &sri);
	if (ret)
		goto out;

	for (agno = 0; !aborted && agno < ctx->mnt.fsgeom.agcount; agno++) {
		ret = -workqueue_add(&wq, scan_ag_metadata, agno, &aborted);
		if (ret) {
			str_liberror(ctx, ret, _("queueing per-AG scrub work"));
			goto out;
		}
	}

	if (aborted)
		goto out;

	ret = -workqueue_add(&wq, scan_fs_metadata, 0, &aborted);
	if (ret) {
		str_liberror(ctx, ret, _("queueing per-FS scrub work"));
		goto out;
	}

out:
	ret2 = -workqueue_terminate(&wq);
	if (ret2) {
		str_liberror(ctx, ret2, _("finishing scrub work"));
		if (!ret && ret2)
			ret = ret2;
	}
	workqueue_destroy(&wq);

	if (!ret && aborted)
		ret = ECANCELED;
	return ret;
}

/* Estimate how much work we're going to do. */
int
phase2_estimate(
	struct scrub_ctx	*ctx,
	uint64_t		*items,
	unsigned int		*nr_threads,
	int			*rshift)
{
	*items = scrub_estimate_ag_work(ctx);
	*nr_threads = scrub_nproc(ctx);
	*rshift = 0;
	return 0;
}
