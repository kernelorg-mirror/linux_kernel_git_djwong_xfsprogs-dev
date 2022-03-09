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
#include "libfrog/ptvar.h"
#include "xfs_scrub.h"
#include "common.h"
#include "counter.h"
#include "inodes.h"
#include "progress.h"
#include "scrub.h"
#include "repair.h"

/* Phase 3: Scan all inodes. */

struct scrub_inode_ctx {
	/* Number of inodes scanned. */
	struct ptcounter	*icount;

	/* Per-thread lists of file repair items. */
	struct ptvar		*repair_ptlists;

	/* Set to true to abort all threads. */
	bool			aborted;

	/* Set to true if we want to defer file repairs to phase 4. */
	bool			always_defer_repairs;
};

/* Report a filesystem error that the vfs fed us on close. */
static void
report_close_error(
	struct scrub_ctx	*ctx,
	struct xfs_bulkstat	*bstat)
{
	char			descr[DESCR_BUFSZ];
	int			old_errno = errno;

	scrub_render_ino_descr(ctx, descr, DESCR_BUFSZ, bstat->bs_ino,
			bstat->bs_gen, NULL);
	errno = old_errno;
	str_errno(ctx, descr);
}

/* Verify the contents, xattrs, and extent maps of an inode. */
static int
scrub_inode(
	struct scrub_ctx	*ctx,
	struct xfs_handle	*handle,
	struct xfs_bulkstat	*bstat,
	void			*arg)
{
	struct scrub_item	sri;
	struct scrub_inode_ctx	*ictx = arg;
	struct ptcounter	*icount = ictx->icount;
	int			fd = -1;
	int			error;

	scrub_item_init_file(&sri, bstat);
	background_sleep();

	/*
	 * Open this regular file to pin it in memory.  Avoiding the use of
	 * scan-by-handle means that the in-kernel scrubber doesn't pay the
	 * cost of opening the handle (looking up the inode in the inode btree,
	 * grabbing the inode, checking the generation) with every scrub call.
	 *
	 * Ignore any runtime or corruption related errors here because we can
	 * fall back to scrubbing by handle.  ESTALE can be ignored for the
	 * following reasons:
	 *
	 *  - If the file has been deleted since bulkstat, there's nothing to
	 *    check.  Scrub-by-handle returns ENOENT for such inodes.
	 *  - If the file has been deleted and reallocated since bulkstat,
	 *    its ondisk metadata have been rewritten and is assumed to be ok.
	 *    Scrub-by-handle also returns ENOENT if the generation doesn't
	 *    match.
	 *  - The file itself is corrupt and cannot be loaded.  In this case,
	 *    we fall back to scrub-by-handle.
	 *  - Reconnection of the file/directory parent failed due to
	 *    corruption in a parent directory.  In that case, we still want to
	 *    check /this/ file.
	 */
	if (S_ISREG(bstat->bs_mode) || S_ISDIR(bstat->bs_mode))
		fd = scrub_open_handle(handle);

	/* Scrub the inode. */
	scrub_item_schedule(&sri, XFS_SCRUB_TYPE_INODE);
	error = scrub_item_check_file(ctx, &sri, fd);
	if (error)
		goto out;

	error = repair_file_corruption(ctx, &sri, fd);
	if (error)
		goto out;

	/* Scrub all block mappings. */
	scrub_item_schedule(&sri, XFS_SCRUB_TYPE_BMBTD);
	scrub_item_schedule(&sri, XFS_SCRUB_TYPE_BMBTA);
	scrub_item_schedule(&sri, XFS_SCRUB_TYPE_BMBTC);
	error = scrub_item_check_file(ctx, &sri, fd);
	if (error)
		goto out;

	error = repair_file_corruption(ctx, &sri, fd);
	if (error)
		goto out;

	/* Check everything accessible via file mapping. */
	if (S_ISLNK(bstat->bs_mode))
		scrub_item_schedule(&sri, XFS_SCRUB_TYPE_SYMLINK);
	else if (S_ISDIR(bstat->bs_mode))
		scrub_item_schedule(&sri, XFS_SCRUB_TYPE_DIR);

	scrub_item_schedule(&sri, XFS_SCRUB_TYPE_XATTR);
	scrub_item_schedule(&sri, XFS_SCRUB_TYPE_PARENT);
	error = scrub_item_check_file(ctx, &sri, fd);
	if (error)
		goto out;

	/*
	 * Try to repair the file while it's open.  If at the start of phase 3
	 * we already had ag/rt metadata repairs queued up for phase 4, defer
	 * file repairs until phase 4 as well.
	 */
	if (!ictx->always_defer_repairs) {
		error = repair_file_corruption(ctx, &sri, fd);
		if (error)
			goto out;
	}

out:
	if (error)
		ictx->aborted = true;

	error = ptcounter_add(icount, 1);
	if (error) {
		str_liberror(ctx, error,
				_("incrementing scanned inode counter"));
		ictx->aborted = true;
	}
	progress_add(1);

	/* If this file requires more work, add it to the per-thread list. */
	if (repair_item_count_needsrepair(&sri) > 0) {
		struct action_list	*alist;

		alist = ptvar_get(ictx->repair_ptlists, &error);
		if (error) {
			str_liberror(ctx, error,
	_("getting per-thread inode repair list"));
			goto out_fd;
		}

		error = action_list_defer(ctx, alist, &sri);
		if (error)
			goto out_fd;
	}

out_fd:
	if (fd >= 0) {
		int	err2;

		err2 = close(fd);
		if (err2) {
			report_close_error(ctx, bstat);
			ictx->aborted = true;
		}
	}

	if (!error && ictx->aborted)
		error = ECANCELED;
	return error;
}

