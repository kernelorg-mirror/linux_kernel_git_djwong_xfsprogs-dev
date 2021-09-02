// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "xfs.h"
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/statvfs.h>
#include "list.h"
#include "libfrog/paths.h"
#include "libfrog/fsgeom.h"
#include "libfrog/scrub.h"
#include "xfs_scrub.h"
#include "common.h"
#include "scrub.h"
#include "progress.h"
#include "repair.h"
#include "descr.h"
#include "scrub_private.h"

/* General repair routines. */

/*
 * Bitmap showing the correctness dependencies between scrub types for repairs.
 * There are no edges between AG btrees and AG headers because we can't mount
 * the filesystem if the btree root pointers in the AG headers are wrong.
 * Dependencies cannot cross scrub groups.
 */
#define DEP(x) (1U << (x))
static const unsigned int repair_deps[XFS_SCRUB_TYPE_NR] = {
	[XFS_SCRUB_TYPE_BMBTD]		= DEP(XFS_SCRUB_TYPE_INODE),
	[XFS_SCRUB_TYPE_BMBTA]		= DEP(XFS_SCRUB_TYPE_INODE),
	[XFS_SCRUB_TYPE_BMBTC]		= DEP(XFS_SCRUB_TYPE_INODE),
	[XFS_SCRUB_TYPE_DIR]		= DEP(XFS_SCRUB_TYPE_BMBTD),
	[XFS_SCRUB_TYPE_XATTR]		= DEP(XFS_SCRUB_TYPE_BMBTA),
	[XFS_SCRUB_TYPE_SYMLINK]	= DEP(XFS_SCRUB_TYPE_BMBTD),
	[XFS_SCRUB_TYPE_PARENT]		= DEP(XFS_SCRUB_TYPE_BMBTD),
	[XFS_SCRUB_TYPE_QUOTACHECK]	= DEP(XFS_SCRUB_TYPE_UQUOTA) |
					  DEP(XFS_SCRUB_TYPE_GQUOTA) |
					  DEP(XFS_SCRUB_TYPE_PQUOTA),
};
#undef DEP

/*
 * Decide if this repair item should be run one at a time.  The only types
 * requiring serialization are the ones that need to freeze the filesystem
 * and the ones that have to use trylocking to avoid ABBA deadlocks.
 */
static inline bool
repair_needs_excl(unsigned int scrub_type)
{
	switch (scrub_type) {
	case XFS_SCRUB_TYPE_PARENT:
	case XFS_SCRUB_TYPE_RMAPBT:
	case XFS_SCRUB_TYPE_RTRMAPBT:
	case XFS_SCRUB_TYPE_NLINKS:
		return true;
	}

	return false;
}

static inline void
restore_oldvec(
	struct xfs_scrub_vec	*oldvec,
	const struct scrub_item	*sri,
	unsigned int		scrub_type)
{
	oldvec->sv_type = scrub_type;
	oldvec->sv_flags = sri->sri_state[scrub_type] & SCRUB_ITEM_REPAIR_ANY;
}

