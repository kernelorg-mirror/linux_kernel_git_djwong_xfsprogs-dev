// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "xfs.h"
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/statvfs.h>
#include "list.h"
#include "libfrog/paths.h"
#include "libfrog/fsgeom.h"
#include "libfrog/scrub.h"
#include "xfs_scrub.h"
#include "common.h"
#include "progress.h"
#include "scrub.h"
#include "xfs_errortag.h"
#include "repair.h"
#include "descr.h"

/* Online scrub and repair wrappers. */

/* Format a scrub description. */
static int
format_scrub_descr(
	struct scrub_ctx		*ctx,
	char				*buf,
	size_t				buflen,
	void				*where)
{
	struct xfs_scrub_metadata	*meta = where;
	const struct xfrog_scrub_descr	*sc = &xfrog_scrubbers[meta->sm_type];

	switch (sc->type) {
	case XFROG_SCRUB_TYPE_AGHEADER:
	case XFROG_SCRUB_TYPE_PERAG:
		return snprintf(buf, buflen, _("AG %u %s"), meta->sm_agno,
				_(sc->descr));
		break;
	case XFROG_SCRUB_TYPE_INODE:
		return scrub_render_ino_descr(ctx, buf, buflen,
				meta->sm_ino, meta->sm_gen, "%s",
				_(sc->descr));
		break;
	case XFROG_SCRUB_TYPE_FS:
	case XFROG_SCRUB_TYPE_SUMMARY:
	case XFROG_SCRUB_TYPE_NONE:
		return snprintf(buf, buflen, _("%s"), _(sc->descr));
		break;
	}
	return -1;
}

/* Predicates for scrub flag state. */

static inline bool is_corrupt(struct xfs_scrub_metadata *sm)
{
	return sm->sm_flags & XFS_SCRUB_OFLAG_CORRUPT;
}

static inline bool is_unoptimized(struct xfs_scrub_metadata *sm)
{
	return sm->sm_flags & XFS_SCRUB_OFLAG_PREEN;
}

static inline bool xref_failed(struct xfs_scrub_metadata *sm)
{
	return sm->sm_flags & XFS_SCRUB_OFLAG_XFAIL;
}

static inline bool xref_disagrees(struct xfs_scrub_metadata *sm)
{
	return sm->sm_flags & XFS_SCRUB_OFLAG_XCORRUPT;
}

static inline bool is_incomplete(struct xfs_scrub_metadata *sm)
{
	return sm->sm_flags & XFS_SCRUB_OFLAG_INCOMPLETE;
}

static inline bool is_suspicious(struct xfs_scrub_metadata *sm)
{
	return sm->sm_flags & XFS_SCRUB_OFLAG_WARNING;
}

/* Should we fix it? */
static inline bool needs_repair(struct xfs_scrub_metadata *sm)
{
	return is_corrupt(sm) || xref_disagrees(sm);
}

/* Warn about strange circumstances after scrub. */
static inline void
scrub_warn_incomplete_scrub(
	struct scrub_ctx		*ctx,
	struct descr			*dsc,
	struct xfs_scrub_metadata	*meta)
{
	if (is_incomplete(meta))
		str_info(ctx, descr_render(dsc), _("Check incomplete."));

	if (is_suspicious(meta)) {
		if (debug)
			str_info(ctx, descr_render(dsc),
					_("Possibly suspect metadata."));
		else
			str_warn(ctx, descr_render(dsc),
					_("Possibly suspect metadata."));
	}

	if (xref_failed(meta))
		str_info(ctx, descr_render(dsc),
				_("Cross-referencing failed."));
}

