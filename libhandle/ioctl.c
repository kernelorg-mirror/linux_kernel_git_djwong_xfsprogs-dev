// SPDX-License-Identifier: LGPL-2.1
/*
 * Copyright (C) 2019 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "xfs.h"
#include <string.h>

/* Wrappers for Linux ioctls. */

/* Try to obtain the xfs geometry. */
int
xfs_fsgeometry(
	int			fd,
	struct xfs_fsop_geom	*fsgeo)
{
	int			ret;

	memset(fsgeo, 0, sizeof(*fsgeo));

	ret = ioctl(fd, XFS_IOC_FSGEOMETRY, fsgeo);
	if (!ret)
		return 0;

	return ioctl(fd, XFS_IOC_FSGEOMETRY_V1, fsgeo);
}

/* Bulkstat a single inode. */
int
xfs_bulkstat_single(
	int			fd,
	uint64_t		ino,
	struct xfs_bstat	*ubuffer)
{
	__u64			i = ino;
	struct xfs_fsop_bulkreq	bulkreq = {
		.lastip		= &i,
		.icount		= 1,
		.ubuffer	= ubuffer,
		.ocount		= NULL,
	};

	return ioctl(fd, XFS_IOC_FSBULKSTAT_SINGLE, &bulkreq);
}

/* Bulkstat a bunch of inodes. */
int
xfs_bulkstat(
	int			fd,
	uint64_t		*lastino,
	uint32_t		icount,
	struct xfs_bstat	*ubuffer,
	uint32_t		*ocount)
{
	struct xfs_fsop_bulkreq	bulkreq = {
		.lastip		= (__u64 *)lastino,
		.icount		= icount,
		.ubuffer	= ubuffer,
		.ocount		= (__s32 *)ocount,
	};

	return ioctl(fd, XFS_IOC_FSBULKSTAT, &bulkreq);
}