static int
repair_epilogue(
	struct scrub_ctx		*ctx,
	struct descr			*dsc,
	struct scrub_item		*sri,
	unsigned int			repair_flags,
	const struct xfs_scrub_vec	*meta)
{
	struct xfs_scrub_vec		oldv;
	struct xfs_scrub_vec		*oldm = &oldv;
	unsigned int			scrub_type = meta->sv_type;
	bool				freeze_allowed;
	int				error = -meta->sv_ret;

	restore_oldvec(oldm, sri, meta->sv_type);

	freeze_allowed = sri->sri_state[scrub_type] & SCRUB_ITEM_FREEZE_OK;

	switch (error) {
	case 0:
		/* No operational errors encountered. */
		break;
	case EUSERS:
		/* Operation skipped because we cannot freeze. */
		if (!freeze_allowed && ctx->freeze_ok) {
			scrub_item_allow_freeze(sri, scrub_type);
			return 0;
		}

		/* Log that we skipped a slow check and forget this item. */
		skip_slow_op(ctx, descr_render(dsc),
_("Repair skipped because we can't freeze the fs."));
		scrub_item_clean_state(sri, scrub_type);
		return 0;
	case EDEADLOCK:
	case EBUSY:
		/* Filesystem is busy, try again later. */
		if (debug || verbose)
			str_info(ctx, descr_render(dsc),
_("Filesystem is busy, deferring repair."));
		return 0;
	case ESHUTDOWN:
		/* Filesystem is already shut down, abort. */
		str_error(ctx, descr_render(dsc),
_("Filesystem is shut down, aborting."));
		return ECANCELED;
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
			return 0;
		/*
		 * If we forced repairs or this is a preen, don't
		 * error out if the kernel doesn't know how to fix.
		 */
		if (is_unoptimized(oldm) ||
		    debug_tweak_on("XFS_SCRUB_FORCE_REPAIR")) {
			scrub_item_clean_state(sri, scrub_type);
			return 0;
		}
		fallthrough;
	case EINVAL:
		/* Kernel doesn't know how to repair this? */
		str_corrupt(ctx, descr_render(dsc),
_("Don't know how to fix; offline repair required."));
		scrub_item_clean_state(sri, scrub_type);
		return 0;
	case EROFS:
		/* Read-only filesystem, can't fix. */
		if (verbose || debug || needs_repair(oldm))
			str_error(ctx, descr_render(dsc),
_("Read-only filesystem; cannot make changes."));
		return ECANCELED;
	case ENOENT:
		/* Metadata not present, just skip it. */
		scrub_item_clean_state(sri, scrub_type);
		return 0;
	case ENOMEM:
	case ENOSPC:
		/* Don't care if preen fails due to low resources. */
		if (is_unoptimized(oldm) && !needs_repair(oldm)) {
			scrub_item_clean_state(sri, scrub_type);
			return 0;
		}
		fallthrough;
	default:
		/*
		 * Operational error.  If the caller doesn't want us to
		 * complain about repair failures, tell the caller to requeue
		 * the repair for later and don't say a thing.  Otherwise,
		 * print an error, mark the item clean because we're done with
		 * trying to repair it, and bail out.
		 */
		if (!(repair_flags & XRM_COMPLAIN_IF_UNFIXED))
			return 0;
		str_liberror(ctx, error, descr_render(dsc));
		scrub_item_clean_state(sri, scrub_type);
		return 0;
	}

	/*
	 * If the kernel says the repair was incomplete or that there was a
	 * cross-referencing discrepancy but no obvious corruption, we'll try
	 * the repair again, just in case the fs was busy.  Only retry so many
	 * times.
	 */
	if (want_retry(meta) && scrub_item_schedule_retry(sri, scrub_type))
		return 0;

	if (repair_flags & XRM_COMPLAIN_IF_UNFIXED)
		scrub_warn_incomplete_scrub(ctx, dsc, meta);
	if (needs_repair(meta) || is_incomplete(meta)) {
		/*
		 * Still broken; if we've been told not to complain then we
		 * just requeue this and try again later.  Otherwise we
		 * log the error loudly and don't try again.
		 */
		if (!(repair_flags & XRM_COMPLAIN_IF_UNFIXED))
			return 0;
		str_corrupt(ctx, descr_render(dsc),
_("Repair unsuccessful; offline repair required."));
	} else if (meta->sv_flags & XFS_SCRUB_OFLAG_NO_REPAIR_NEEDED) {
		if (verbose)
			str_info(ctx, descr_render(dsc),
					_("No modification needed."));
	} else {
		/* Clean operation, no corruption detected. */
		if (needs_repair(oldm))
			record_repair(ctx, descr_render(dsc),
					_("Repairs successful."));
		else
			record_preen(ctx, descr_render(dsc),
					_("Optimization successful."));
	}

	scrub_item_clean_state(sri, scrub_type);
	return 0;
}

