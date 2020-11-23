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
#include "xfs_scrub.h"
#include "common.h"
#include "scrub.h"
#include "progress.h"
#include "repair.h"

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
	struct repair_item	rpi;
};

/*
 * Figure out which AG metadata must be fixed before we can move on
 * to the inode scan.
 */
void
repair_item_mustfix(
	struct repair_item	*rpi,
	struct repair_item	*fix_now,
	unsigned long long	*broken_primaries,
	unsigned long long	*broken_secondaries)
{
	unsigned int		scrub_type;

	assert(rpi->rpi_agno != -1U);
	repair_item_init_ag(fix_now, rpi->rpi_agno);

	*broken_primaries = 0;
	*broken_secondaries = 0;

	for (scrub_type = 0; scrub_type < XFS_SCRUB_TYPE_NR; scrub_type++) {
		if (!(rpi->rpi_oflags[scrub_type] & XFS_SCRUB_OFLAG_CORRUPT))
			continue;

		switch (scrub_type) {
		case XFS_SCRUB_TYPE_RMAPBT:
			(*broken_secondaries)++;
			break;
		case XFS_SCRUB_TYPE_FINOBT:
		case XFS_SCRUB_TYPE_INOBT:
			fix_now->rpi_oflags[scrub_type] |=
						XFS_SCRUB_OFLAG_CORRUPT;
			rpi->rpi_oflags[scrub_type] &= 
						~XFS_SCRUB_OFLAG_CORRUPT;
			/* fall through */
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
		alist->nr--;
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
	alist->nr = 0;
	alist->sorted = false;
}

/* Number of pending repairs in this list. */
size_t
action_list_length(
	struct action_list		*alist)
{
	struct action_item		*aitem;
	size_t				ret = 0;

	list_for_each_entry(aitem, &alist->list, list) {
		unsigned int		b;

		for (b = 0; b < XFS_SCRUB_TYPE_NR; b++)
			if (aitem->rpi.rpi_oflags[b])
				ret++;
	}

	return ret;
}

/* Add to the list of repairs. */
void
action_list_add(
	struct action_list		*alist,
	struct action_item		*aitem)
{
	list_add_tail(&aitem->list, &alist->list);
	alist->nr++;
	alist->sorted = false;
}

/* Repair everything on this list. */
int
action_list_process(
	struct scrub_ctx		*ctx,
	int				fd,
	struct action_list		*alist,
	unsigned int			repair_flags)
{
	struct action_item		*aitem;
	struct action_item		*n;
	int				ret;

	list_for_each_entry_safe(aitem, n, &alist->list, list) {
		if (scrub_excessive_errors(ctx))
			return ECANCELED;

		ret = repair_item(ctx, &aitem->rpi, repair_flags);
		if (ret)
			break;

		if (repair_item_is_clean(&aitem->rpi)) {
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
	struct repair_item		*rpi,
	__u32				repair_mask,
	unsigned int			flags)
{
	unsigned int			scrub_type;

	for (scrub_type = 0; scrub_type < XFS_SCRUB_TYPE_NR; scrub_type++) {
		if (scrub_excessive_errors(ctx))
			return ECANCELED;

		if (rpi->rpi_oflags[scrub_type] & repair_mask) {
			enum check_outcome	fix;

			fix = xfs_repair_metadata(ctx, ctx->mnt.fd, scrub_type,
					rpi, flags);
			switch (fix) {
			case CHECK_TOOSLOW:
			case CHECK_DONE:
				if (!(flags & ALP_NOPROGRESS))
					progress_add(1);
				continue;
			case CHECK_ABORT:
				return ECANCELED;
			case CHECK_RETRY:
				continue;
			case CHECK_REPAIR:
				abort();
			}
		}
	}

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
	struct repair_item	*rpi)
{
	return repair_item_class(ctx, rpi, XFS_SCRUB_OFLAG_CORRUPT,
			ALP_REPAIR_ONLY | ALP_NOPROGRESS);
}

/*
 * Repair everything in this filesystem object that needs it.  This includes
 * cross-referencing and preening.
 */
int
repair_item(
	struct scrub_ctx	*ctx,
	struct repair_item	*rpi,
	unsigned int		flags)
{
	int			ret;

	ret = repair_item_class(ctx, rpi, XFS_SCRUB_OFLAG_CORRUPT, flags);
	if (ret)
		return ret;

	ret = repair_item_class(ctx, rpi, XFS_SCRUB_OFLAG_XCORRUPT, flags);
	if (ret)
		return ret;

	ret = repair_item_class(ctx, rpi, XFS_SCRUB_OFLAG_XFAIL, flags);
	if (ret)
		return ret;

	return repair_item_class(ctx, rpi, XFS_SCRUB_OFLAG_PREEN, flags);
}

/* Defer all the repairs until phase 4. */
int
repair_item_defer(
	struct scrub_ctx		*ctx,
	const struct repair_item	*rpi)
{
	struct action_item		*aitem;
	unsigned int			agno;

	if (repair_item_is_clean(rpi))
		return 0;

	aitem = malloc(sizeof(struct action_item));
	if (!aitem) {
		int x = errno;
		str_errno(ctx, _("adding item to repair list"));
		return x;
	}
	INIT_LIST_HEAD(&aitem->list);
	memcpy(&aitem->rpi, rpi, sizeof(struct repair_item));

	if (rpi->rpi_agno != -1U)
		agno = rpi->rpi_agno;
	else if (rpi->rpi_ino != -1ULL && rpi->rpi_gen != -1U)
		agno = cvt_ino_to_agno(&ctx->mnt, rpi->rpi_ino);
	else
		agno = 0;
	ASSERT(agno < ctx->mnt.fsgeom.agcount);

	action_list_add(&ctx->action_lists[agno], aitem);
	return 0;
}

/* Is this filesystem object totally clean? */
bool
repair_item_is_clean(
	const struct repair_item	*rpi)
{
	unsigned int			scrub_type;

	for (scrub_type = 0; scrub_type < XFS_SCRUB_TYPE_NR; scrub_type++)
		if (rpi->rpi_oflags[scrub_type])
			return false;

	return true;
}
