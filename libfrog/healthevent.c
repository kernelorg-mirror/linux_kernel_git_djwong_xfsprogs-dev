// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025-2026 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"

#include "platform_defs.h"
#include "libfrog/healthevent.h"
#include "libfrog/flagmap.h"

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

static const struct flag_map device_domains[] = {
	{ XFS_HEALTH_MONITOR_DOMAIN_DATADEV,	N_("datadev") },
	{ XFS_HEALTH_MONITOR_DOMAIN_RTDEV,	N_("rtdev") },
	{ XFS_HEALTH_MONITOR_DOMAIN_LOGDEV,	N_("logdev") },
	{0, NULL},
};

static inline const char *
device_domain_string(
	uint32_t		domain)
{
	return value_to_string(device_domains, domain);
}

static const struct flag_map fileio_types[] = {
	{ XFS_HEALTH_MONITOR_TYPE_BUFREAD,	N_("buffered_read") },
	{ XFS_HEALTH_MONITOR_TYPE_BUFWRITE,	N_("buffered_write") },
	{ XFS_HEALTH_MONITOR_TYPE_DIOREAD,	N_("directio_read") },
	{ XFS_HEALTH_MONITOR_TYPE_DIOWRITE,	N_("directio_write") },
	{ XFS_HEALTH_MONITOR_TYPE_DATALOST,	N_("media") },
	{0, NULL},
};

static inline const char *
fileio_type_string(
	uint32_t		type)
{
	return value_to_string(fileio_types, type);
}

static const struct flag_map health_types[] = {
	{ XFS_HEALTH_MONITOR_TYPE_SICK,		N_("sick") },
	{ XFS_HEALTH_MONITOR_TYPE_CORRUPT,	N_("corrupt") },
	{ XFS_HEALTH_MONITOR_TYPE_HEALTHY,	N_("healthy") },
	{0, NULL},
};