/* Decide if the dependent scrub types of the given scrub type are ok. */
static bool
repair_item_dependencies_ok(
	const struct scrub_item	*sri,
	unsigned int		scrub_type)
{
	unsigned int		dep_mask = repair_deps[scrub_type];
	unsigned int		b;

	for (b = 0; dep_mask && b < XFS_SCRUB_TYPE_NR; b++, dep_mask >>= 1) {
		if (!(dep_mask & 1))
			continue;
		/*
		 * If this lower level object also needs repair, we can't fix
		 * the higher level item.
		 */
		if (sri->sri_state[b] & SCRUB_ITEM_NEEDSREPAIR)
			return false;
	}

	return true;
}

/* Decide if we want to repair a particular type of metadata. */
static bool
can_repair_now(
	const struct scrub_item	*sri,
	unsigned int		scrub_type,
	__u32			repair_mask,
	unsigned int		repair_flags)
{
	struct xfs_scrub_vec	oldvec;
	bool			repair_only;

	/* Do we even need to repair this thing? */
	if (!(sri->sri_state[scrub_type] & repair_mask))
		return false;

	restore_oldvec(&oldvec, sri, scrub_type);

	/*
	 * If the caller boosted the priority of this scrub type on behalf of a
	 * higher level repair by setting IFLAG_REPAIR, ignore REPAIR_ONLY.
	 */
	repair_only = (repair_flags & XRM_REPAIR_ONLY) &&
		      !(sri->sri_state[scrub_type] & SCRUB_ITEM_BOOST_REPAIR);
	if (!is_corrupt(&oldvec) && repair_only)
		return false;

	/*
	 * Don't try to repair higher level items if their lower-level
	 * dependencies haven't been verified, unless this is our last chance
	 * to fix things without complaint.
	 */
	if (!(repair_flags & XRM_COMPLAIN_IF_UNFIXED) &&
	    !repair_item_dependencies_ok(sri, scrub_type))
		return false;

	return true;
}

/*
 * Repair some metadata.
 *
 * Returns 0 for success (or repair item deferral), or ECANCELED to abort the
 * program.
 */
int
repair_call_kernel(
	struct scrub_ctx		*ctx,
	struct scrub_item		*sri,
	__u32				repair_mask,
	unsigned int			repair_flags)
{
	DEFINE_DESCR(dsc, ctx, format_scrubv_descr);
	struct scrubv_head		bh = { };
	struct xfs_scrub_vec		*v;
	struct xfs_fd			*xfdp = &ctx->mnt;
	unsigned int			scrub_type;
	bool				serial_repair = false;
	bool				need_barrier = false;
	int				error;

	assert(!debug_tweak_on("XFS_SCRUB_NO_KERNEL"));

	scrub_item_to_vhead(&bh, sri);
	descr_set(&dsc, &bh);

	foreach_scrub_type(scrub_type) {
		if (scrub_excessive_errors(ctx))
			return ECANCELED;

		if (!can_repair_now(sri, scrub_type, repair_mask,
					repair_flags))
			continue;

		if (need_barrier) {
			scrub_vhead_add_barrier(&bh);
			need_barrier = false;
		}

		scrub_vhead_add(&bh, sri, scrub_type, true);
		serial_repair |= repair_needs_excl(scrub_type);

		if (sri->sri_state[scrub_type] & SCRUB_ITEM_NEEDSREPAIR)
			str_info(ctx, descr_render(&dsc),
					_("Attempting repair."));
		else if (debug || verbose)
			str_info(ctx, descr_render(&dsc),
					_("Attempting optimization."));

		dbg_printf("repair %s flags %xh tries %u\n", descr_render(&dsc),
				sri->sri_state[scrub_type],
				sri->sri_tries[scrub_type]);

		/*
		 * One of the other scrub types depends on this one.  Set us up
		 * to add a repair barrier if we decide to schedule a repair
		 * after this one.  If the UNFIXED flag is set, that means this
		 * is our last chance to fix things, so we skip the barriers
		 * just let everything run.
		 */
		if (!(repair_flags & XRM_COMPLAIN_IF_UNFIXED) &&
		    (sri->sri_state[scrub_type] & SCRUB_ITEM_BARRIER))
			need_barrier = true;
	}

	/*
	 * Certain types of repairs involve full filesystem scans and
	 * trylocking.  These repair activities are substantially more likely
	 * to succeed if they don't have to compete with other activity.  Use a
	 * exclusive lock to serialize the repair functions that require it,
	 * and a shared lock for those that can run concurrently.
	 */
	if (serial_repair)
		pthread_rwlock_wrlock(&ctx->repair_rwlock);
	else
		pthread_rwlock_rdlock(&ctx->repair_rwlock);
	error = -xfrog_scrubv_metadata(xfdp, &bh.head);
	pthread_rwlock_unlock(&ctx->repair_rwlock);
	if (error)
		return error;

	foreach_bighead_vec(&bh, v) {
		/* Deal with barriers separately. */
		if (v->sv_type == XFS_SCRUB_TYPE_BARRIER) {
			/* -ECANCELED means the kernel stopped here. */
			if (v->sv_ret == -ECANCELED)
				return 0;
			if (v->sv_ret)
				return -v->sv_ret;
			continue;
		}

		error = repair_epilogue(ctx, &dsc, sri, repair_flags, v);
		if (error)
			return error;

		/* Maybe update progress if we fixed the problem. */
		if (!(repair_flags & XRM_NOPROGRESS) &&
		    !(sri->sri_state[v->sv_type] & SCRUB_ITEM_REPAIR_ANY))
			progress_add(1);
	}

	return 0;
}

