// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include <pthread.h>
#include <stdlib.h>

#include "platform_defs.h"
#include "libfrog/fsgeom.h"
#include "libfrog/workqueue.h"
#include "xfs_healer.h"

/*
 * Log string format is as follows:
 *
 * WHICH OBJECT: STATUS
 *
 * /mnt: 32 events lost
 * /mnt agno 0x5 bnobt, rmapbt: sick
 * /mnt rgno 0x5 bitmap: sick
 * /mnt ino 13 gen 0x3 bmbtd: sick
 * /mnt/a bmbtd: sick
 * /mnt ino 13 gen 0x3 pos 4096 len 4096: directio_write failed
 * /mnt/a pos 4096 len 4096: directio_read failed
 * /mnt datadev daddr 0x13 bbcount 0x5: media error
 * /mnt: filesystem shut down due to shenanigans, badness
 */

struct u32_str {
	uint32_t		flag;
	const char		*string;
};

#define foreach_u32_str(cur, coll) \
	for ((cur) = (coll); (cur)->string != NULL; (cur)++)

#define foreach_u32_str_in(cur, mask, coll) \
	for ((cur) = (coll); (cur)->string != NULL; (cur)++) \
		if ((mask) & (cur)->flag)

/* Report that the kernel lost events. */
static void
report_lost(
	struct healer_ctx			*ctx,
	const struct xfs_health_monitor_event	*hme)
{
	pthread_mutex_lock(&ctx->conlock);
	printf("%s: %llu %s\n", ctx->mntpoint,
			(unsigned long long)hme->e.lost.count,
			_("events lost"));
	fflush(stdout);
	pthread_mutex_unlock(&ctx->conlock);
}

/* Report that the monitor is running. */
static void
report_running(
	struct healer_ctx			*ctx,
	const struct xfs_health_monitor_event	*hme)
{
	pthread_mutex_lock(&ctx->conlock);
	printf("%s: %s\n", ctx->mntpoint, _("monitoring started"));
	fflush(stdout);
	pthread_mutex_unlock(&ctx->conlock);
}

/* Report that the filesystem was unmounted. */
static void
report_unmounted(
	struct healer_ctx			*ctx,
	const struct xfs_health_monitor_event	*hme)
{
	pthread_mutex_lock(&ctx->conlock);
	printf("%s: %s\n", ctx->mntpoint, _("filesystem unmounted"));
	fflush(stdout);
	pthread_mutex_unlock(&ctx->conlock);
}

static void
print_u32_flags(
	uint32_t		flags,
	const struct u32_str	*map)
{
	const struct u32_str	*f = map;
	const char		*tag = "";

	foreach_u32_str_in(f, flags, map) {
		printf("%s%s", tag, f->string);
		tag = ", ";
	}
}

static void
print_u32_enum(
	uint32_t		x,
	const struct u32_str	*map)
{
	const struct u32_str	*f = map;

	foreach_u32_str(f, map) {
		if (x == f->flag) {
			printf("%s", f->string);
			return;
		}
	}

	printf("0x%x", x);
}

static void
print_device_domain(
	uint32_t		domain)
{
#define X(code, string) { XFS_HEALTH_MONITOR_DOMAIN_ ## code, (string) }
	struct u32_str		DOMAINS[] = {
		X(DATADEV,	_("datadev")),
		X(RTDEV,	_("rtdev")),
		X(LOGDEV,	_("logdev")),
		{0,		NULL},
	};
#undef X

	print_u32_enum(domain, DOMAINS);
}

static void
print_fileio_type(
	uint32_t		type)
{
#define X(code, string) { XFS_HEALTH_MONITOR_TYPE_ ## code, (string) }
	struct u32_str		TYPES[] = {
		X(BUFREAD,	_("readahead")),
		X(BUFWRITE,	_("writeback")),
		X(DIOREAD,	_("directio_read")),
		X(DIOWRITE,	_("directio_write")),
		{0,		NULL},
	};
#undef X

	print_u32_enum(type, TYPES);
}

static void
print_health_type(
	uint32_t		type)
{
#define X(code, string) { XFS_HEALTH_MONITOR_TYPE_ ## code, (string) }
	struct u32_str		TYPES[] = {
		X(SICK,		_("sick")),
		X(CORRUPT,	_("corrupt")),
		X(HEALTHY,	_("healthy")),
		{0,		NULL},
	};
#undef X

	print_u32_enum(type, TYPES);
}