/* Do a read-only check of some metadata. */
static enum check_outcome
xfs_check_metadata(
	struct scrub_ctx		*ctx,
	struct xfs_scrub_metadata	*meta,
	bool				is_inode)
{
	DEFINE_DESCR(dsc, ctx, format_scrub_descr);
	unsigned int			tries = 0;
	int				error;

	assert(!debug_tweak_on("XFS_SCRUB_NO_KERNEL"));
	assert(meta->sm_type < XFS_SCRUB_TYPE_NR);
	descr_set(&dsc, meta);

	dbg_printf("check %s flags %xh\n", descr_render(&dsc), meta->sm_flags);
retry:
	error = -xfrog_scrub_metadata(&ctx->mnt, meta);
	if (debug_tweak_on("XFS_SCRUB_FORCE_REPAIR") && !error)
		meta->sm_flags |= XFS_SCRUB_OFLAG_CORRUPT;
	switch (error) {
	case 0:
		/* No operational errors encountered. */
		break;
	case EUSERS:
		/* Operation skipped because we cannot freeze. */
		if (!(meta->sm_flags & XFS_SCRUB_IFLAG_FREEZE_OK) &&
		    ctx->freeze_ok) {
			meta->sm_flags |= XFS_SCRUB_IFLAG_FREEZE_OK;
			goto retry;
		}
		skip_slow_op(ctx, descr_render(&dsc),
_("Check skipped because we can't freeze the fs."));
		return CHECK_TOOSLOW;
	case ENOENT:
		/* Metadata not present, just skip it. */
		return CHECK_DONE;
	case ESHUTDOWN:
		/* FS already crashed, give up. */
		str_error(ctx, descr_render(&dsc),
_("Filesystem is shut down, aborting."));
		return CHECK_ABORT;
	case EIO:
	case ENOMEM:
		/* Abort on I/O errors or insufficient memory. */
		str_errno(ctx, descr_render(&dsc));
		return CHECK_ABORT;
	case EDEADLOCK:
	case EBUSY:
	case EFSBADCRC:
	case EFSCORRUPTED:
		/*
		 * The first two should never escape the kernel,
		 * and the other two should be reported via sm_flags.
		 */
		str_liberror(ctx, error, _("Kernel bug"));
		/* fall through */
	default:
		/* Operational error. */
		str_errno(ctx, descr_render(&dsc));
		return CHECK_DONE;
	}

	/*
	 * If the kernel says the test was incomplete or that there was
	 * a cross-referencing discrepancy but no obvious corruption,
	 * we'll try the scan again, just in case the fs was busy.
	 * Only retry so many times.
	 */
	if (tries < 10 && (is_incomplete(meta) ||
			   (xref_disagrees(meta) && !is_corrupt(meta)))) {
		tries++;
		goto retry;
	}

	/* Complain about incomplete or suspicious metadata. */
	scrub_warn_incomplete_scrub(ctx, &dsc, meta);

	/*
	 * If we need repairs or there were discrepancies, schedule a
	 * repair if desired, otherwise complain.
	 */
	if (is_corrupt(meta) || xref_disagrees(meta)) {
		if (ctx->mode < SCRUB_MODE_REPAIR) {
			str_corrupt(ctx, descr_render(&dsc),
_("Repairs are required."));
			return CHECK_DONE;
		}

		return CHECK_REPAIR;
	}

	/*
	 * If we could optimize, schedule a repair if desired,
	 * otherwise complain.
	 */
	if (is_unoptimized(meta)) {
		if (ctx->mode != SCRUB_MODE_REPAIR) {
			if (!is_inode) {
				/* AG or FS metadata, always warn. */
				str_info(ctx, descr_render(&dsc),
_("Optimization is possible."));
			} else if (!ctx->preen_triggers[meta->sm_type]) {
				/* File metadata, only warn once per type. */
				pthread_mutex_lock(&ctx->lock);
				if (!ctx->preen_triggers[meta->sm_type])
					ctx->preen_triggers[meta->sm_type] = true;
				pthread_mutex_unlock(&ctx->lock);
			}
			return CHECK_DONE;
		}

		return CHECK_REPAIR;
	}

	/* Everything is ok. */
	return CHECK_DONE;
}

/* Bulk-notify user about things that could be optimized. */
void
scrub_report_preen_triggers(
	struct scrub_ctx		*ctx)
{
	int				i;