/*
 * Prioritize action items in order of how long we can wait.
 *
 * To minimize the amount of repair work, we want to prioritize metadata
 * objects by perceived corruptness.  If CORRUPT is set, the fields are
 * just plain bad; try fixing that first.  Otherwise if XCORRUPT is set,
 * the fields could be bad, but the xref data could also be bad; we'll
 * try fixing that next.  Finally, if XFAIL is set, some other metadata
 * structure failed validation during xref, so we'll recheck this
 * metadata last since it was probably fine.
 *
 * For metadata that lie in the critical path of checking other metadata
 * (superblock, AG{F,I,FL}, inobt) we scrub and fix those things before
 * we even get to handling their dependencies, so things should progress
 * in order.
 */

struct action_item {
	struct list_head	list;
	struct scrub_item	sri;
};

/*
 * The operation of higher level metadata objects depends on the correctness of
 * lower level metadata objects.  This means that if X depends on Y, we must
 * investigate and correct all the observed issues with Y before we try to make
 * a correction to X.  For all scheduled repair activity on X, boost the
 * priority of repairs on all the Ys to ensure this correctness.
 */
static void
repair_item_boost_priorities(
	struct scrub_item		*sri)
{
	unsigned int			scrub_type;

	foreach_scrub_type(scrub_type) {
		unsigned int		dep_mask = repair_deps[scrub_type];
		unsigned int		b;

		if (repair_item_count_needsrepair(sri) == 0 || !dep_mask)
			continue;

		/*
		 * Check if the repairs for this scrub type depend on any other
		 * scrub types that have been flagged with cross-referencing
		 * errors and are not already tagged for the highest priority
		 * repair (SCRUB_ITEM_CORRUPT).  If so, boost the priority of
		 * that scrub type (via SCRUB_ITEM_BOOST_REPAIR) so that any
		 * problems with the dependencies will (hopefully) be fixed
		 * before we start repairs on this scrub type.
		 *
		 * So far in the history of xfs_scrub we have maintained that
		 * lower numbered scrub types do not depend on higher numbered
		 * scrub types, so we need only process the bit mask once.
		 */
		for (b = 0; b < XFS_SCRUB_TYPE_NR; b++, dep_mask >>= 1) {
			if (!dep_mask)
				break;
			if (!(dep_mask & 1))
				continue;
			if (!(sri->sri_state[b] & SCRUB_ITEM_REPAIR_XREF))
				continue;
			if (sri->sri_state[b] & SCRUB_ITEM_CORRUPT)
				continue;
			sri->sri_state[b] |= SCRUB_ITEM_BOOST_REPAIR;
		}
	}
}