/* Report an abortive shutdown of the filesystem. */
static void
report_shutdown(
	struct healer_ctx			*ctx,
	const struct xfs_health_monitor_event	*hme)
{
#define X(code, string) { XFS_HEALTH_SHUTDOWN_ ## code, (string) }
	struct u32_str				REASONS[] = {
		X(META_IO_ERROR,	_("metadata I/O error")),
		X(LOG_IO_ERROR,		_("log I/O error")),
		X(FORCE_UMOUNT,		_("forced unmount")),
		X(CORRUPT_INCORE,	_("in-memory state corruption")),
		X(CORRUPT_ONDISK,	_("ondisk metadata corruption")),
		X(DEVICE_REMOVED,	_("device removed")),
		{0,			NULL},
	};
#undef X

	pthread_mutex_lock(&ctx->conlock);
	printf("%s: %s ", ctx->mntpoint, _("filesystem shut down due to"));
	print_u32_flags(hme->e.shutdown.reasons, REASONS);
	printf("\n");
	fflush(stdout);
	pthread_mutex_unlock(&ctx->conlock);
}

#define INO_X(code, string) { XFS_BS_SICK_ ## code, (string) }
#define DEFINE_INODE_STRINGS(name) \
struct u32_str		(name)[] = { \
	INO_X(INODE,	_("core")), \
	INO_X(BMBTD,	_("datafork")), \
	INO_X(BMBTA,	_("attrfork")), \
	INO_X(BMBTC,	_("cowfork")), \
	INO_X(DIR,	_("directory")), \
	INO_X(XATTR,	_("xattr")), \
	INO_X(SYMLINK,	_("symlink")), \
	INO_X(PARENT,	_("parent")), \
	INO_X(DIRTREE,	_("dirtree")), \
	{0,		NULL}, \
}

#define AG_X(code, string) { XFS_AG_GEOM_SICK_ ## code, (string) }
#define DEFINE_AG_STRINGS(name) \
struct u32_str		(name)[] = { \
	AG_X(SB,	_("super")), \
	AG_X(AGF,	_("agf")), \
	AG_X(AGFL,	_("agfl")), \
	AG_X(AGI,	_("agi")), \
	AG_X(BNOBT,	_("bnobt")), \
	AG_X(CNTBT,	_("cntbt")), \
	AG_X(INOBT,	_("inobt")), \
	AG_X(FINOBT,	_("finobt")), \
	AG_X(RMAPBT,	_("rmapbt")), \
	AG_X(REFCNTBT,	_("refcountbt")), \
	AG_X(INODES,	_("inodes")), \
	{0,		NULL}, \
}

#define RTG_X(code, string) { XFS_RTGROUP_GEOM_SICK_ ## code, (string) }
#define DEFINE_RTG_STRINGS(name) \
struct u32_str		(name)[] = { \
	RTG_X(SUPER,	_("super")), \
	RTG_X(BITMAP,	_("bitmap")), \
	RTG_X(SUMMARY,	_("summary")), \
	RTG_X(RMAPBT,	_("rmapbt")), \
	RTG_X(REFCNTBT,	_("refcountbt")), \
	{0,		NULL}, \
}

#define FS_X(code, string) { XFS_FSOP_GEOM_SICK_ ## code, (string) }
#define DEFINE_FS_STRINGS(name) \
struct u32_str		(name)[] = { \
	FS_X(COUNTERS,	_("fscounters")), \
	FS_X(UQUOTA,	_("usrquota")), \
	FS_X(GQUOTA,	_("grpquota")), \
	FS_X(PQUOTA,	_("prjquota")), \
	FS_X(RT_BITMAP,	_("bitmap")), \
	FS_X(RT_SUMMARY, _("summary")), \
	FS_X(QUOTACHECK, _("quotacheck")), \
	FS_X(NLINKS,	_("nlinks")), \
	FS_X(METADIR,	_("metadir")), \
	FS_X(METAPATH,	_("metapath")), \
	{0,		NULL}, \
}

static const char *
first_u32_str(
	uint32_t		flags,
	const struct u32_str	*map)
{
	const struct u32_str	*f = map;

	foreach_u32_str_in(f, flags, map)
		return f->string;

	return "???";
}

/* XFS_FSOP_GEOM_SICK -> name */
const char *fs_structname(uint32_t what)
{
	DEFINE_FS_STRINGS(FS_STRUCTURES);

	return first_u32_str(what, FS_STRUCTURES);
}

/* XFS_AG_GEOM_SICK -> name */
const char *ag_structname(uint32_t what)
{
	DEFINE_AG_STRINGS(AG_STRUCTURES);

	return first_u32_str(what, AG_STRUCTURES);
}

/* XFS_RTGROUP_GEOM_SICK -> name */
const char *rtg_structname(uint32_t what)
{
	DEFINE_RTG_STRINGS(RTG_STRUCTURES);

	return first_u32_str(what, RTG_STRUCTURES);
}