	for (i = 0; i < XFS_SCRUB_TYPE_NR; i++) {
		pthread_mutex_lock(&ctx->lock);
		if (ctx->preen_triggers[i]) {
			ctx->preen_triggers[i] = false;
			pthread_mutex_unlock(&ctx->lock);
			str_info(ctx, ctx->mntpoint,
_("Optimizations of %s are possible."), _(xfrog_scrubbers[i].descr));
		} else {
			pthread_mutex_unlock(&ctx->lock);
		}
	}
}

/*
 * Scrub a single XFS_SCRUB_TYPE_*, saving corruption reports for later.
 * Do not call this function to repair file metadata.
 *
 * Returns 0 for success.  If errors occur, this function will log them and
 * return a positive error code.
 */
int
scrub_meta_type(
	struct scrub_ctx		*ctx,
	unsigned int			type,
	struct repair_item		*rpi)
{
	struct xfs_scrub_metadata	meta = {
		.sm_type		= type,
	};
	enum check_outcome		fix;

	background_sleep();

	switch (xfrog_scrubbers[type].type) {
	case XFROG_SCRUB_TYPE_AGHEADER:
	case XFROG_SCRUB_TYPE_PERAG:
		meta.sm_agno = rpi->rpi_agno;
		break;
	case XFROG_SCRUB_TYPE_FS:
	case XFROG_SCRUB_TYPE_SUMMARY:
	case XFROG_SCRUB_TYPE_NONE:
		break;
	default:
		assert(0);
		break;
	}

	/* Check the item. */
	fix = xfs_check_metadata(ctx, &meta, false);
	progress_add(1);

	switch (fix) {
	case CHECK_ABORT:
		return ECANCELED;
	case CHECK_REPAIR:
		repair_item_save_state(rpi, &meta);
		return 0;
	case CHECK_TOOSLOW:
	case CHECK_DONE:
		repair_item_clean_state(rpi, &meta);
		return 0;
	default:
		/* CHECK_RETRY should never happen. */
		abort();
	}
}

/*
 * Scrub all metadata types that are assigned to the given XFROG_SCRUB_TYPE_*,
 * saving corruption reports for later.  This should not be used for
 * XFROG_SCRUB_TYPE_INODE or for checking summary metadata.
 */
static bool
scrub_all_types(
	struct scrub_ctx		*ctx,
	enum xfrog_scrub_type		scrub_type,
	struct repair_item		*rpi)
{
	const struct xfrog_scrub_descr	*sc;
	unsigned int			type;

	sc = xfrog_scrubbers;
	for (type = 0; type < XFS_SCRUB_TYPE_NR; type++, sc++) {
		int			ret;

		if (sc->type != scrub_type)
			continue;

		ret = scrub_meta_type(ctx, type, rpi);
		if (ret)
			return ret;
	}

	return 0;
}

/* Scrub each AG's header blocks. */
int
scrub_ag_headers(
	struct scrub_ctx		*ctx,
	struct repair_item		*rpi)
{
	return scrub_all_types(ctx, XFROG_SCRUB_TYPE_AGHEADER, rpi);
}

/* Scrub each AG's metadata btrees. */
int
scrub_ag_metadata(
	struct scrub_ctx		*ctx,
	struct repair_item		*rpi)
{
	return scrub_all_types(ctx, XFROG_SCRUB_TYPE_PERAG, rpi);
}

/* Scrub whole-FS metadata btrees. */
int
scrub_fs_metadata(
	struct scrub_ctx		*ctx,
	struct repair_item		*rpi)
{
	return scrub_all_types(ctx, XFROG_SCRUB_TYPE_FS, rpi);
}

/* Scrub FS summary metadata. */
int
scrub_summary(
	struct scrub_ctx		*ctx,
	struct repair_item		*rpi)
{
	return scrub_all_types(ctx, XFROG_SCRUB_TYPE_SUMMARY, rpi);
}

/* How many items do we have to check? */
unsigned int
scrub_estimate_ag_work(
	struct scrub_ctx		*ctx)
{
	const struct xfrog_scrub_descr	*sc;
	int				type;
	unsigned int			estimate = 0;

