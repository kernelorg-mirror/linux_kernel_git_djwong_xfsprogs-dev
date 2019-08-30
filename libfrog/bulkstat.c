// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2019 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include <string.h>
#include <strings.h>
#include "xfs.h"
#include "xfrog.h"
#include "libfrog.h"
#include "bitops.h"

/* Grab fs geometry needed to degrade to v1 bulkstat/inumbers ioctls. */
static inline int
xfrog_bulkstat_prep_v1_emulation(
	struct xfs_fd		*xfd)
{
	if (xfd->fsgeom.blocksize > 0)
		return 0;

	return xfd_prepare_geometry(xfd);
}

/* Bulkstat a single inode.  Returns zero or a positive error code. */
int
xfrog_bulkstat_single(
	struct xfs_fd		*xfd,
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
	int			ret;

	ret = ioctl(xfd->fd, XFS_IOC_FSBULKSTAT_SINGLE, &bulkreq);
	if (ret)
		return errno;
	return 0;
}

/*
 * Set up emulation of a v5 bulk request ioctl with a v1 bulk request ioctl.
 * Returns 0 if the emulation should proceed; ECANCELED if there are no
 * records; or a positive error code.
 */
static int
xfrog_bulk_req_setup(
	struct xfs_fd		*xfd,
	struct xfs_bulk_ireq	*hdr,
	struct xfs_fsop_bulkreq	*bulkreq,
	size_t			rec_size)
{
	void			*buf;

	if (hdr->flags & XFS_BULK_IREQ_AGNO) {
		uint32_t	agno = cvt_ino_to_agno(xfd, hdr->ino);

		if (hdr->ino == 0)
			hdr->ino = cvt_agino_to_ino(xfd, hdr->agno, 0);
		else if (agno < hdr->agno)
			return EINVAL;
		else if (agno > hdr->agno)
			goto no_results;
	}

	if (cvt_ino_to_agno(xfd, hdr->ino) > xfd->fsgeom.agcount)
		goto no_results;

	buf = malloc(hdr->icount * rec_size);
	if (!buf)
		return errno;

	if (hdr->ino)
		hdr->ino--;
	bulkreq->lastip = (__u64 *)&hdr->ino,
	bulkreq->icount = hdr->icount,
	bulkreq->ocount = (__s32 *)&hdr->ocount,
	bulkreq->ubuffer = buf;
	return 0;

no_results:
	hdr->ocount = 0;
	return ECANCELED;
}

/*
 * Convert records and free resources used to do a v1 emulation of v5 bulk
 * request.
 */
static int
xfrog_bulk_req_teardown(
	struct xfs_fd		*xfd,
	struct xfs_bulk_ireq	*hdr,
	struct xfs_fsop_bulkreq	*bulkreq,
	size_t			v1_rec_size,
	uint64_t		(*v1_ino)(void *v1_rec),
	void			*v5_records,
	size_t			v5_rec_size,
	void			(*cvt)(struct xfs_fd *xfd, void *v5, void *v1),
	unsigned int		startino_adj,
	int			error)
{
	void			*v1_rec = bulkreq->ubuffer;
	void			*v5_rec = v5_records;
	unsigned int		i;

	if (error == ECANCELED) {
		error = 0;
		goto free;
	}
	if (error)
		goto free;

	/*
	 * Convert each record from v1 to v5 format, keeping the startino
	 * value up to date and (if desired) stopping at the end of the
	 * AG.
	 */
	for (i = 0;
	     i < hdr->ocount;
	     i++, v1_rec += v1_rec_size, v5_rec += v5_rec_size) {
		uint64_t	ino = v1_ino(v1_rec);

		/* Stop if we hit a different AG. */
		if ((hdr->flags & XFS_BULK_IREQ_AGNO) &&
		    cvt_ino_to_agno(xfd, ino) != hdr->agno) {
			hdr->ocount = i;
			break;
		}
		cvt(xfd, v5_rec, v1_rec);
		hdr->ino = ino + startino_adj;
	}

free:
	free(bulkreq->ubuffer);
	return error;
}