/*
 * These are the scrub item state bits that must be copied when scheduling
 * a (per-AG) scrub type for immediate repairs.  The original state tracking
 * bits are left untouched to force a rescan in phase 4.
 */
#define MUSTFIX_SAVE_STATE	(SCRUB_ITEM_CORRUPT | \
				 SCRUB_ITEM_BOOST_REPAIR | \
				 SCRUB_ITEM_BARRIER)
/*
 * Figure out which AG metadata must be fixed before we can move on
 * to the inode scan.
 */
void
repair_item_mustfix(
	struct scrub_item	*sri,
	struct scrub_item	*fix_now,
	unsigned long long	*broken_primaries,
	unsigned long long	*broken_secondaries)
{
	unsigned int		scrub_type;

	assert(sri->sri_agno != -1U);
	repair_item_boost_priorities(sri);
	scrub_item_init_ag(fix_now, sri->sri_agno);

	*broken_primaries = 0;
	*broken_secondaries = 0;

	foreach_scrub_type(scrub_type) {
		unsigned int	state;

		state = sri->sri_state[scrub_type] & MUSTFIX_SAVE_STATE;
		if (!state)
			continue;

		switch (scrub_type) {
		case XFS_SCRUB_TYPE_RMAPBT:
			(*broken_secondaries)++;
			break;
		case XFS_SCRUB_TYPE_AGI:
		case XFS_SCRUB_TYPE_FINOBT:
		case XFS_SCRUB_TYPE_INOBT:
			fix_now->sri_state[scrub_type] = state;
			fallthrough;
		case XFS_SCRUB_TYPE_SB:
		case XFS_SCRUB_TYPE_AGF:
		case XFS_SCRUB_TYPE_AGFL:
		case XFS_SCRUB_TYPE_BNOBT:
		case XFS_SCRUB_TYPE_CNTBT:
		case XFS_SCRUB_TYPE_REFCNTBT:
			(*broken_primaries)++;
			break;
		default:
			abort();
			break;
		}
	}
}

/*
 * Allocate a certain number of repair lists for the scrub context.  Returns
 * zero or a positive error number.
 */
int
action_lists_alloc(
	size_t				nr,
	struct action_list		**listsp)
{
	struct action_list		*lists;
	xfs_agnumber_t			agno;

	lists = calloc(nr, sizeof(struct action_list));
	if (!lists)
		return errno;

	for (agno = 0; agno < nr; agno++)
		action_list_init(&lists[agno]);
	*listsp = lists;

	return 0;
}

/* Discard repair list contents. */
void
action_list_discard(
	struct action_list		*alist)
{
	struct action_item		*aitem;
	struct action_item		*n;

	list_for_each_entry_safe(aitem, n, &alist->list, list) {
		list_del(&aitem->list);
		free(aitem);
	}
}

/* Free the repair lists. */
void
action_lists_free(
	struct action_list		**listsp)
{
	free(*listsp);
	*listsp = NULL;
}

/* Initialize repair list */
void
action_list_init(
	struct action_list		*alist)
{
	INIT_LIST_HEAD(&alist->list);
}

/* Number of pending repairs in this list. */
size_t
action_list_length(
	struct action_list		*alist)
{
	struct action_item		*aitem;
	size_t				ret = 0;

	list_for_each_entry(aitem, &alist->list, list)
		ret += repair_item_count_needsrepair(&aitem->sri);
	return ret;
}

/* Add to the list of repairs. */
void
action_list_add(
	struct action_list		*alist,
	struct action_item		*aitem)
{
	list_add_tail(&aitem->list, &alist->list);
}