	sc = xfrog_scrubbers;
	for (type = 0; type < XFS_SCRUB_TYPE_NR; type++, sc++) {
		switch (sc->type) {
		case XFROG_SCRUB_TYPE_AGHEADER:
		case XFROG_SCRUB_TYPE_PERAG:
			estimate += ctx->mnt.fsgeom.agcount;
			break;
		case XFROG_SCRUB_TYPE_FS:
		case XFROG_SCRUB_TYPE_SUMMARY:
			estimate++;
			break;
		default:
			break;
		}
	}
	return estimate;
}

/*
 * Scrub file metadata of some sort.  If errors occur, this function will log
 * them and return nonzero.
 */
int
scrub_file(
	struct scrub_ctx		*ctx,
	const struct xfs_bulkstat	*bstat,
	unsigned int			type,
	struct repair_item		*rpi)
{
	struct xfs_scrub_metadata	meta = {0};
	enum check_outcome		fix;

	assert(type < XFS_SCRUB_TYPE_NR);
	assert(xfrog_scrubbers[type].type == XFROG_SCRUB_TYPE_INODE);

	meta.sm_type = type;
	meta.sm_ino = bstat->bs_ino;
	meta.sm_gen = bstat->bs_gen;

	/* Scrub the piece of metadata. */
	fix = xfs_check_metadata(ctx, &meta, true);
	if (fix == CHECK_ABORT)
		return ECANCELED;
	if (fix == CHECK_DONE) {
		repair_item_clean_state(rpi, &meta);
		return 0;
	}

	repair_item_save_state(rpi, &meta);
	return 0;
}

/*
 * Test the availability of a kernel scrub command.  If errors occur (or the
 * scrub ioctl is rejected) the errors will be logged and this function will
 * return false.
 */
static bool
__scrub_test(
	struct scrub_ctx		*ctx,
	unsigned int			type,
	bool				repair)
{
	struct xfs_scrub_metadata	meta = {0};
	struct xfs_error_injection	inject;
	static bool			injected;
	int				error;

	if (debug_tweak_on("XFS_SCRUB_NO_KERNEL"))
		return false;
	if (debug_tweak_on("XFS_SCRUB_FORCE_REPAIR") && !injected) {
		inject.fd = ctx->mnt.fd;
		inject.errtag = XFS_ERRTAG_FORCE_SCRUB_REPAIR;
		error = ioctl(ctx->mnt.fd, XFS_IOC_ERROR_INJECTION, &inject);
		if (error == 0)
			injected = true;
	}

	meta.sm_type = type;
	if (repair)
		meta.sm_flags |= XFS_SCRUB_IFLAG_REPAIR;
	error = -xfrog_scrub_metadata(&ctx->mnt, &meta);
	switch (error) {
	case 0:
		return true;
	case EROFS:
		str_info(ctx, ctx->mntpoint,
_("Filesystem is mounted read-only; cannot proceed."));
		return false;
	case ENOTRECOVERABLE:
		str_info(ctx, ctx->mntpoint,
_("Filesystem is mounted norecovery; cannot proceed."));
		return false;
	case EOPNOTSUPP:
	case ENOTTY:
		if (debug || verbose)
			str_info(ctx, ctx->mntpoint,
_("Kernel %s %s facility not detected."),
					_(xfrog_scrubbers[type].descr),
					repair ? _("repair") : _("scrub"));
		return false;
	case ENOENT:
		/* Scrubber says not present on this fs; that's fine. */
		return true;
	default:
		str_info(ctx, ctx->mntpoint, "%s", strerror(errno));
		return true;
	}
}

bool
can_scrub_fs_metadata(
	struct scrub_ctx	*ctx)
{
	return __scrub_test(ctx, XFS_SCRUB_TYPE_PROBE, false);
}

bool
can_scrub_inode(
	struct scrub_ctx	*ctx)
{
	return __scrub_test(ctx, XFS_SCRUB_TYPE_INODE, false);
}

bool
can_scrub_bmap(
	struct scrub_ctx	*ctx)
{
	return __scrub_test(ctx, XFS_SCRUB_TYPE_BMBTD, false);
}

bool
can_scrub_dir(
	struct scrub_ctx	*ctx)
{
	return __scrub_test(ctx, XFS_SCRUB_TYPE_DIR, false);
}

