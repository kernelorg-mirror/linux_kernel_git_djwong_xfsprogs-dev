// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#ifndef LIBFROG_QUOTACTL_H_
#define LIBFROG_QUOTACTL_H_

#include "xqm.h"

enum xfs_quota_cmd {
	XFS_QUOTAON,	/* enable accounting/enforcement */
	XFS_QUOTAOFF,	/* disable accounting/enforcement */
	XFS_GETQUOTA,	/* get disk limits and usage */
	XFS_SETQLIM,	/* set disk limits */
	XFS_GETQSTAT,	/* get quota subsystem status */
	XFS_QUOTARM,	/* free disk space used by dquots */
	XFS_QSYNC,	/* flush delayed allocate space */
	XFS_GETQSTATV,	/* newer version of quota stats */
	XFS_GETNEXTQUOTA, /* get disk limits and usage */
};

int xfsquotactl(int mnt_fd, const char *device, enum xfs_quota_cmd xcommand,
		unsigned int xtype, unsigned int id, void *addr);

static inline int xfssetqlim(const char *device, uint xtype, uint id,
		struct fs_disk_quota *addr)
{
	return xfsquotactl(-1, device, XFS_SETQLIM, xtype, id, addr);
}

struct fs_path;
int xfrog_quotactl(const struct fs_path *mount, enum xfs_quota_cmd xcommand,
		uint xtype, uint id, void *addr);

static inline int xfrog_getquota(const struct fs_path *mount, uint xtype,
		uint id, struct fs_disk_quota *addr)
{
	return xfrog_quotactl(mount, XFS_GETQUOTA, xtype, id, addr);
}

static inline int xfrog_getnextquota(const struct fs_path *mount, uint xtype,
		uint id, struct fs_disk_quota *addr)
{
	return xfrog_quotactl(mount, XFS_GETNEXTQUOTA, xtype, id, addr);
}

static inline int xfrog_setqlim(const struct fs_path *mount, uint xtype,
		uint id, struct fs_disk_quota *addr)
{
	return xfrog_quotactl(mount, XFS_SETQLIM, xtype, id, addr);
}

static inline int xfrog_getqstat(const struct fs_path *mount, uint xtype,
		uint id, struct fs_quota_stat *addr)
{
	return xfrog_quotactl(mount, XFS_GETQSTAT, xtype, id, addr);
}

static inline int xfrog_getqstatv(const struct fs_path *mount, uint xtype,
		uint id, struct fs_quota_statv *addr)
{
	return xfrog_quotactl(mount, XFS_GETQSTATV, xtype, id, addr);
}

static inline int xfrog_qsync(const struct fs_path *mount, uint xtype,
		uint id)
{
	return xfrog_quotactl(mount, XFS_QSYNC, xtype, id, NULL);
}

static inline int xfrog_quotaon(const struct fs_path *mount, uint xtype,
		uint id, uint *qflags)
{
	return xfrog_quotactl(mount, XFS_QUOTAON, xtype, id, qflags);
}

static inline int xfrog_quotaoff(const struct fs_path *mount, uint xtype,
		uint id, uint *qflags)
{
	return xfrog_quotactl(mount, XFS_QUOTAOFF, xtype, id, qflags);
}

static inline int xfrog_quotarm(const struct fs_path *mount, uint xtype,
		uint id, uint *type)
{
	return xfrog_quotactl(mount, XFS_QUOTARM, xtype, id, type);
}

#endif /* LIBFROG_QUOTACTL_H_ */
