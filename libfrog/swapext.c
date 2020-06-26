// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2020 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <string.h>
#include "xfs.h"
#include "fsgeom.h"
#include "bulkstat.h"
#include "swapext.h"

/* Prepare the freshness component of a swapext request. */
static int
xfrog_swapext_prep_freshness(
	struct xfs_fd		*dest,
	struct file_swap_range	*req)
{
	struct stat		stat;
	struct xfs_bulkstat	bulkstat;
	int			error;

	error = fstat(dest->fd, &stat);
	if (error)
		return -errno;
	req->file2_ino = stat.st_ino;

	/*
	 * Try to fill out the [cm]time data from bulkstat.  We prefer this
	 * approach because bulkstat v5 gives us 64-bit time even on 32-bit.
	 *
	 * However, we'll take our chances on the C library if the filesystem
	 * supports 64-bit time but we ended up with bulkstat v5 emulation.
	 */
	error = xfrog_bulkstat_single(dest, stat.st_ino, 0, &bulkstat);
	if (!error &&
	    !((dest->fsgeom.flags & XFS_FSOP_GEOM_FLAGS_BIGTIME) &&
	      bulkstat.bs_version < XFS_BULKSTAT_VERSION_V5)) {
		req->file2_mtime = bulkstat.bs_mtime;
		req->file2_ctime = bulkstat.bs_ctime;
		req->file2_mtime_nsec = bulkstat.bs_mtime_nsec;
		req->file2_ctime_nsec = bulkstat.bs_ctime_nsec;
		return 0;
	}

	/* Otherwise, use the stat information and hope for the best. */
	req->file2_mtime = stat.st_mtime;
	req->file2_ctime = stat.st_ctime;
	req->file2_mtime_nsec = stat.st_mtim.tv_nsec;
	req->file2_ctime_nsec = stat.st_ctim.tv_nsec;
	return 0;
}

/* Prepare an extent swap request. */
int
xfrog_swapext_prep(
	struct xfs_fd		*dest,
	uint64_t		flags,
	int64_t			file2_offset,
	int			file1_fd,
	int64_t			file1_offset,
	int64_t			length,
	struct file_swap_range	*req)
{
	memset(req, 0, sizeof(*req));
	req->file1_fd = file1_fd;
	req->file1_offset = file1_offset;
	req->length = length;
	req->file2_offset = file2_offset;
	req->flags = flags;

	if (flags & FILE_SWAP_RANGE_FILE2_FRESH)
		return xfrog_swapext_prep_freshness(dest, req);

	return 0;
}

/* Swap two files' extents with the vfs swaprange ioctl. */
static int
xfrog_swapext_vfs(
	struct xfs_fd		*xfd,
	struct file_swap_range	*req)
{
	int			ret;

	ret = ioctl(xfd->fd, FISWAPRANGE, req);
	if (ret) {
		/* the old swapext ioctl returned EFAULT for bad length */
		if (errno == EDOM)
			return -EFAULT;
		return -errno;
	}
	return 0;
}

/* Swap two files' extents with the old xfs swaprange ioctl. */
static int
xfrog_swapext0(
	struct xfs_fd		*xfd,
	struct file_swap_range	*req)
{
	struct xfs_swapext	sx = {
		.sx_version	= XFS_SX_VERSION,
		.sx_fdtarget	= xfd->fd,
		.sx_fdtmp	= req->file1_fd,
		.sx_length	= req->length,
	};
	int			ret;

	if (req->file1_offset != req->file2_offset)
		return -EINVAL;

	/*
	 * The old swapext ioctl did not provide atomic swap; it required that
	 * the supplied offset and length matched both files' lengths; and it
	 * also required that the sx_stat information match the dest file.
	 */
	if (!(req->flags & FILE_SWAP_RANGE_NONATOMIC) ||
	    !(req->flags & FILE_SWAP_RANGE_FULL_FILES) ||
	    !(req->flags & FILE_SWAP_RANGE_FILE2_FRESH))
		return -EOPNOTSUPP;

	sx.sx_stat.bs_ino = req->file2_ino;
	sx.sx_stat.bs_ctime.tv_sec = req->file2_ctime;
	sx.sx_stat.bs_ctime.tv_nsec = req->file2_ctime_nsec;
	sx.sx_stat.bs_mtime.tv_sec = req->file2_mtime;
	sx.sx_stat.bs_mtime.tv_nsec = req->file2_mtime_nsec;

	ret = ioctl(xfd->fd, XFS_IOC_SWAPEXT, &sx);
	if (ret)
		return -errno;
	return 0;
}

/* Swap extents between an XFS file and a donor fd. */
int
xfrog_swapext(
	struct xfs_fd		*xfd,
	struct file_swap_range	*req)
{
	int			error;

	if (xfd->flags & XFROG_FLAG_SWAPEXT_FORCE_V0)
		goto try_v1;

	error = xfrog_swapext_vfs(xfd, req);
	if ((error != -ENOTTY && error != -EOPNOTSUPP) ||
	    (xfd->flags & XFROG_FLAG_SWAPEXT_FORCE_VFS))
		return error;

	/* If the vfs ioctl wasn't found, we punt to v0. */
	switch (error) {
	case -EOPNOTSUPP:
	case -ENOTTY:
		xfd->flags |= XFROG_FLAG_SWAPEXT_FORCE_V0;
		break;
	}

try_v1:
	return xfrog_swapext0(xfd, req);
}
