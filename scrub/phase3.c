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
#include "xfs_scrub.h"
#include "common.h"
#include "counter.h"
#include "inodes.h"
#include "progress.h"
#include "scrub.h"
#include "repair.h"

/* Phase 3: Scan all inodes. */

struct scrub_inode_ctx {
	struct ptcounter	*icount;
	bool			aborted;
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
	unsigned int		to_check;
	int			fd = -1;
	int			error;

	scrub_item_init_file(&sri, bstat);
	background_sleep();

	/* Try to open the inode to pin it. */
	if (S_ISREG(bstat->bs_mode)) {
		fd = scrub_open_handle(handle);
		/* Stale inode means we scan the whole cluster again. */
		if (fd < 0 && errno == ESTALE)
			return ESTALE;
	}

	/* Scrub the inode. */
	scrub_item_schedule(&sri, XFS_SCRUB_TYPE_INODE);

	/* Scrub all block mappings. */
	scrub_item_schedule(&sri, XFS_SCRUB_TYPE_BMBTD);
	scrub_item_schedule(&sri, XFS_SCRUB_TYPE_BMBTA);
	scrub_item_schedule(&sri, XFS_SCRUB_TYPE_BMBTC);

	/* Check everything accessible via file mapping. */
	if (S_ISLNK(bstat->bs_mode))
		scrub_item_schedule(&sri, XFS_SCRUB_TYPE_SYMLINK);
	else if (S_ISDIR(bstat->bs_mode))
		scrub_item_schedule(&sri, XFS_SCRUB_TYPE_DIR);

	scrub_item_schedule(&sri, XFS_SCRUB_TYPE_XATTR);
	scrub_item_schedule(&sri, XFS_SCRUB_TYPE_PARENT);

	/*
	 * Try to check all of the metadata items that we just scheduled.  If
	 * we return with some types still needing a check, try repairing any
	 * damaged metadata that we've found so far, and try again.  Abort if
	 * we stop making forward progress.
	 */
	error = scrub_item_check_file(ctx, &sri, fd);
	if (error)
		goto out;

	to_check = scrub_item_count_needscheck(&sri);
	while (to_check > 0) {
		unsigned int	nr;

		error = repair_item_corruption(ctx, &sri);
		if (error)
			goto out;

		error = scrub_item_check_file(ctx, &sri, fd);
		if (error)
			goto out;

		nr = scrub_item_count_needscheck(&sri);
		if (nr == to_check) {
			char	descr[DESCR_BUFSZ];

			scrub_render_ino_descr(ctx, descr, DESCR_BUFSZ,
					bstat->bs_ino, bstat->bs_gen, NULL);
			str_corrupt(ctx, descr,
	_("Unable to make forward checking progress."));
			ictx->aborted = true;
			goto out;
		}
		to_check = nr;
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
	error = repair_item_defer(ctx, &sri);
	if (error)
		return error;
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

/* Verify all the inodes in a filesystem. */
int
phase3_func(
	struct scrub_ctx	*ctx)
{
	struct scrub_inode_ctx	ictx = { NULL };
	uint64_t		val;
	unsigned int		scan_flags = 0;
	int			err;

	err = ptcounter_alloc(scrub_nproc(ctx), &ictx.icount);
	if (err) {
		str_liberror(ctx, err, _("creating scanned inode counter"));
		return err;
	}

	if (ctx->mnt.fsgeom.flags & XFS_FSOP_GEOM_FLAGS_METADIR)
		scan_flags |= SCRUB_SCAN_METADIR;

	err = scrub_scan_all_inodes(ctx, scrub_inode, scan_flags, &ictx);
	if (!err && ictx.aborted)
		err = ECANCELED;
	if (err)
		goto free;

	scrub_report_preen_triggers(ctx);
	err = ptcounter_value(ictx.icount, &val);
	if (err) {
		str_liberror(ctx, err, _("summing scanned inode counter"));
		return err;
	}

	ctx->inodes_checked = val;
free:
	ptcounter_free(ictx.icount);
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
