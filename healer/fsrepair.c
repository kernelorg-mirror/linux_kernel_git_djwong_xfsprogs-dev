// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include <sys/wait.h>

#include "platform_defs.h"
#include "libfrog/fsgeom.h"
#include "libfrog/workqueue.h"
#include "xfs_healer.h"

enum repair_outcome {
	REPAIR_SUCCESS,
	REPAIR_FAILED,
	REPAIR_PROBABLY_OK,
	REPAIR_UNNECESSARY,
};

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

static const char *repair_report(enum repair_outcome o)
{
	switch (o) {
	case REPAIR_FAILED:
		return _("Repair unsuccessful; offline repair required.");
	case REPAIR_PROBABLY_OK:
		return _("Seems correct but cross-referencing failed; offline repair recommended.");
	case REPAIR_UNNECESSARY:
		return _("No modification needed.");
	case REPAIR_SUCCESS:
		return _("Repairs successful.");
	}

	return NULL;
}

struct u32_scrub {
	uint32_t	flag;
	uint32_t	scrub_type;
};

#define foreach_scrub_type(cur, mask, coll) \
	for ((cur) = (coll); (cur)->scrub_type != 0; (cur)++) \
		if ((mask) & (cur)->flag)

/* Call the kernel to repair some inode metadata. */
static inline uint32_t
__xfs_repair_metadata(
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
		return XFS_SCRUB_OFLAG_INCOMPLETE;

	return sm.sm_flags;
}

/* React to a fs-domain corruption event by repairing it. */
static enum what_next
try_repair_wholefs(
	struct healer_ctx			*ctx,
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
	const struct u32_scrub *f;

	foreach_scrub_type(f, hme->e.fs.mask, FS_STRUCTURES) {
		uint32_t oflags =
			__xfs_repair_metadata(mnt_fd, f->scrub_type, 0, 0, 0);
		enum repair_outcome outcome = from_repair_oflags(oflags);
		const char *report = repair_report(outcome);
		const char *what = fs_structname(f->flag);

		pthread_mutex_lock(&ctx->conlock);
		printf("%s: %s: %s\n", ctx->mntpoint, what, report);
		fflush(stdout);
		pthread_mutex_unlock(&ctx->conlock);

		if (outcome == REPAIR_FAILED)
			return NEED_FULL_REPAIR;
	}

	return REPAIR_DONE;
}

/* React to a group-domain corruption event by repairing it. */
static enum what_next
try_repair_group(
	struct healer_ctx			*ctx,
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
	const bool is_rtgroup = hme->domain == XFS_HEALTH_MONITOR_DOMAIN_RTGROUP;

	foreach_scrub_type(f, hme->e.group.mask, is_rtgroup ? RTG_STRUCTURES :
							       AG_STRUCTURES) {
		uint32_t oflags = __xfs_repair_metadata(mnt_fd, f->scrub_type,
						hme->e.group.gno, 0, 0);
		enum repair_outcome outcome = from_repair_oflags(oflags);
		const char *report = repair_report(outcome);
		const char *what = is_rtgroup ? rtg_structname(f->flag) :
						 ag_structname(f->flag);

		pthread_mutex_lock(&ctx->conlock);
		printf("%s: %s %s: %s\n", ctx->mntpoint,
				is_rtgroup ? _("rtgroup") : _("AG"), what,
				report);
		fflush(stdout);
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
	const struct u32_scrub *f;
	char			path[MAXPATHLEN];

	report_inode_location(ctx, hme, path, MAXPATHLEN);
	foreach_scrub_type(f, hme->e.inode.mask, INODE_STRUCTURES) {
		uint32_t oflags = __xfs_repair_metadata(mnt_fd, f->scrub_type,
						0, hme->e.inode.ino,
						hme->e.inode.gen);
		enum repair_outcome outcome = from_repair_oflags(oflags);
		const char *report = repair_report(outcome);
		const char *what = fs_structname(f->flag);

		pthread_mutex_lock(&ctx->conlock);
		printf("%s: %s: %s\n", path, what, report);
		fflush(stdout);
		pthread_mutex_unlock(&ctx->conlock);

		if (outcome == REPAIR_FAILED)
			return NEED_FULL_REPAIR;
	}

	return REPAIR_DONE;
}

/* Repair a metadata corruption. */
int
repair_metadata(
	struct healer_ctx			*ctx,
	const struct xfs_health_monitor_event	*hme)
{
	enum what_next				what_next;
	int					repair_fd;
	int					ret;

	ret = weakhandle_reopen(ctx->wh, &repair_fd);
	if (ret) {
		fprintf(stderr, "%s: %s: %s\n", ctx->mntpoint,
				_("cannot open filesystem to repair"),
				strerror(errno));
		return ret;
	}

	switch (hme->domain) {
	case XFS_HEALTH_MONITOR_DOMAIN_FS:
		what_next = try_repair_wholefs(ctx, repair_fd, hme);
		break;
	case XFS_HEALTH_MONITOR_DOMAIN_AG:
	case XFS_HEALTH_MONITOR_DOMAIN_RTGROUP:
		what_next = try_repair_group(ctx, repair_fd, hme);
		break;
	case XFS_HEALTH_MONITOR_DOMAIN_INODE:
		what_next = try_repair_inode(ctx, repair_fd, hme);
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
	char			svcname[PATH_MAX];
	pid_t			child_pid;
	int			child_status;
	int			ret;

	ret = weakhandle_instance_unit_name(ctx->wh, XFS_SCRUB_SVCNAME,
			svcname, PATH_MAX);
	if (ret) {
		fprintf(stderr, "%s: %s: %s\n", ctx->mntpoint,
				_("computing name of xfs_scrub service"),
				strerror(errno));
		return;
	}

	child_pid = fork();
	if (child_pid < 0) {
		perror(ctx->mntpoint);
		return;
	}
	if (!child_pid) {
		/* child starts the process */
		char		*argv[] = {
			"systemctl",
			"start",
			"--no-block",
			svcname,
			NULL,
		};

		ret = execvp("systemctl", argv);
		if (ret)
			perror("systemctl");

		exit(EXIT_FAILURE);
	}

	/* parent waits for process */
	waitpid(child_pid, &child_status, 0);

	if (WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0) {
		printf("%s: %s\n", ctx->mntpoint,
 _("Full repair: Repairs in progress."));
		fflush(stdout);
		return;
	}

	fprintf(stderr, "%s: %s\n", ctx->mntpoint,
 _("Could not start xfs_scrub service."));
}