bool
can_scrub_attr(
	struct scrub_ctx	*ctx)
{
	return __scrub_test(ctx, XFS_SCRUB_TYPE_XATTR, false);
}

bool
can_scrub_symlink(
	struct scrub_ctx	*ctx)
{
	return __scrub_test(ctx, XFS_SCRUB_TYPE_SYMLINK, false);
}

bool
can_scrub_parent(
	struct scrub_ctx	*ctx)
{
	return __scrub_test(ctx, XFS_SCRUB_TYPE_PARENT, false);
}

bool
xfs_can_repair(
	struct scrub_ctx	*ctx)
{
	return __scrub_test(ctx, XFS_SCRUB_TYPE_PROBE, true);
}

/* General repair routines. */

/*
 * Decide if this repair item should be run one at a time.  The only types
 * requiring serialization are the ones that need to freeze the filesystem
 * and the ones that have to use trylocking to avoid ABBA deadlocks.
 */
static inline bool
repair_needs_excl(const struct xfs_scrub_metadata *meta)
{
	switch (meta->sm_type) {
	case XFS_SCRUB_TYPE_PARENT:
	case XFS_SCRUB_TYPE_RMAPBT:
	case XFS_SCRUB_TYPE_RTRMAPBT:
		return true;
	}

	return false;
}

