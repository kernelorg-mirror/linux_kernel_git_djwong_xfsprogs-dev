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

struct fs_path;
int xfrog_quotactl(const struct fs_path *mount, enum xfs_quota_cmd xcommand,
		uint xtype, uint id, void *addr);

#endif /* LIBFROG_QUOTACTL_H_ */
