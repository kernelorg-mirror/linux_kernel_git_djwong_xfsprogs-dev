// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#ifndef LIBFROG_QUOTACTL_H_
#define LIBFROG_QUOTACTL_H_

#include "xqm.h"

/*
 * System call definitions mapping to platform-specific quotactl
 */
extern int xfsquotactl(int __cmd, const char *__device,
			uint __type, uint __id, void * __addr);
enum {
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

#endif /* LIBFROG_QUOTACTL_H_ */