/* XFS_BS_GEOM_SICK -> name */
const char *inode_structname(uint32_t what)
{
	DEFINE_INODE_STRINGS(INODE_STRUCTURES);

	return first_u32_str(what, INODE_STRUCTURES);
}

/* Log a monitoring event to stdout. */
static void
report_loggable(
	struct healer_ctx			*ctx,
	const struct xfs_health_monitor_event	*hme)
{
	switch (hme->domain) {
	case XFS_HEALTH_MONITOR_DOMAIN_INODE:
		DEFINE_INODE_STRINGS(INODE_STRUCTURES);

		pthread_mutex_lock(&ctx->conlock);
		printf("%s %s %llu %s 0x%x ", ctx->mntpoint,
				_("ino"),
				(unsigned long long)hme->e.inode.ino,
				_("gen"),
				hme->e.inode.gen);
		print_u32_flags(hme->e.inode.mask, INODE_STRUCTURES);
		printf(": ");
		print_health_type(hme->type);
		printf("\n");
		fflush(stdout);
		pthread_mutex_unlock(&ctx->conlock);
		break;

	case XFS_HEALTH_MONITOR_DOMAIN_AG:
	case XFS_HEALTH_MONITOR_DOMAIN_RTGROUP:
		DEFINE_AG_STRINGS(AG_STRUCTURES);
		DEFINE_RTG_STRINGS(RTG_STRUCTURES);
		const bool is_rtgroup =
			hme->domain == XFS_HEALTH_MONITOR_DOMAIN_RTGROUP;

		pthread_mutex_lock(&ctx->conlock);
		printf("%s %s 0x%x ", ctx->mntpoint,
				is_rtgroup ? _("rgno") : _("agno"),
				hme->e.group.gno);
		print_u32_flags(hme->e.group.mask, is_rtgroup ? RTG_STRUCTURES :
								AG_STRUCTURES);
		printf(": ");
		print_health_type(hme->type);
		printf("\n");
		fflush(stdout);
		pthread_mutex_unlock(&ctx->conlock);
		break;

	case XFS_HEALTH_MONITOR_DOMAIN_FS:
		DEFINE_FS_STRINGS(FS_STRUCTURES);

		pthread_mutex_lock(&ctx->conlock);
		printf("%s ", ctx->mntpoint);
		print_u32_flags(hme->e.fs.mask, FS_STRUCTURES);
		printf(": ");
		print_health_type(hme->type);
		printf("\n");
		fflush(stdout);
		pthread_mutex_unlock(&ctx->conlock);
		break;

	case XFS_HEALTH_MONITOR_DOMAIN_DATADEV:
	case XFS_HEALTH_MONITOR_DOMAIN_RTDEV:
	case XFS_HEALTH_MONITOR_DOMAIN_LOGDEV:
		pthread_mutex_lock(&ctx->conlock);
		printf("%s ", ctx->mntpoint);
		print_device_domain(hme->domain);
		printf(" %s 0x%llx %s 0x%llx: %s\n",
				_("daddr"),
				(unsigned long long)hme->e.media.daddr,
				_("bbcount"),
				(unsigned long long)hme->e.media.bbcount,
				_("media error"));
		fflush(stdout);
		pthread_mutex_unlock(&ctx->conlock);
		break;

	case XFS_HEALTH_MONITOR_DOMAIN_FILERANGE:
		pthread_mutex_lock(&ctx->conlock);
		printf("%s %s %llu %s 0x%x %s %llu %s %llu: ",
				ctx->mntpoint,
				_("ino"),
				(unsigned long long)hme->e.filerange.ino,
				_("gen"),
				hme->e.filerange.gen,
				_("pos"),
				(unsigned long long)hme->e.filerange.pos,
				_("len"),
				(unsigned long long)hme->e.filerange.len);
		print_fileio_type(hme->type);
		printf(" %s\n", _("failed"));
		fflush(stdout);
		pthread_mutex_unlock(&ctx->conlock);
		break;
	}
}

void
report_event(
	struct healer_ctx			*ctx,
	const struct xfs_health_monitor_event	*hme)
{
	/*
	 * Deal with reporting-only events; these should always generate log
	 * messages.
	 */
	switch (hme->type) {
	case XFS_HEALTH_MONITOR_TYPE_LOST:
		report_lost(ctx, hme);
		return;
	case XFS_HEALTH_MONITOR_TYPE_RUNNING:
		report_running(ctx, hme);
		return;
	case XFS_HEALTH_MONITOR_TYPE_UNMOUNT:
		report_unmounted(ctx, hme);
		return;
	case XFS_HEALTH_MONITOR_TYPE_SHUTDOWN:
		report_shutdown(ctx, hme);
		return;
	}

	/* Deal with everything else. */
	if (ctx->log)
		report_loggable(ctx, hme);
}