static uint64_t xfrog_bstat_ino(void *v1_rec)
{
	return ((struct xfs_bstat *)v1_rec)->bs_ino;
}

static void xfrog_bstat_cvt(struct xfs_fd *xfd, void *v5, void *v1)
{
	xfrog_bstat_to_bulkstat(xfd, v5, v1);
}

/* Bulkstat a bunch of inodes using the v5 interface. */
static int
xfrog_bulkstat5(
	struct xfs_fd		*xfd,
	struct xfs_bulkstat_req	*req)
{
	int			ret;

	ret = ioctl(xfd->fd, XFS_IOC_BULKSTAT, req);
	if (ret)
		return errno;
	return 0;
}

/* Bulkstat a bunch of inodes using the v1 interface. */
static int
xfrog_bulkstat1(
	struct xfs_fd		*xfd,
	struct xfs_bulkstat_req	*req)
{
	struct xfs_fsop_bulkreq	bulkreq = { 0 };
	int			error;

	error = xfrog_bulkstat_prep_v1_emulation(xfd);
	if (error)
		return error;

	error = xfrog_bulk_req_setup(xfd, &req->hdr, &bulkreq,
			sizeof(struct xfs_bstat));
	if (error == ECANCELED)
		goto out_teardown;
	if (error)
		return error;

	error = ioctl(xfd->fd, XFS_IOC_FSBULKSTAT, &bulkreq);
	if (error)
		error = errno;

out_teardown:
	return xfrog_bulk_req_teardown(xfd, &req->hdr, &bulkreq,
			sizeof(struct xfs_bstat), xfrog_bstat_ino,
			&req->bulkstat, sizeof(struct xfs_bulkstat),
			xfrog_bstat_cvt, 1, error);
}

/* Bulkstat a bunch of inodes.  Returns zero or a positive error code. */
int
xfrog_bulkstat(
	struct xfs_fd		*xfd,
	struct xfs_bulkstat_req	*req)
{
	int			error;

	if (xfd->flags & XFROG_FLAG_BULKSTAT_FORCE_V1)
		goto try_v1;

	error = xfrog_bulkstat5(xfd, req);
	if (error == 0 || (xfd->flags & XFROG_FLAG_BULKSTAT_FORCE_V5))
		return error;

	/* If the v5 ioctl wasn't found, we punt to v1. */
	switch (error) {
	case EOPNOTSUPP:
	case ENOTTY:
		xfd->flags |= XFROG_FLAG_BULKSTAT_FORCE_V1;
		break;
	}

try_v1:
	return xfrog_bulkstat1(xfd, req);
}

/* Convert bulkstat (v5) to bstat (v1). */
void
xfrog_bulkstat_to_bstat(
	struct xfs_fd			*xfd,
	struct xfs_bstat		*bs1,
	const struct xfs_bulkstat	*bstat)
{
	bs1->bs_ino = bstat->bs_ino;
	bs1->bs_mode = bstat->bs_mode;
	bs1->bs_nlink = bstat->bs_nlink;
	bs1->bs_uid = bstat->bs_uid;
	bs1->bs_gid = bstat->bs_gid;
	bs1->bs_rdev = bstat->bs_rdev;
	bs1->bs_blksize = bstat->bs_blksize;
	bs1->bs_size = bstat->bs_size;
	bs1->bs_atime.tv_sec = bstat->bs_atime;
	bs1->bs_mtime.tv_sec = bstat->bs_mtime;
	bs1->bs_ctime.tv_sec = bstat->bs_ctime;
	bs1->bs_atime.tv_nsec = bstat->bs_atime_nsec;
	bs1->bs_mtime.tv_nsec = bstat->bs_mtime_nsec;
	bs1->bs_ctime.tv_nsec = bstat->bs_ctime_nsec;
	bs1->bs_blocks = bstat->bs_blocks;
	bs1->bs_xflags = bstat->bs_xflags;
	bs1->bs_extsize = cvt_off_fsb_to_b(xfd, bstat->bs_extsize_blks);
	bs1->bs_extents = bstat->bs_extents;
	bs1->bs_gen = bstat->bs_gen;
	bs1->bs_projid_lo = bstat->bs_projectid & 0xFFFF;
	bs1->bs_forkoff = bstat->bs_forkoff;
	bs1->bs_projid_hi = bstat->bs_projectid >> 16;
	bs1->bs_sick = bstat->bs_sick;
	bs1->bs_checked = bstat->bs_checked;
	bs1->bs_cowextsize = cvt_off_fsb_to_b(xfd, bstat->bs_cowextsize_blks);
	bs1->bs_dmevmask = 0;
	bs1->bs_dmstate = 0;
	bs1->bs_aextents = bstat->bs_aextents;
}

