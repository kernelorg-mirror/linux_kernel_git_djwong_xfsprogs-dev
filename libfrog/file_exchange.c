// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2020-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <string.h>
#include "xfs.h"
#include "fsgeom.h"
#include "bulkstat.h"
#include "libfrog/file_exchange.h"

/* Prepare for a file contents exchange. */
void
xfrog_exchangerange_prep(
	struct xfs_exchange_range	*fxr,
	off_t				file2_offset,
	int				file1_fd,
	off_t				file1_offset,
	uint64_t			length)
{
	memset(fxr, 0, sizeof(*fxr));

	fxr->file1_fd		= file1_fd;
	fxr->file1_offset	= file1_offset;
	fxr->length		= length;
	fxr->file2_offset	= file2_offset;
}

/*
 * Execute an exchange-range operation.  Returns 0 for success or a negative
 * errno.
 */
int
xfrog_exchangerange(
	int				file2_fd,
	struct xfs_exchange_range	*fxr,
	uint64_t			flags)
{
	int				ret;

	fxr->flags = flags;

	ret = ioctl(file2_fd, XFS_IOC_EXCHANGE_RANGE, fxr);
	if (ret)
		return -errno;

	return 0;
}

/*
 * Prepare for committing a file contents exchange if nobody changes file2 in
 * the meantime.
 *
 * If @file2_stat or @file2_bulkstat Returns 0 for success or a negative errno.
 */
int
xfrog_commitrange_prep(
	struct xfs_commit_range		*xcr,
	struct xfs_fd			*file2,
	off_t				file2_offset,
	int				file1_fd,
	off_t				file1_offset,
	uint64_t			length)
{
	memset(xcr, 0, sizeof(*xcr));

	xcr->file1_fd		= file1_fd;
	xcr->file1_offset	= file1_offset;
	xcr->length		= length;
	xcr->file2_offset	= file2_offset;

	return ioctl(file2->fd, XFS_IOC_START_COMMIT, xcr);
}

/*
 * Execute an exchange-commit operation.  Returns 0 for success or a negative
 * errno.
 */
int
xfrog_commitrange(
	int				file2_fd,
	struct xfs_commit_range		*xcr,
	uint64_t			flags)
{
	int				ret;

	xcr->flags = flags;

	ret = ioctl(file2_fd, XFS_IOC_COMMIT_RANGE, xcr);
	if (ret)
		return -errno;

	return 0;
}

/*
 * Prepare for defragmenting a file by committing a file contents exchange if
 * nobody changes file2 in the meantime.  This can fall back to the old SWAPEXT
 * ioctl if needed.
 */
int
xfrog_defragrange_prep(
	struct xfs_commit_range		*xdf,
	struct xfs_fd			*file2,
	int				file1_fd,
	const struct xfs_bulkstat	*file2_stat)
{
	memset(xdf, 0, sizeof(*xdf));

	xdf->file1_fd		= file1_fd;
	xdf->length		= file2_stat->bs_size;

	return ioctl(file2->fd, XFS_IOC_START_COMMIT, xdf);
}

/* Opaque freshness blob for XFS_IOC_COMMIT_RANGE */
struct xfs_commit_range_fresh {
	__u64		file2_ino;	/* inode number */
	__s64		file2_mtime;	/* modification time */
	__s64		file2_ctime;	/* change time */
	__s32		file2_mtime_nsec; /* mod time, nsec */
	__s32		file2_ctime_nsec; /* change time, nsec */
	__u64		pad;		/* zero */
};

/* Invoke the old swapext ioctl. */
static int
xfrog_ioc_swapext(
	int				file2_fd,
	struct xfs_commit_range		*xdf)
{
	struct xfs_swapext		args = {
		.sx_version		= XFS_SX_VERSION,
		.sx_fdtarget		= xdf->file1_fd,
		.sx_length		= xdf->length,
	};
	struct xfs_commit_range_fresh	*f;
	int				ret;

	BUILD_BUG_ON(sizeof(struct xfs_commit_range_fresh) !=
		     sizeof(xdf->file2_freshness));

	f = (struct xfs_commit_range_fresh *)&xdf->file2_freshness;
	args.sx_stat.bs_ino		= f->file2_ino;
	args.sx_stat.bs_mtime.tv_sec	= f->file2_mtime;
	args.sx_stat.bs_mtime.tv_nsec	= f->file2_mtime_nsec;
	args.sx_stat.bs_ctime.tv_sec	= f->file2_ctime;
	args.sx_stat.bs_ctime.tv_nsec	= f->file2_ctime_nsec;

	ret = ioctl(file2_fd, XFS_IOC_SWAPEXT, &args);
	if (ret) {
		/*
		 * Old swapext returns EFAULT if file1 or file2 length doesn't
		 * match.  The new new COMMIT_RANGE doesn't check the file
		 * length, but the freshness checks will trip and return EBUSY.
		 * If we see EFAULT from the old ioctl, turn that into EBUSY.
		 */
		if (errno == EFAULT)
			return -EBUSY;
		return -errno;
	}

	return 0;
}

/* Execute an exchange operation.  Returns 0 for success or a negative errno. */
int
xfrog_defragrange(
	int			file2_fd,
	struct xfs_commit_range	*xdf)
{
	int			ret;

	ret = ioctl(file2_fd, XFS_IOC_COMMIT_RANGE, xdf);
	if (!ret)
		return 0;

	if (errno != EOPNOTSUPP && errno != ENOTTY)
		return -errno;

	return xfrog_ioc_swapext(file2_fd, xdf);
}