/* Collect all the inode repairs in the main repairs list. */
static int
collect_repairs(
	struct ptvar		*ptv,
	void			*data,
	void			*foreach_arg)
{
	struct scrub_ctx	*ctx = foreach_arg;
	struct action_list	*alist = data;

	action_list_append(ctx->repair_list, alist);
	return 0;
}

/* Initialize this per-thread file repair item list. */
static void
action_ptlist_init(
	void			*priv)
{
	struct action_list	*alist = priv;

	action_list_init(alist);
}

/* Verify all the inodes in a filesystem. */
int
phase3_func(
	struct scrub_ctx	*ctx)
{
	struct scrub_inode_ctx	ictx = { NULL };
	uint64_t		val;
	xfs_agnumber_t		agno;
	int			err;

	err = -ptvar_alloc(scrub_nproc(ctx), sizeof(struct action_list),
			action_ptlist_init, &ictx.repair_ptlists);
	if (err) {
		str_liberror(ctx, err,
	_("creating per-thread file repair item lists"));
		return err;
	}

	err = ptcounter_alloc(scrub_nproc(ctx), &ictx.icount);
	if (err) {
		str_liberror(ctx, err, _("creating scanned inode counter"));
		goto out_ptvar;
	}

	/*
	 * If we already have ag/fs metadata to repair from previous phases,
	 * we would rather not try to repair file metadata until we've tried
	 * to repair the space metadata.
	 */
	for (agno = 0; agno < ctx->mnt.fsgeom.agcount; agno++) {
		if (!action_list_empty(ctx->repair_list))
			ictx.always_defer_repairs = true;
	}

	err = scrub_scan_all_inodes(ctx, scrub_inode, &ictx);
	if (!err && ictx.aborted)
		err = ECANCELED;
	if (err)
		goto out_ptcounter;

	/*
	 * Combine all of the file repair items into the main repair list.
	 * We don't need locks here since we're the only thread running now.
	 */
	err = -ptvar_foreach(ictx.repair_ptlists, collect_repairs, ctx);
	if (err) {
		str_liberror(ctx, err, _("collecting inode repair lists"));
		goto out_ptcounter;
	}

	scrub_report_preen_triggers(ctx);
	err = ptcounter_value(ictx.icount, &val);
	if (err) {
		str_liberror(ctx, err, _("summing scanned inode counter"));
		goto out_ptcounter;
	}

	ctx->inodes_checked = val;
out_ptcounter:
	ptcounter_free(ictx.icount);
out_ptvar:
	ptvar_free(ictx.repair_ptlists);
	return err;
}

/* Estimate how much work we're going to do. */
int
phase3_estimate(
	struct scrub_ctx	*ctx,
	uint64_t		*items,
	unsigned int		*nr_threads,
	int			*rshift)
{
	*items = ctx->mnt_sv.f_files - ctx->mnt_sv.f_ffree;
	*nr_threads = scrub_nproc(ctx);
	*rshift = 0;
	return 0;
}