/* Repair some metadata. */
enum check_outcome
xfs_repair_metadata(
	struct scrub_ctx		*ctx,
	int				fd,
	unsigned int			scrub_type,
	struct repair_item		*rpi,
	unsigned int			repair_flags)
{
	struct xfs_scrub_metadata	meta = { 0 };
	struct xfs_scrub_metadata	oldm;
	DEFINE_DESCR(dsc, ctx, format_scrub_descr);
	int				error;

	assert(scrub_type < XFS_SCRUB_TYPE_NR);
	assert(!debug_tweak_on("XFS_SCRUB_NO_KERNEL"));
	meta.sm_type = scrub_type;
	meta.sm_flags = rpi->rpi_oflags[scrub_type] | XFS_SCRUB_IFLAG_REPAIR;
	switch (xfrog_scrubbers[scrub_type].type) {
	case XFROG_SCRUB_TYPE_AGHEADER:
	case XFROG_SCRUB_TYPE_PERAG:
		meta.sm_agno = rpi->rpi_agno;
		break;
	case XFROG_SCRUB_TYPE_INODE:
		meta.sm_ino = rpi->rpi_ino;
		meta.sm_gen = rpi->rpi_gen;
		break;
	default:
		break;
	}

	if (!is_corrupt(&meta) && (repair_flags & XRM_REPAIR_ONLY))
		return CHECK_RETRY;

	memcpy(&oldm, &meta, sizeof(oldm));
	descr_set(&dsc, &oldm);

	if (needs_repair(&meta))
		str_info(ctx, descr_render(&dsc), _("Attempting repair."));
	else if (debug || verbose)
		str_info(ctx, descr_render(&dsc),
				_("Attempting optimization."));
retry:
	/*
	 * Certain types of repairs involve full filesystem scans and
	 * trylocking.  These repair activities are substantially more likely
	 * to succeed if they don't have to compete with other activity.  Use a
	 * exclusive lock to serialize the repair functions that require it,
	 * and a shared lock for those that can run concurrently.
	 */
	if (repair_needs_excl(&meta))
		pthread_rwlock_wrlock(&ctx->repair_rwlock);
	else
		pthread_rwlock_rdlock(&ctx->repair_rwlock);
	error = -xfrog_scrub_metadata(&ctx->mnt, &meta);
	pthread_rwlock_unlock(&ctx->repair_rwlock);
	switch (error) {
	case 0:
		/* No operational errors encountered. */
		break;
	case EUSERS:
		/* Operation skipped because we cannot freeze. */
		if (!(meta.sm_flags & XFS_SCRUB_IFLAG_FREEZE_OK) &&
		    ctx->freeze_ok) {
			meta.sm_flags |= XFS_SCRUB_IFLAG_FREEZE_OK;
			goto retry;
		}
		skip_slow_op(ctx, descr_render(&dsc),
_("Repair skipped because we can't freeze the fs."));
		return CHECK_TOOSLOW;
	case EDEADLOCK:
	case EBUSY:
		/* Filesystem is busy, try again later. */
		if (debug || verbose)
			str_info(ctx, descr_render(&dsc),
_("Filesystem is busy, deferring repair."));
		return CHECK_RETRY;
	case ESHUTDOWN:
		/* Filesystem is already shut down, abort. */
		str_error(ctx, descr_render(&dsc),
_("Filesystem is shut down, aborting."));
		return CHECK_ABORT;
	case ENOTTY:
	case EOPNOTSUPP:
		/*
		 * If we're in no-complain mode, requeue the check for
		 * later.  It's possible that an error in another
		 * component caused us to flag an error in this
		 * component.  Even if the kernel didn't think it
		 * could fix this, it's at least worth trying the scan
		 * again to see if another repair fixed it.
		 */
		if (!(repair_flags & XRM_COMPLAIN_IF_UNFIXED))
			return CHECK_RETRY;
		/*
		 * If we forced repairs or this is a preen, don't
		 * error out if the kernel doesn't know how to fix.
		 */
		if (is_unoptimized(&oldm) ||
		    debug_tweak_on("XFS_SCRUB_FORCE_REPAIR")) {
			repair_item_clean_state(rpi, &meta);
			return CHECK_DONE;
		}
		/* fall through */
	case EINVAL:
		/* Kernel doesn't know how to repair this? */
		str_corrupt(ctx, descr_render(&dsc),
_("Don't know how to fix; offline repair required."));
		repair_item_clean_state(rpi, &meta);
		return CHECK_DONE;
	case EROFS:
		/* Read-only filesystem, can't fix. */
		if (verbose || debug || needs_repair(&oldm))
			str_error(ctx, descr_render(&dsc),
_("Read-only filesystem; cannot make changes."));
		return CHECK_ABORT;
	case ENOENT:
		/* Metadata not present, just skip it. */
		repair_item_clean_state(rpi, &meta);
		return CHECK_DONE;
	case ENOMEM:
	case ENOSPC:
		/* Don't care if preen fails due to low resources. */
		if (is_unoptimized(&oldm) && !needs_repair(&oldm)) {
			repair_item_clean_state(rpi, &meta);
			return CHECK_DONE;
		}
		/* fall through */
	default:
		/*
		 * Operational error.  If the caller doesn't want us to
		 * complain about repair failures, tell the caller to requeue
		 * the repair for later and don't say a thing.  Otherwise,
		 * print an error, mark the item clean because we're done with
		 * trying to repair it, and bail out.
		 */
		if (!(repair_flags & XRM_COMPLAIN_IF_UNFIXED))
			return CHECK_RETRY;
		str_liberror(ctx, error, descr_render(&dsc));
		repair_item_clean_state(rpi, &meta);
		return CHECK_DONE;
	}

	if (repair_flags & XRM_COMPLAIN_IF_UNFIXED)
		scrub_warn_incomplete_scrub(ctx, &dsc, &meta);
	if (needs_repair(&meta)) {
		/*
		 * Still broken; if we've been told not to complain then we
		 * just requeue this and try again later.  Otherwise we
		 * log the error loudly and don't try again.
		 */
		if (!(repair_flags & XRM_COMPLAIN_IF_UNFIXED))
			return CHECK_RETRY;
		str_corrupt(ctx, descr_render(&dsc),
_("Repair unsuccessful; offline repair required."));
	} else if (meta.sm_flags & XFS_SCRUB_OFLAG_NO_REPAIR_NEEDED) {
		if (verbose)
			str_info(ctx, descr_render(&dsc),
					_("No modification needed."));
	} else {
		/* Clean operation, no corruption detected. */
		if (needs_repair(&oldm))
			record_repair(ctx, descr_render(&dsc),
					_("Repairs successful."));
		else
			record_preen(ctx, descr_render(&dsc),
					_("Optimization successful."));
	}

	repair_item_clean_state(rpi, &meta);
	return CHECK_DONE;
}
