// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025-2026 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"

#include "platform_defs.h"
#include "libfrog/fsgeom.h"
#include "libfrog/workqueue.h"
#include "libfrog/healthevent.h"
#include "libfrog/systemd.h"
#include "xfs_healer.h"

enum what_next {
	NEED_FULL_REPAIR,
	REPAIR_DONE,
};

/* Translate scrub output flags to outcome. */
static enum repair_outcome from_repair_oflags(uint32_t oflags)
{
	if (oflags & (XFS_SCRUB_OFLAG_CORRUPT | XFS_SCRUB_OFLAG_INCOMPLETE))
		return REPAIR_FAILED;

	if (oflags & XFS_SCRUB_OFLAG_XFAIL)
		return REPAIR_PROBABLY_OK;

	if (oflags & XFS_SCRUB_OFLAG_NO_REPAIR_NEEDED)
		return REPAIR_UNNECESSARY;

	return REPAIR_SUCCESS;
}

struct u32_scrub {
	uint32_t	event_mask;
	uint32_t	scrub_type;
};

#define foreach_scrub_type(cur, mask, coll) \
	for ((cur) = (coll); (cur)->scrub_type != 0; (cur)++) \
		if ((mask) & (cur)->event_mask)

/* Call the kernel to repair some inode metadata. */
static inline enum repair_outcome
xfs_repair_metadata(
	int			fd,
	uint32_t		scrub_type,
	uint32_t		group,
	uint64_t		ino,
	uint32_t		gen)
{
	struct xfs_scrub_metadata sm = {
		.sm_type = scrub_type,
		.sm_flags = XFS_SCRUB_IFLAG_REPAIR,
		.sm_ino = ino,
		.sm_gen = gen,
		.sm_agno = group,
	};
	int			ret;

	ret = ioctl(fd, XFS_IOC_SCRUB_METADATA, &sm);
	if (ret)
		return REPAIR_FAILED;

	return from_repair_oflags(sm.sm_flags);
}

/* React to a fs-domain corruption event by repairing it. */
static enum what_next
try_repair_wholefs(
	struct healer_ctx			*ctx,
	const struct hme_prefix			*pfx,
	int					mnt_fd,
	const struct xfs_health_monitor_event	*hme)
{
#define X(code, type) { XFS_FSOP_GEOM_SICK_ ## code, XFS_SCRUB_TYPE_ ## type }
	static const struct u32_scrub		FS_STRUCTURES[] = {
		X(COUNTERS,	FSCOUNTERS),
		X(UQUOTA,	UQUOTA),
		X(GQUOTA,	GQUOTA),
		X(PQUOTA,	PQUOTA),
		X(RT_BITMAP,	RTBITMAP),
		X(RT_SUMMARY,	RTSUM),
		X(QUOTACHECK,	QUOTACHECK),
		X(NLINKS,	NLINKS),
		{0,		0},
	};
#undef X
	const struct u32_scrub	*f;

	foreach_scrub_type(f, hme->e.fs.mask, FS_STRUCTURES) {
		enum repair_outcome	outcome =
			xfs_repair_metadata(mnt_fd, f->scrub_type, 0, 0, 0);

		pthread_mutex_lock(&ctx->conlock);
		report_health_repair(pfx, hme, f->event_mask, outcome);
		pthread_mutex_unlock(&ctx->conlock);

		if (outcome == REPAIR_FAILED)
			return NEED_FULL_REPAIR;
	}

	return REPAIR_DONE;
}

