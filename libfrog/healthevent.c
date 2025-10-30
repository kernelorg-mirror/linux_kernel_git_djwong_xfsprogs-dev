// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025-2026 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"

#include "platform_defs.h"
#include "healthevent.h"

/*
 * The healthmon log string format is as follows:
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

/*
 * Given a bitmask and a mapping of bits to strings, print the bitmask as a
 * list of strings.
 */
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

/*
 * Given a value and a mapping of values to strings, return the matching string
 * or confusion.
 */
static const char *
u32_enum_string(
	uint32_t		x,
	const struct u32_str	*map)
{
	const struct u32_str	*f = map;

	foreach_u32_str(f, map) {
		if (x == f->flag)
			return f->string;
	}

	return _("unknown value");
}

static const char *
device_domain_string(
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

	return u32_enum_string(domain, DOMAINS);
}

static const char *
fileio_type_string(
	uint32_t		type)
{
#define X(code, string) { XFS_HEALTH_MONITOR_TYPE_ ## code, (string) }
	struct u32_str		TYPES[] = {
		X(BUFREAD,	_("buffered_read")),
		X(BUFWRITE,	_("buffered_write")),
		X(DIOREAD,	_("directio_read")),
		X(DIOWRITE,	_("directio_write")),
		X(DATALOST,	_("media")),
		{0,		NULL},
	};
#undef X

	return u32_enum_string(type, TYPES);
}

static const char *
health_type_string(
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

	return u32_enum_string(type, TYPES);
}

#define INO_X(code, string) { XFS_BS_SICK_ ## code, (string) }
#define DEFINE_INODE_STRINGS(name) \
struct u32_str		(name)[] = { \
	INO_X(INODE,		_("core")), \
	INO_X(BMBTD,		_("datafork")), \
	INO_X(BMBTA,		_("attrfork")), \
	INO_X(BMBTC,		_("cowfork")), \
	INO_X(DIR,		_("directory")), \
	INO_X(XATTR,		_("xattr")), \
	INO_X(SYMLINK,		_("symlink")), \
	INO_X(PARENT,		_("parent")), \
	INO_X(DIRTREE,		_("dirtree")), \
	{0,			NULL}, \
}

#define AG_X(code, string) { XFS_AG_GEOM_SICK_ ## code, (string) }
#define DEFINE_AG_STRINGS(name) \
struct u32_str		(name)[] = { \
	AG_X(SB,		_("super")), \
	AG_X(AGF,		_("agf")), \
	AG_X(AGFL,		_("agfl")), \
	AG_X(AGI,		_("agi")), \
	AG_X(BNOBT,		_("bnobt")), \
	AG_X(CNTBT,		_("cntbt")), \
	AG_X(INOBT,		_("inobt")), \
	AG_X(FINOBT,		_("finobt")), \
	AG_X(RMAPBT,		_("rmapbt")), \
	AG_X(REFCNTBT,		_("refcountbt")), \
	AG_X(INODES,		_("inodes")), \
	{0,			NULL}, \
}

#define RTG_X(code, string) { XFS_RTGROUP_GEOM_SICK_ ## code, (string) }
#define DEFINE_RTG_STRINGS(name) \
struct u32_str		(name)[] = { \
	RTG_X(SUPER,		_("super")), \
	RTG_X(BITMAP,		_("bitmap")), \
	RTG_X(SUMMARY,		_("summary")), \
	RTG_X(RMAPBT,		_("rmapbt")), \
	RTG_X(REFCNTBT,		_("refcountbt")), \
	{0,			NULL}, \
}

#define FS_X(code, string) { XFS_FSOP_GEOM_SICK_ ## code, (string) }
#define DEFINE_FS_STRINGS(name) \
struct u32_str		(name)[] = { \
	FS_X(COUNTERS,		_("fscounters")), \
	FS_X(UQUOTA,		_("usrquota")), \
	FS_X(GQUOTA,		_("grpquota")), \
	FS_X(PQUOTA,		_("prjquota")), \
	FS_X(RT_BITMAP,		_("bitmap")), \
	FS_X(RT_SUMMARY,	_("summary")), \
	FS_X(QUOTACHECK,	_("quotacheck")), \
	FS_X(NLINKS,		_("nlinks")), \
	FS_X(METADIR,		_("metadir")), \
	FS_X(METAPATH,		_("metapath")), \
	{0,			NULL}, \
}

/* Report that the kernel lost events. */
static void
report_lost(
	const struct hme_prefix			*pfx,
	const struct xfs_health_monitor_event	*hme)
{
	printf("%s: %llu %s\n", pfx->mountpoint,
			(unsigned long long)hme->e.lost.count,
			_("events lost"));
	fflush(stdout);
}

/* Report that the monitor is running. */
static void
report_running(
	const struct hme_prefix			*pfx,
	const struct xfs_health_monitor_event	*hme)
{
	printf("%s: %s\n", pfx->mountpoint, _("monitoring started"));
	fflush(stdout);
}

/* Report that the filesystem was unmounted. */
static void
report_unmounted(
	const struct hme_prefix			*pfx,
	const struct xfs_health_monitor_event	*hme)
{
	printf("%s: %s\n", pfx->mountpoint, _("filesystem unmounted"));
	fflush(stdout);
}

/* Report an abortive shutdown of the filesystem. */
static void
report_shutdown(
	const struct hme_prefix			*pfx,
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

	printf("%s: %s ", pfx->mountpoint, _("filesystem shut down due to"));
	print_u32_flags(hme->e.shutdown.reasons, REASONS);
	printf("\n");
	fflush(stdout);
}