static inline const char *
health_type_string(
	uint32_t		type)
{
	return value_to_string(health_types, type);
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

static const struct flag_map shutdown_reasons[] = {
	{ XFS_HEALTH_SHUTDOWN_META_IO_ERROR,	N_("metadata I/O error") },
	{ XFS_HEALTH_SHUTDOWN_LOG_IO_ERROR,	N_("log I/O error") },
	{ XFS_HEALTH_SHUTDOWN_FORCE_UMOUNT,	N_("forced unmount") },
	{ XFS_HEALTH_SHUTDOWN_CORRUPT_INCORE,	N_("in-memory state corruption") },
	{ XFS_HEALTH_SHUTDOWN_CORRUPT_ONDISK,	N_("ondisk metadata corruption") },
	{ XFS_HEALTH_SHUTDOWN_DEVICE_REMOVED,	N_("device removed") },
	{0, NULL},
};

/* Report an abortive shutdown of the filesystem. */
static void
report_shutdown(
	const struct hme_prefix			*pfx,
	const struct xfs_health_monitor_event	*hme)
{
	char					buf[512];

	mask_to_string(shutdown_reasons, hme->e.shutdown.reasons, ", ", buf,
			sizeof(buf));

	printf("%s: %s %s\n", pfx->mountpoint,
			_("filesystem shut down due to"), buf);
	fflush(stdout);
}

static const struct flag_map inode_structs[] = {
	{ XFS_BS_SICK_INODE,	N_("core") },
	{ XFS_BS_SICK_BMBTD,	N_("datafork") },
	{ XFS_BS_SICK_BMBTA,	N_("attrfork") },
	{ XFS_BS_SICK_BMBTC,	N_("cowfork") },
	{ XFS_BS_SICK_DIR,	N_("directory") },
	{ XFS_BS_SICK_XATTR,	N_("xattr") },
	{ XFS_BS_SICK_SYMLINK,	N_("symlink") },
	{ XFS_BS_SICK_PARENT,	N_("parent") },
	{ XFS_BS_SICK_DIRTREE,	N_("dirtree") },
	{0, NULL},
};

/* Report inode metadata corruption */
static void
report_inode(
	const struct hme_prefix			*pfx,
	const struct xfs_health_monitor_event	*hme)
{
	char					buf[512];

	mask_to_string(inode_structs, hme->e.inode.mask, ", ", buf,
			sizeof(buf));

	if (hme_prefix_has_path(pfx))
		printf("%s %s: %s\n",
				pfx->path,
				buf,
				health_type_string(hme->type));
	else
		printf("%s %s %llu %s 0x%x %s: %s\n",
				pfx->mountpoint,
				_("ino"),
				(unsigned long long)hme->e.inode.ino,
				_("gen"),
				hme->e.inode.gen,
				buf,
				health_type_string(hme->type));
	fflush(stdout);
}

static const struct flag_map ag_structs[] = {
	{ XFS_AG_GEOM_SICK_SB,		N_("super") },
	{ XFS_AG_GEOM_SICK_AGF,		N_("agf") },
	{ XFS_AG_GEOM_SICK_AGFL,	N_("agfl") },
	{ XFS_AG_GEOM_SICK_AGI,		N_("agi") },
	{ XFS_AG_GEOM_SICK_BNOBT,	N_("bnobt") },
	{ XFS_AG_GEOM_SICK_CNTBT,	N_("cntbt") },
	{ XFS_AG_GEOM_SICK_INOBT,	N_("inobt") },
	{ XFS_AG_GEOM_SICK_FINOBT,	N_("finobt") },
	{ XFS_AG_GEOM_SICK_RMAPBT,	N_("rmapbt") },
	{ XFS_AG_GEOM_SICK_REFCNTBT,	N_("refcountbt") },
	{ XFS_AG_GEOM_SICK_INODES,	N_("inodes") },
	{0, NULL},
};

/* Report AG metadata corruption */
static void
report_ag(
	const struct hme_prefix			*pfx,
	const struct xfs_health_monitor_event	*hme)
{
	char					buf[512];

	mask_to_string(ag_structs, hme->e.group.mask, ", ", buf,
			sizeof(buf));

	printf("%s %s 0x%x %s: %s\n",
			pfx->mountpoint,
			_("agno"),
			hme->e.group.gno,
			buf,
			health_type_string(hme->type));
	fflush(stdout);
}

static const struct flag_map rtgroup_structs[] = {
	{ XFS_RTGROUP_GEOM_SICK_SUPER,		N_("super") },
	{ XFS_RTGROUP_GEOM_SICK_BITMAP,		N_("bitmap") },
	{ XFS_RTGROUP_GEOM_SICK_SUMMARY,	N_("summary") },
	{ XFS_RTGROUP_GEOM_SICK_RMAPBT,		N_("rmapbt") },
	{ XFS_RTGROUP_GEOM_SICK_REFCNTBT,	N_("refcountbt") },
	{0, NULL},
};

/* Report rtgroup metadata corruption */
static void
report_rtgroup(
	const struct hme_prefix			*pfx,
	const struct xfs_health_monitor_event	*hme)
{
	char					buf[512];

	mask_to_string(rtgroup_structs, hme->e.group.mask, ", ", buf,
			sizeof(buf));

	printf("%s %s 0x%x %s: %s\n",
			pfx->mountpoint,
			_("rgno"),
			hme->e.group.gno,
			buf, health_type_string(hme->type));
	fflush(stdout);
}

static const struct flag_map fs_structs[] = {
	{ XFS_FSOP_GEOM_SICK_COUNTERS,		N_("fscounters") },
	{ XFS_FSOP_GEOM_SICK_UQUOTA,		N_("usrquota") },
	{ XFS_FSOP_GEOM_SICK_GQUOTA,		N_("grpquota") },
	{ XFS_FSOP_GEOM_SICK_PQUOTA,		N_("prjquota") },
	{ XFS_FSOP_GEOM_SICK_RT_BITMAP,		N_("bitmap") },
	{ XFS_FSOP_GEOM_SICK_RT_SUMMARY,	N_("summary") },
	{ XFS_FSOP_GEOM_SICK_QUOTACHECK,	N_("quotacheck") },
	{ XFS_FSOP_GEOM_SICK_NLINKS,		N_("nlinks") },
	{ XFS_FSOP_GEOM_SICK_METADIR,		N_("metadir") },
	{ XFS_FSOP_GEOM_SICK_METAPATH,		N_("metapath") },
	{0, NULL},
};

/* Report fs-wide metadata corruption */
static void
report_fs(
	const struct hme_prefix			*pfx,
	const struct xfs_health_monitor_event	*hme)
{
	char					buf[512];

	mask_to_string(fs_structs, hme->e.fs.mask, ", ", buf, sizeof(buf));

	printf("%s %s: %s\n",
			pfx->mountpoint,
			buf,
			health_type_string(hme->type));
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

static const char *
repair_outcome_string(
	enum repair_outcome	o)
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

/* Report inode metadata repair */
static void
report_inode_repair(
	const struct hme_prefix			*pfx,
	const struct xfs_health_monitor_event	*hme,
	uint32_t				domain_mask,
	enum repair_outcome			outcome)
{
	if (hme_prefix_has_path(pfx))
		printf("%s %s: %s\n",
				pfx->path,
				lowest_set_mask_string(inode_structs,
						       domain_mask),
				repair_outcome_string(outcome));
	else
		printf("%s %s %llu %s 0x%x %s: %s\n",
				pfx->mountpoint,
				_("ino"),
				(unsigned long long)hme->e.inode.ino,
				_("gen"),
				hme->e.inode.gen,
				lowest_set_mask_string(inode_structs,
						       domain_mask),
				repair_outcome_string(outcome));
	fflush(stdout);
}

/* Report AG metadata repair */
static void
report_ag_repair(
	const struct hme_prefix			*pfx,
	const struct xfs_health_monitor_event	*hme,
	uint32_t				domain_mask,
	enum repair_outcome			outcome)
{
	printf("%s %s 0x%x %s: %s\n", pfx->mountpoint,
			_("agno"),
			hme->e.group.gno,
			lowest_set_mask_string(ag_structs, domain_mask),
			repair_outcome_string(outcome));
	fflush(stdout);
}

/* Report rtgroup metadata repair */
static void
report_rtgroup_repair(
	const struct hme_prefix			*pfx,
	const struct xfs_health_monitor_event	*hme,
	uint32_t				domain_mask,
	enum repair_outcome			outcome)
{
	printf("%s %s 0x%x %s: %s\n", pfx->mountpoint,
			_("rgno"),
			hme->e.group.gno,
			lowest_set_mask_string(rtgroup_structs, domain_mask),
			repair_outcome_string(outcome));
	fflush(stdout);
}

/* Report fs-wide metadata repair */
static void
report_fs_repair(
	const struct hme_prefix			*pfx,
	const struct xfs_health_monitor_event	*hme,
	uint32_t				domain_mask,
	enum repair_outcome			outcome)
{
	printf("%s %s: %s\n", pfx->mountpoint,
			lowest_set_mask_string(fs_structs, domain_mask),
			repair_outcome_string(outcome));
	fflush(stdout);
}

/* Log a repair event to stdout. */
void
report_health_repair(
	const struct hme_prefix			*pfx,
	const struct xfs_health_monitor_event	*hme,
	uint32_t				domain_mask,
	enum repair_outcome			outcome)
{
	switch (hme->domain) {
	case XFS_HEALTH_MONITOR_DOMAIN_INODE:
		report_inode_repair(pfx, hme, domain_mask, outcome);
		break;
	case XFS_HEALTH_MONITOR_DOMAIN_AG:
		report_ag_repair(pfx, hme, domain_mask, outcome);
		break;
	case XFS_HEALTH_MONITOR_DOMAIN_RTGROUP:
		report_rtgroup_repair(pfx, hme, domain_mask, outcome);
		break;
	case XFS_HEALTH_MONITOR_DOMAIN_FS:
		report_fs_repair(pfx, hme, domain_mask, outcome);
		break;
	default:
		break;
	}
}