/* Convert bstat (v1) to bulkstat (v5). */
void
xfrog_bstat_to_bulkstat(
	struct xfs_fd			*xfd,
	struct xfs_bulkstat		*bstat,
	const struct xfs_bstat		*bs1)
{
	memset(bstat, 0, sizeof(*bstat));
	bstat->bs_version = XFS_BULKSTAT_VERSION_V1;

	bstat->bs_ino = bs1->bs_ino;
	bstat->bs_mode = bs1->bs_mode;
	bstat->bs_nlink = bs1->bs_nlink;
	bstat->bs_uid = bs1->bs_uid;
	bstat->bs_gid = bs1->bs_gid;
	bstat->bs_rdev = bs1->bs_rdev;
	bstat->bs_blksize = bs1->bs_blksize;
	bstat->bs_size = bs1->bs_size;
	bstat->bs_atime = bs1->bs_atime.tv_sec;
	bstat->bs_mtime = bs1->bs_mtime.tv_sec;
	bstat->bs_ctime = bs1->bs_ctime.tv_sec;
	bstat->bs_atime_nsec = bs1->bs_atime.tv_nsec;
	bstat->bs_mtime_nsec = bs1->bs_mtime.tv_nsec;
	bstat->bs_ctime_nsec = bs1->bs_ctime.tv_nsec;
	bstat->bs_blocks = bs1->bs_blocks;
	bstat->bs_xflags = bs1->bs_xflags;
	bstat->bs_extsize_blks = cvt_b_to_off_fsbt(xfd, bs1->bs_extsize);
	bstat->bs_extents = bs1->bs_extents;
	bstat->bs_gen = bs1->bs_gen;
	bstat->bs_projectid = bstat_get_projid(bs1);
	bstat->bs_forkoff = bs1->bs_forkoff;
	bstat->bs_sick = bs1->bs_sick;
	bstat->bs_checked = bs1->bs_checked;
	bstat->bs_cowextsize_blks = cvt_b_to_off_fsbt(xfd, bs1->bs_cowextsize);
	bstat->bs_aextents = bs1->bs_aextents;
}

/* Allocate a bulkstat request.  On error returns NULL and sets errno. */
struct xfs_bulkstat_req *
xfrog_bulkstat_alloc_req(
	uint32_t		nr,
	uint64_t		startino)
{
	struct xfs_bulkstat_req	*breq;

	breq = calloc(1, XFS_BULKSTAT_REQ_SIZE(nr));
	if (!breq)
		return NULL;

	breq->hdr.icount = nr;
	breq->hdr.ino = startino;

	return breq;
}

/*
 * Query inode allocation bitmask information.  Returns zero or a positive
 * error code.
 */
int
xfrog_inumbers(
	struct xfs_fd		*xfd,
	uint64_t		*lastino,
	uint32_t		icount,
	struct xfs_inogrp	*ubuffer,
	uint32_t		*ocount)
{
	struct xfs_fsop_bulkreq	bulkreq = {
		.lastip		= (__u64 *)lastino,
		.icount		= icount,
		.ubuffer	= ubuffer,
		.ocount		= (__s32 *)ocount,
	};
	int			ret;

	ret = ioctl(xfd->fd, XFS_IOC_FSINUMBERS, &bulkreq);
	if (ret)
		return errno;
	return 0;
}
