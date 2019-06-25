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
	if (xfd->fsgeom.blocksize == 0 && xfrog_prepare_geometry(xfd))
		return -1;
	return 0;
}

/* Bulkstat a single inode using v5 ioctl. */
static int
xfrog_bulkstat_single5(
	struct xfs_fd			*xfd,
	struct xfs_bulkstat_single_req	*req)
{
	return ioctl(xfd->fd, XFS_IOC_BULKSTAT_SINGLE, req);
}

/* Bulkstat a single inode using v1 ioctl. */
static int
xfrog_bulkstat_single1(
	struct xfs_fd			*xfd,
	struct xfs_bulkstat_single_req	*req)
{
	struct xfs_bstat		bstat;
	struct xfs_fsop_bulkreq		bulkreq = { 0 };
	int				error;

	/* Old bulkstat_single doesn't do special inodes. */
	if (req->hdr.flags) {
		errno = EOPNOTSUPP;
		return -1;
	}

	error = xfrog_bulkstat_prep_v1_emulation(xfd);
	if (error)
		return error;

	bulkreq.lastip = (__u64 *)&req->hdr.ino,
	bulkreq.icount = 1;
	bulkreq.ubuffer = &bstat;
	error = ioctl(xfd->fd, XFS_IOC_FSBULKSTAT_SINGLE, &bulkreq);
	if (error)
		return error;

	xfrog_bstat_to_bulkstat(xfd, &req->bulkstat, &bstat);
	return 0;
}

/* Bulkstat a single inode using v1 ioctl. */
int
xfrog_bulkstat_single(
	struct xfs_fd			*xfd,
	struct xfs_bulkstat_single_req	*req)
{
	int				error;

	if (xfd->flags & XFROG_FLAG_BULKSTAT_FORCE_V1)
		goto try_v1;

	error = xfrog_bulkstat_single5(xfd, req);
	if (error == 0 || (xfd->flags & XFROG_FLAG_BULKSTAT_FORCE_V5))
		return 0;

	/* If the v5 ioctl wasn't found, we punt to v1. */
	switch (errno) {
	case EOPNOTSUPP:
	case ENOTTY:
		xfd->flags |= XFROG_FLAG_BULKSTAT_FORCE_V1;
		break;
	}

try_v1:
	return xfrog_bulkstat_single1(xfd, req);
}

/*
 * Set up emulation of a v5 bulk request ioctl with a v1 bulk request ioctl.
 * Returns 0 if the emulation should proceed; XFROG_ITER_ABORT if there are no
 * records; or -1 for error.
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
		uint32_t	agno = xfrog_ino_to_agno(xfd, hdr->ino);

		if (hdr->ino == 0)
			hdr->ino = xfrog_agino_to_ino(xfd, hdr->agno, 0);
		else if (agno < hdr->agno) {
			errno = EINVAL;
			return -1;
		} else if (agno > hdr->agno)
			goto no_results;
	}

	if (xfrog_ino_to_agno(xfd, hdr->ino) > xfd->fsgeom.agcount)
		goto no_results;

	buf = malloc(hdr->icount * rec_size);
	if (!buf)
		return -1;

	if (hdr->ino)
		hdr->ino--;
	bulkreq->lastip = (__u64 *)&hdr->ino,
	bulkreq->icount = hdr->icount,
	bulkreq->ocount = (__s32 *)&hdr->ocount,
	bulkreq->ubuffer = buf;
	return 0;

no_results:
	hdr->ocount = 0;
	return XFROG_ITER_ABORT;
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

	if (error == XFROG_ITER_ABORT) {
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
		    xfrog_ino_to_agno(xfd, ino) != hdr->agno) {
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
	return ioctl(xfd->fd, XFS_IOC_BULKSTAT, req);
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
	if (error == XFROG_ITER_ABORT)
		goto out_teardown;
	if (error < 0)
		return error;

	error = ioctl(xfd->fd, XFS_IOC_FSBULKSTAT, &bulkreq);

out_teardown:
	return xfrog_bulk_req_teardown(xfd, &req->hdr, &bulkreq,
			sizeof(struct xfs_bstat), xfrog_bstat_ino,
			&req->bulkstat, sizeof(struct xfs_bulkstat),
			xfrog_bstat_cvt, 1, error);
}

/* Bulkstat a bunch of inodes. */
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
	switch (errno) {
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
	bs1->bs_extsize = xfrog_fsb_to_b(xfd, bstat->bs_extsize_blks);
	bs1->bs_extents = bstat->bs_extents;
	bs1->bs_gen = bstat->bs_gen;
	bs1->bs_projid_lo = bstat->bs_projectid & 0xFFFF;
	bs1->bs_forkoff = bstat->bs_forkoff;
	bs1->bs_projid_hi = bstat->bs_projectid >> 16;
	bs1->bs_sick = bstat->bs_sick;
	bs1->bs_checked = bstat->bs_checked;
	bs1->bs_cowextsize = xfrog_fsb_to_b(xfd, bstat->bs_cowextsize_blks);
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
	bstat->bs_extsize_blks = xfrog_b_to_fsbt(xfd, bs1->bs_extsize);
	bstat->bs_extents = bs1->bs_extents;
	bstat->bs_gen = bs1->bs_gen;
	bstat->bs_projectid = bstat_get_projid(bs1);
	bstat->bs_forkoff = bs1->bs_forkoff;
	bstat->bs_sick = bs1->bs_sick;
	bstat->bs_checked = bs1->bs_checked;
	bstat->bs_cowextsize_blks = xfrog_b_to_fsbt(xfd, bs1->bs_cowextsize);
	bstat->bs_aextents = bs1->bs_aextents;
}