/* Repair everything on this list. */
int
action_list_process(
	struct scrub_ctx		*ctx,
	struct action_list		*alist,
	unsigned int			repair_flags)
{
	struct action_item		*aitem;
	struct action_item		*n;
	int				ret;

	list_for_each_entry_safe(aitem, n, &alist->list, list) {
		if (scrub_excessive_errors(ctx))
			return ECANCELED;

		ret = repair_item(ctx, &aitem->sri, repair_flags);
		if (ret)
			break;

		if (repair_item_count_needsrepair(&aitem->sri) == 0) {
			list_del(&aitem->list);
			free(aitem);
		}
	}

	return ret;
}

/*
 * For a given filesystem object, perform all repairs of a given class
 * (corrupt, xcorrupt, xfail, preen) if the repair item says it's needed.
 */
static int
repair_item_class(
	struct scrub_ctx		*ctx,
	struct scrub_item		*sri,
	uint8_t				repair_mask,
	unsigned int			flags)
{
	struct scrub_item		old_sri;
	int				error = 0;

	if (ctx->mode < SCRUB_MODE_REPAIR ||
	    !scrub_item_schedule_work(sri, repair_mask, repair_deps))
		return 0;

	do {
		memcpy(&old_sri, sri, sizeof(struct scrub_item));
		error = repair_call_kernel(ctx, sri, repair_mask, flags);
		if (error)
			return error;
	} while (scrub_item_call_kernel_again(sri, repair_mask, &old_sri));

	return 0;
}

/*
 * Repair all parts (i.e. scrub types) of this filesystem object for which
 * corruption has been observed directly.  Other types of repair work (fixing
 * cross referencing problems and preening) are deferred.
 *
 * This function should only be called to perform spot repairs of fs objects
 * during phase 2 and 3 while we still have open handles to those objects.
 */
int
repair_item_corruption(
	struct scrub_ctx	*ctx,
	struct scrub_item	*sri)
{
	repair_item_boost_priorities(sri);

	return repair_item_class(ctx, sri, SCRUB_ITEM_CORRUPT,
			XRM_REPAIR_ONLY | XRM_NOPROGRESS);
}

/*
 * Repair everything in this filesystem object that needs it.  This includes
 * cross-referencing and preening.
 */
int
repair_item(
	struct scrub_ctx	*ctx,
	struct scrub_item	*sri,
	unsigned int		flags)
{
	int			ret;

	repair_item_boost_priorities(sri);

	ret = repair_item_class(ctx, sri, SCRUB_ITEM_CORRUPT, flags);
	if (ret)
		return ret;

	ret = repair_item_class(ctx, sri, SCRUB_ITEM_XCORRUPT, flags);
	if (ret)
		return ret;

	ret = repair_item_class(ctx, sri, SCRUB_ITEM_XFAIL, flags);
	if (ret)
		return ret;

	return repair_item_class(ctx, sri, SCRUB_ITEM_PREEN, flags);
}

/* Defer all the repairs until phase 4. */
int
repair_item_defer(
	struct scrub_ctx		*ctx,
	const struct scrub_item	*sri)
{
	struct action_item		*aitem;
	unsigned int			agno;

	if (repair_item_count_needsrepair(sri) == 0)
		return 0;

	aitem = malloc(sizeof(struct action_item));
	if (!aitem) {
		int x = errno;
		str_errno(ctx, _("adding item to repair list"));
		return x;
	}
	INIT_LIST_HEAD(&aitem->list);
	memcpy(&aitem->sri, sri, sizeof(struct scrub_item));

	if (sri->sri_agno != -1U)
		agno = sri->sri_agno;
	else if (sri->sri_ino != -1ULL && sri->sri_gen != -1U)
		agno = cvt_ino_to_agno(&ctx->mnt, sri->sri_ino);
	else
		agno = 0;
	ASSERT(agno < ctx->mnt.fsgeom.agcount);

	action_list_add(&ctx->action_lists[agno], aitem);
	return 0;
}