/* React to an ag corruption event by repairing it. */
static enum what_next
try_repair_ag(
	struct healer_ctx			*ctx,
	const struct hme_prefix			*pfx,
	int					mnt_fd,
	const struct xfs_health_monitor_event	*hme)
{
#define X(code, type) { XFS_AG_GEOM_SICK_ ## code, XFS_SCRUB_TYPE_ ## type }
	static const struct u32_scrub		AG_STRUCTURES[] = {
		X(SB,		SB),
		X(AGF,		AGF),
		X(AGFL,		AGFL),
		X(AGI,		AGI),
		X(BNOBT,	BNOBT),
		X(CNTBT,	CNTBT),
		X(INOBT,	INOBT),
		X(FINOBT,	FINOBT),
		X(RMAPBT,	RMAPBT),
		X(REFCNTBT,	REFCNTBT),
		{0,		0},
	};
#undef X
	const struct u32_scrub *f;

	foreach_scrub_type(f, hme->e.group.mask, AG_STRUCTURES) {
		enum repair_outcome	outcome =
			xfs_repair_metadata(mnt_fd, f->scrub_type,
					hme->e.group.gno, 0, 0);

		pthread_mutex_lock(&ctx->conlock);
		report_health_repair(pfx, hme, f->event_mask, outcome);
		pthread_mutex_unlock(&ctx->conlock);

		if (outcome == REPAIR_FAILED)
			return NEED_FULL_REPAIR;
	}

	return REPAIR_DONE;
}

/* React to a rtgroup corruption event by repairing it. */
static enum what_next
try_repair_rtgroup(
	struct healer_ctx			*ctx,
	const struct hme_prefix			*pfx,
	int					mnt_fd,
	const struct xfs_health_monitor_event	*hme)
{
#define X(code, type) { XFS_RTGROUP_GEOM_SICK_ ## code, XFS_SCRUB_TYPE_ ## type }
	static const struct u32_scrub		RTG_STRUCTURES[] = {
		X(SUPER,	RGSUPER),
		X(BITMAP,	RTBITMAP),
		X(SUMMARY,	RTSUM),
		X(RMAPBT,	RTRMAPBT),
		X(REFCNTBT,	RTREFCBT),
		{0,		0},
	};
#undef X
	const struct u32_scrub *f;

	foreach_scrub_type(f, hme->e.group.mask, RTG_STRUCTURES) {
		enum repair_outcome	outcome =
			xfs_repair_metadata(mnt_fd, f->scrub_type,
					hme->e.group.gno, 0, 0);

		pthread_mutex_lock(&ctx->conlock);
		report_health_repair(pfx, hme, f->event_mask, outcome);
		pthread_mutex_unlock(&ctx->conlock);

		if (outcome == REPAIR_FAILED)
			return NEED_FULL_REPAIR;
	}

	return REPAIR_DONE;
}

/* React to a inode-domain corruption event by repairing it. */
static enum what_next
try_repair_inode(
	struct healer_ctx			*ctx,
	const struct hme_prefix			*orig_pfx,
	int					mnt_fd,
	const struct xfs_health_monitor_event	*hme)
{
#define X(code, type) { XFS_BS_SICK_ ## code, XFS_SCRUB_TYPE_ ## type }
	static const struct u32_scrub		INODE_STRUCTURES[] = {
		X(INODE,	INODE),
		X(BMBTD,	BMBTD),
		X(BMBTA,	BMBTA),
		X(BMBTC,	BMBTC),
		X(DIR,		DIR),
		X(XATTR,	XATTR),
		X(SYMLINK,	SYMLINK),
		X(PARENT,	PARENT),
		X(DIRTREE,	DIRTREE),
		{0,		0},
	};
#undef X
	struct hme_prefix	new_pfx;
	const struct hme_prefix	*pfx = orig_pfx;
	const struct u32_scrub	*f;

	foreach_scrub_type(f, hme->e.inode.mask, INODE_STRUCTURES) {
		enum repair_outcome	outcome =
			xfs_repair_metadata(mnt_fd, f->scrub_type,
					0, hme->e.inode.ino, hme->e.inode.gen);

		/*
		 * Try again to find the file path, maybe we fixed the dir
		 * tree.
		 */
		if (!hme_prefix_has_path(pfx)) {
			lookup_path(ctx, hme, &new_pfx);
			if (hme_prefix_has_path(&new_pfx))
				pfx = &new_pfx;
		}

		pthread_mutex_lock(&ctx->conlock);
		report_health_repair(pfx, hme, f->event_mask, outcome);
		pthread_mutex_unlock(&ctx->conlock);

		if (outcome == REPAIR_FAILED)
			return NEED_FULL_REPAIR;
	}

	return REPAIR_DONE;
}

