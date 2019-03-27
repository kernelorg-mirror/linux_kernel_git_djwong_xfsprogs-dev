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
