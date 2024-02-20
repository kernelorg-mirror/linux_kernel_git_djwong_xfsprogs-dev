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

/* Can we use struct stat to provide freshness data for commitrange? */
static inline bool
use_stat_for_commitrange(
	const struct stat		*file2_stat)
{
	/* Caller didn't provide a stat buffer; find our own. */
	if (!file2_stat)
		return false;

	/* Don't trust a caller-provided stat buffer with a 32-bit inumber */
	if (sizeof(file2_stat->st_ino) < sizeof(xfs_ino_t))
		return false;

	/* Don't trust 32-bit time now that xfs supports post-2038 dates */
	if (sizeof(file2_stat->st_mtime) < sizeof(uint64_t))
		return false;

	return true;
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
	int64_t				file2_offset,
	int				file1_fd,
	int64_t				file1_offset,
	int64_t				length,
	const struct stat		*file2_stat,
	const struct xfs_bulkstat	*file2_bulkstat)
{
	struct xfs_bulkstat		bstat;
	struct stat			stat;
	int				ret;

	memset(xcr, 0, sizeof(*xcr));

	xcr->file1_fd		= file1_fd;
	xcr->file1_offset	= file1_offset;
	xcr->length		= length;
	xcr->file2_offset	= file2_offset;

	/*
	 * If the caller passed in a bulkstat buffer for file2, use that as the
	 * freshness data.  The fields are always wide enough for commitrange.
	 */
	if (file2_bulkstat) {
		xcr->file2_ino		= file2_bulkstat->bs_ino;
		xcr->file2_mtime	= file2_bulkstat->bs_mtime;
		xcr->file2_ctime	= file2_bulkstat->bs_ctime;
		xcr->file2_mtime_nsec	= file2_bulkstat->bs_mtime_nsec;
		xcr->file2_ctime_nsec	= file2_bulkstat->bs_ctime_nsec;
		return 0;
	}

	/* Try using the caller's stat buf? */
	if (use_stat_for_commitrange(file2_stat)) {
		xcr->file2_ino		= file2_stat->st_ino;
		xcr->file2_mtime	= file2_stat->st_mtime;
		xcr->file2_ctime	= file2_stat->st_ctime;
		xcr->file2_mtime_nsec	= file2_stat->st_mtim.tv_nsec;
		xcr->file2_ctime_nsec	= file2_stat->st_ctim.tv_nsec;
		return 0;
	}

	/*
	 * Try to stat the open file.  If that fails due to integer overflow,
	 * then we have an inumber or a timestamp that cannot be expressed via
	 * the stat structure and we cannot support the commitrange operation.
	 */
	ret = fstat(file2->fd, &stat);
	if (ret) {
		if (errno == EOVERFLOW)
			return -EOPNOTSUPP;
		return -errno;
	}

	/* Try using the sampled stat buf? */
	if (use_stat_for_commitrange(&stat)) {
		xcr->file2_ino		= stat.st_ino;
		xcr->file2_mtime	= stat.st_mtime;
		xcr->file2_ctime	= stat.st_ctime;
		xcr->file2_mtime_nsec	= stat.st_mtim.tv_nsec;
		xcr->file2_ctime_nsec	= stat.st_ctim.tv_nsec;
		return 0;
	}

	ret = xfrog_bulkstat_single(file2, stat.st_ino, 0, &bstat);
	if (!ret) {
		/*
		 * If BULKSTAT_SINGLE worked, use that for the freshness data.
		 * bs_ino and the timestamps are guaranteed to be a u64 here.
		 */
		xcr->file2_ino		= bstat.bs_ino;
		xcr->file2_mtime	= bstat.bs_mtime;
		xcr->file2_ctime	= bstat.bs_ctime;
		xcr->file2_mtime_nsec	= bstat.bs_mtime_nsec;
		xcr->file2_ctime_nsec	= bstat.bs_ctime_nsec;
		return 0;
	}

	/*
	 * Nothing else worked for freshness, so we cannot support this
	 * operation.  This should never happen since we define both
	 * _FILE_OFFSET_BITS=64 and _TIME_BITS=64.
	 */
	return -EOPNOTSUPP;
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
void
xfrog_defragrange_prep(
	struct xfs_commit_range		*xdf,
	int				file1_fd,
	const struct xfs_bulkstat	*file2_stat)
{
	memset(xdf, 0, sizeof(*xdf));

	xdf->file1_fd		= file1_fd;
	xdf->length		= file2_stat->bs_size;

	xdf->file2_ino		= file2_stat->bs_ino;
	xdf->file2_mtime	= file2_stat->bs_mtime;
	xdf->file2_ctime	= file2_stat->bs_ctime;
	xdf->file2_mtime_nsec	= file2_stat->bs_mtime_nsec;
	xdf->file2_ctime_nsec	= file2_stat->bs_ctime_nsec;
}

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
		.sx_stat		= {
			.bs_ino		= xdf->file2_ino,
			.bs_ctime.tv_sec  = xdf->file2_ctime,
			.bs_ctime.tv_nsec = xdf->file2_ctime_nsec,
			.bs_mtime.tv_sec  = xdf->file2_mtime,
			.bs_mtime.tv_nsec = xdf->file2_mtime_nsec,
		},
	};
	int				ret;

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

/* Prepare for a file contents exchange. */
void
xfrog_exchrange_prep(
	struct xfs_exch_range	*fxr,
	int64_t			file2_offset,
	int			file1_fd,
	int64_t			file1_offset,
	int64_t			length)
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
xfrog_exchrange(
	int			file2_fd,
	struct xfs_exch_range	*fxr,
	uint64_t		flags)
{
	int			ret;

	fxr->flags = flags;

	ret = ioctl(file2_fd, XFS_IOC_EXCHANGE_RANGE, fxr);
	if (ret)
		return -errno;

	return 0;
}