/* Report inode metadata corruption */
static void
report_inode(
	const struct hme_prefix			*pfx,
	const struct xfs_health_monitor_event	*hme)
{
	DEFINE_INODE_STRINGS(INODE_STRUCTURES);

	if (hme_prefix_has_path(pfx))
		printf("%s ", pfx->path);
	else
		printf("%s %s %llu %s 0x%x ",
				pfx->mountpoint,
				_("ino"),
				(unsigned long long)hme->e.inode.ino,
				_("gen"),
				hme->e.inode.gen);
	print_u32_flags(hme->e.inode.mask, INODE_STRUCTURES);
	printf(": %s\n", health_type_string(hme->type));
	fflush(stdout);
}

/* Report AG metadata corruption */
static void
report_ag(
	const struct hme_prefix			*pfx,
	const struct xfs_health_monitor_event	*hme)
{
	DEFINE_AG_STRINGS(AG_STRUCTURES);

	printf("%s %s 0x%x ", pfx->mountpoint, _("agno"),
			hme->e.group.gno);
	print_u32_flags(hme->e.group.mask, AG_STRUCTURES);
	printf(": %s\n", health_type_string(hme->type));
	fflush(stdout);
}

/* Report rtgroup metadata corruption */
static void
report_rtgroup(
	const struct hme_prefix			*pfx,
	const struct xfs_health_monitor_event	*hme)
{
	DEFINE_RTG_STRINGS(RTG_STRUCTURES);

	printf("%s %s 0x%x ", pfx->mountpoint, _("rgno"),
			hme->e.group.gno);
	print_u32_flags(hme->e.group.mask, RTG_STRUCTURES);
	printf(": %s\n", health_type_string(hme->type));
	fflush(stdout);
}

/* Report fs-wide metadata corruption */
static void
report_fs(
	const struct hme_prefix			*pfx,
	const struct xfs_health_monitor_event	*hme)
{
	DEFINE_FS_STRINGS(FS_STRUCTURES);

	printf("%s ", pfx->mountpoint);
	print_u32_flags(hme->e.fs.mask, FS_STRUCTURES);
	printf(": %s\n", health_type_string(hme->type));
	fflush(stdout);
}

/* Report device media corruption */
static void
report_device_error(
	const struct hme_prefix			*pfx,
	const struct xfs_health_monitor_event	*hme)
{
	printf("%s %s %s 0x%llx %s 0x%llx: %s\n", pfx->mountpoint,
			device_domain_string(hme->domain),
			_("daddr"),
			(unsigned long long)hme->e.media.daddr,
			_("bbcount"),
			(unsigned long long)hme->e.media.bbcount,
			_("media error"));
	fflush(stdout);
}

/* Report file range errors */
static void
report_file_range(
	const struct hme_prefix			*pfx,
	const struct xfs_health_monitor_event	*hme)
{
	if (hme_prefix_has_path(pfx))
		printf("%s ", pfx->path);
	else
		printf("%s %s %llu %s 0x%x ",
				pfx->mountpoint,
				_("ino"),
				(unsigned long long)hme->e.filerange.ino,
				_("gen"),
				hme->e.filerange.gen);
	if (hme->type != XFS_HEALTH_MONITOR_TYPE_DATALOST &&
	    hme->e.filerange.error)
		printf("%s %llu %s %llu: %s: %s\n",
				_("pos"),
				(unsigned long long)hme->e.filerange.pos,
				_("len"),
				(unsigned long long)hme->e.filerange.len,
				fileio_type_string(hme->type),
				strerror(hme->e.filerange.error));
	else
		printf("%s %llu %s %llu: %s %s\n",
				_("pos"),
				(unsigned long long)hme->e.filerange.pos,
				_("len"),
				(unsigned long long)hme->e.filerange.len,
				fileio_type_string(hme->type),
				_("failed"));
	fflush(stdout);
}

/* Log a health monitoring event to stdout. */
void
hme_report_event(
	const struct hme_prefix			*pfx,
	const struct xfs_health_monitor_event	*hme)
{
	switch (hme->domain) {
	case XFS_HEALTH_MONITOR_DOMAIN_MOUNT:
		switch (hme->type) {
		case XFS_HEALTH_MONITOR_TYPE_LOST:
			report_lost(pfx, hme);
			return;
		case XFS_HEALTH_MONITOR_TYPE_RUNNING:
			report_running(pfx, hme);
			return;
		case XFS_HEALTH_MONITOR_TYPE_UNMOUNT:
			report_unmounted(pfx, hme);
			return;
		case XFS_HEALTH_MONITOR_TYPE_SHUTDOWN:
			report_shutdown(pfx, hme);
			return;
		}
		break;
	case XFS_HEALTH_MONITOR_DOMAIN_INODE:
		report_inode(pfx, hme);
		break;
	case XFS_HEALTH_MONITOR_DOMAIN_AG:
		report_ag(pfx, hme);
		break;
	case XFS_HEALTH_MONITOR_DOMAIN_RTGROUP:
		report_rtgroup(pfx, hme);
		break;
	case XFS_HEALTH_MONITOR_DOMAIN_FS:
		report_fs(pfx, hme);
		break;
	case XFS_HEALTH_MONITOR_DOMAIN_DATADEV:
	case XFS_HEALTH_MONITOR_DOMAIN_RTDEV:
	case XFS_HEALTH_MONITOR_DOMAIN_LOGDEV:
		report_device_error(pfx, hme);
		break;
	case XFS_HEALTH_MONITOR_DOMAIN_FILERANGE:
		report_file_range(pfx, hme);
		break;
	}
}