/* Make sure the reopened file is on the same fs as the monitor. */
static bool
is_same_fs(
	int				mnt_fd,
	void				*data)
{
	struct xfs_health_file_on_monitored_fs hms = {
		.fd = mnt_fd,
	};
	FILE				*mon_fp = data;
	int				ret;

	ret = ioctl(fileno(mon_fp), XFS_IOC_HEALTH_FD_ON_MONITORED_FS, &hms);
	return ret == 0;
}

/* Repair a metadata corruption. */
int
repair_metadata(
	struct healer_ctx			*ctx,
	const struct hme_prefix			*pfx,
	const struct xfs_health_monitor_event	*hme)
{
	enum what_next				what_next;
	int					repair_fd;
	int					ret;

	ret = weakhandle_reopen(ctx->wh, &repair_fd, is_same_fs, ctx->mon_fp);
	if (ret) {
		fprintf(stderr, "%s: %s: %s\n", ctx->mntpoint,
				_("cannot open filesystem to repair"),
				strerror(errno));
		return ret;
	}

	switch (hme->domain) {
	case XFS_HEALTH_MONITOR_DOMAIN_FS:
		what_next = try_repair_wholefs(ctx, pfx, repair_fd, hme);
		break;
	case XFS_HEALTH_MONITOR_DOMAIN_AG:
		what_next = try_repair_ag(ctx, pfx, repair_fd, hme);
		break;
	case XFS_HEALTH_MONITOR_DOMAIN_RTGROUP:
		what_next = try_repair_rtgroup(ctx, pfx, repair_fd, hme);
		break;
	case XFS_HEALTH_MONITOR_DOMAIN_INODE:
		what_next = try_repair_inode(ctx, pfx, repair_fd, hme);
		break;
	default:
		what_next = REPAIR_DONE;
	}

	/* Transform into a full repair if we failed to fix this item. */
	if (what_next == NEED_FULL_REPAIR)
		run_full_repair(ctx);

	close(repair_fd);
	return 0;
}

/* Ask the kernel if it supports repairs. */
bool
healer_can_repair(
	struct healer_ctx	*ctx)
{
	struct xfs_scrub_metadata sm = {
		.sm_type = XFS_SCRUB_TYPE_PROBE,
		.sm_flags = XFS_SCRUB_IFLAG_REPAIR,
	};
	int			ret;

	/* assume any errno means not supported */
	ret = ioctl(ctx->mnt.fd, XFS_IOC_SCRUB_METADATA, &sm);
	return ret ? false : true;
}

/* Run a full repair of the filesystem using the background fsck service. */
void
run_full_repair(
	struct healer_ctx	*ctx)
{
	char			unitname[PATH_MAX];
	int			ret;

	ret = weakhandle_instance_unit_name(ctx->wh, XFS_SCRUB_SVCNAME,
			unitname, PATH_MAX);
	if (ret) {
		fprintf(stderr, "%s: %s\n", ctx->mntpoint,
				_("Could not determine xfs_scrub unit name."));
		return;
	}

	/*
	 * Scrub could already be repairing something, so try to start the unit
	 * and be content if it's already running.
	 */
	ret = systemd_manage_unit(UM_START, unitname);
	if (ret) {
		fprintf(stderr, "%s: %s: %s\n", ctx->mntpoint,
				_("Could not start xfs_scrub service unit"),
				unitname);
		return;
	}

	printf("%s: %s\n", ctx->mntpoint, _("Full repairs in progress."));
	fflush(stdout);
}
