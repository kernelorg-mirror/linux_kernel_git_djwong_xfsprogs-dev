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

/* Bulkstat a single inode. */
int
xfrog_bulkstat_single(
	int				fd,
	struct xfs_fsop_geom		*fsgeom,
	struct xfs_bulkstat_single_req	*req)
{
	struct xfs_fsop_geom		geom;
	struct xfs_bstat		bstat;
	struct xfs_fsop_bulkreq		bulkreq = { 0 };
	int				error;

	error = ioctl(fd, XFS_IOC_BULKSTAT_SINGLE, req);
	if (!error)
		return 0;

	/* Emulate the v5 call as best we can with v1. */

	/* Old bulkstat_single doesn't do special inodes. */
	if (req->hdr.flags) {
		errno = EOPNOTSUPP;
		return -1;
	}

	if (!fsgeom) {
		if (xfrog_geometry(fd, &geom))
			return -1;
		fsgeom = &geom;
	}

	bulkreq.lastip = (__u64 *)&req->hdr.ino,
	bulkreq.icount = 1;
	bulkreq.ubuffer = &bstat;
	error = ioctl(fd, XFS_IOC_FSBULKSTAT_SINGLE, &bulkreq);
	if (error)
		return error;

	xfrog_bstat_to_bulkstat(fsgeom, &req->bulkstat, &bstat);
	return 0;
}

static inline int
xfrog_ino_agino_bits(
	struct xfs_fsop_geom	*geom)
{
	int agblklog = log2_roundup(geom->agblocks);
	int blocklog = highbit32(geom->blocksize);
	int inodelog = highbit32(geom->inodesize);
	int inopblog = blocklog - inodelog;

	return agblklog + inopblog;
}

static inline uint64_t
xfrog_agino_to_ino(
	struct xfs_fsop_geom	*geom,
	uint32_t		agno,
	uint32_t		agino)
{
	return ((uint64_t)agno << xfrog_ino_agino_bits(geom)) + agino;
}

static inline uint32_t
xfrog_ino_to_agno(
	struct xfs_fsop_geom	*geom,
	uint64_t		ino)
{
	return ino >> xfrog_ino_agino_bits(geom);
}

/*
 * Set up emulation of a v5 bulk request ioctl with a v1 bulk request ioctl.
 * Returns 0 if the emulation should proceed; 1 if there are no records; or
 * -1 for error.
 */
static int
xfrog_bulk_req_setup(
	struct xfs_fsop_geom	*fsgeom,
	struct xfs_bulk_ireq	*hdr,
	struct xfs_fsop_bulkreq	*bulkreq,
	size_t			rec_size)
{
	void			*buf;

	if (hdr->flags & XFS_BULK_IREQ_AGNO) {
		uint32_t	agno = xfrog_ino_to_agno(fsgeom, hdr->ino);

		if (hdr->ino == 0)
			hdr->ino = xfrog_agino_to_ino(fsgeom, hdr->agno, 0);
		else if (agno < hdr->agno) {
			errno = EINVAL;
			return -1;
		} else if (agno > hdr->agno)
			goto no_results;
	}

	if (xfrog_ino_to_agno(fsgeom, hdr->ino) > fsgeom->agcount)
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
	return 1;
}

/*
 * Convert records and free resources used to do a v1 emulation of v5 bulk
 * request.
 */
static int
xfrog_bulk_req_teardown(
	struct xfs_fsop_geom	*fsgeom,
	struct xfs_bulk_ireq	*hdr,
	struct xfs_fsop_bulkreq	*bulkreq,
	size_t			v1_rec_size,
	uint64_t		(*v1_ino)(void *v1_rec),
	void			*v5_records,
	size_t			v5_rec_size,
	void			(*cvt)(struct xfs_fsop_geom *fsgeom,
				       void *v5, void *v1),
	unsigned int		startino_adj,
	int			error)
{
	void			*v1_rec = bulkreq->ubuffer;
	void			*v5_rec = v5_records;
	unsigned int		i;

	if (error == 1) {
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
		    xfrog_ino_to_agno(fsgeom, ino) != hdr->agno) {
			hdr->ocount = i;
			break;
		}
		cvt(fsgeom, v5_rec, v1_rec);
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

static void xfrog_bstat_cvt(struct xfs_fsop_geom *fsgeom, void *v5, void *v1)
{
	xfrog_bstat_to_bulkstat(fsgeom, v5, v1);
}

/* Bulkstat a bunch of inodes. */
int
xfrog_bulkstat(
	int			fd,
	struct xfs_fsop_geom	*fsgeom,
	struct xfs_bulkstat_req	*req)
{
	struct xfs_fsop_geom	geo;
	struct xfs_fsop_bulkreq	bulkreq = { 0 };
	int			error;

	error = ioctl(fd, XFS_IOC_BULKSTAT, req);
	if (!error)
		return 0;

	/* Emulate the v5 call as best we can with v1. */
	if (!fsgeom) {
		if (xfrog_geometry(fd, &geo))
			return -1;
		fsgeom = &geo;
	}

	error = xfrog_bulk_req_setup(fsgeom, &req->hdr, &bulkreq,
			sizeof(struct xfs_bstat));
	if (error < 0)
		return error;

	if (!error)
		error = ioctl(fd, XFS_IOC_FSBULKSTAT, &bulkreq);

	return xfrog_bulk_req_teardown(fsgeom, &req->hdr, &bulkreq,
			sizeof(struct xfs_bstat), xfrog_bstat_ino,
			&req->bulkstat, sizeof(struct xfs_bulkstat),
			xfrog_bstat_cvt, 1, error);
}

/* Convert bulkstat (v5) to bstat (v1). */
void
xfrog_bulkstat_to_bstat(
	const struct xfs_fsop_geom	*geo,
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
	bs1->bs_extsize = bstat->bs_extsize_blks * geo->blocksize;
	bs1->bs_extents = bstat->bs_extents;
	bs1->bs_gen = bstat->bs_gen;
	bs1->bs_projid_lo = bstat->bs_projectid & 0xFFFF;
	bs1->bs_forkoff = bstat->bs_forkoff;
	bs1->bs_projid_hi = bstat->bs_projectid >> 16;
	bs1->bs_sick = bstat->bs_sick;
	bs1->bs_checked = bstat->bs_checked;
	bs1->bs_cowextsize = bstat->bs_cowextsize_blks * geo->blocksize;
	bs1->bs_dmevmask = 0;
	bs1->bs_dmstate = 0;
	bs1->bs_aextents = bstat->bs_aextents;
}

/* Convert bstat (v1) to bulkstat (v5). */
void
xfrog_bstat_to_bulkstat(
	const struct xfs_fsop_geom	*geo,
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
	bstat->bs_extsize_blks = bs1->bs_extsize / geo->blocksize;
	bstat->bs_extents = bs1->bs_extents;
	bstat->bs_gen = bs1->bs_gen;
	bstat->bs_projectid = bstat_get_projid(bs1);
	bstat->bs_forkoff = bs1->bs_forkoff;
	bstat->bs_sick = bs1->bs_sick;
	bstat->bs_checked = bs1->bs_checked;
	bstat->bs_cowextsize_blks = bs1->bs_cowextsize / geo->blocksize;
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

/* Query inode allocation bitmask information. */
int
xfrog_inumbers(
	int			fd,
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

	return ioctl(fd, XFS_IOC_FSINUMBERS, &bulkreq);
}