/* Allocate a bulkstat request. */
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

/* Convert an inumbers (v5) struct to a inogrp (v1) struct. */
void
xfrog_inumbers_to_inogrp(
	struct xfs_inogrp		*ig1,
	const struct xfs_inumbers	*ig)
{
	ig1->xi_startino = ig->xi_startino;
	ig1->xi_alloccount = ig->xi_alloccount;
	ig1->xi_allocmask = ig->xi_allocmask;
}

/* Convert an inogrp (v1) struct to a inumbers (v5) struct. */
void
xfrog_inogrp_to_inumbers(
	struct xfs_inumbers		*ig,
	const struct xfs_inogrp		*ig1)
{
	memset(ig, 0, sizeof(*ig));
	ig->xi_version = XFS_INUMBERS_VERSION_V1;

	ig->xi_startino = ig1->xi_startino;
	ig->xi_alloccount = ig1->xi_alloccount;
	ig->xi_allocmask = ig1->xi_allocmask;
}

static uint64_t xfrog_inum_ino(void *v1_rec)
{
	return ((struct xfs_inogrp *)v1_rec)->xi_startino;
}

static void xfrog_inum_cvt(struct xfs_fd *xfd, void *v5, void *v1)
{
	xfrog_inogrp_to_inumbers(v5, v1);
}

/* Query inode allocation bitmask information using v5 ioctl. */
static int
xfrog_inumbers5(
	struct xfs_fd		*xfd,
	struct xfs_inumbers_req	*req)
{
	return ioctl(xfd->fd, XFS_IOC_INUMBERS, req);
}

/* Query inode allocation bitmask information using v1 ioctl. */
static int
xfrog_inumbers1(
	struct xfs_fd		*xfd,
	struct xfs_inumbers_req	*req)
{
	struct xfs_fsop_bulkreq	bulkreq;
	int			error;

	error = xfrog_bulkstat_prep_v1_emulation(xfd);
	if (error)
		return error;

	error = xfrog_bulk_req_setup(xfd, &req->hdr, &bulkreq,
			sizeof(struct xfs_inogrp));
	if (error == XFROG_ITER_ABORT)
		goto out_teardown;
	if (error < 0)
		return error;

	error = ioctl(xfd->fd, XFS_IOC_FSINUMBERS, &bulkreq);

out_teardown:
	return xfrog_bulk_req_teardown(xfd, &req->hdr, &bulkreq,
			sizeof(struct xfs_inogrp), xfrog_inum_ino,
			&req->inumbers, sizeof(struct xfs_inumbers),
			xfrog_inum_cvt, 64, error);
}

/* Query inode allocation bitmask information. */
int
xfrog_inumbers(
	struct xfs_fd		*xfd,
	struct xfs_inumbers_req	*req)
{
	int			error;

	if (xfd->flags & XFROG_FLAG_BULKSTAT_FORCE_V1)
		goto try_v1;

	error = xfrog_inumbers5(xfd, req);
	if (error == 0 || (xfd->flags & XFROG_FLAG_BULKSTAT_FORCE_V5))
		return 0;

	/* If the v5 ioctl wasn't found, we punt to v1. */
	switch (errno) {
	case EOPNOTSUPP:
	case ENOTTY:
		xfd->flags |= XFROG_FLAG_BULKSTAT_FORCE_V1;
		break;
	}

try_v1:
	return xfrog_inumbers1(xfd, req);
}

/* Allocate a inumbers request. */
struct xfs_inumbers_req *
xfrog_inumbers_alloc_req(
	uint32_t		nr,
	uint64_t		startino)
{
	struct xfs_inumbers_req	*ireq;

	ireq = calloc(1, XFS_INUMBERS_REQ_SIZE(nr));
	if (!ireq)
		return NULL;

	ireq->hdr.icount = nr;
	ireq->hdr.ino = startino;

	return ireq;
}
