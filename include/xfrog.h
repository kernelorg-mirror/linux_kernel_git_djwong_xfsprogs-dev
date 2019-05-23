// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2002 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#ifndef __XFROG_H__
#define __XFROG_H__

/*
 * XFS Filesystem Random Online Gluecode
 * =====================================
 *
 * These support functions wrap the more complex xfs ioctls so that xfs
 * utilities can take advantage of them without having to deal with graceful
 * degradation in the face of new ioctls.  They will also provide higher level
 * abstractions when possible.
 */

struct xfs_fsop_geom;
int xfrog_geometry(int fd, struct xfs_fsop_geom *fsgeo);

/*
 * Structure for recording whatever observations we want about the level of
 * xfs runtime support for this fd.  Right now we only store the fd and fs
 * geometry.
 */
struct xfrog {
	/* ioctl file descriptor */
	int			fd;

	/* filesystem geometry */
	struct xfs_fsop_geom	fsgeom;

	/* log2 of sb_agblocks (rounded up) */
	unsigned int		agblklog;

	/* log2 of sb_blocksize */
	unsigned int		blocklog;

	/* log2 of sb_inodesize */
	unsigned int		inodelog;

	/* log2 of sb_inopblock */
	unsigned int		inopblog;

	/* bits for agino in inum */
	unsigned int		aginolog;

	/* log2 of sb_blocksize / sb_sectsize */
	unsigned int		blkbb_log;

	/* XFROG_FLAG_* state flags */
	unsigned int		flags;
};

/* Only use v1 bulkstat/inumbers ioctls. */
#define XFROG_FLAG_BULKSTAT_FORCE_V1	(1 << 0)

/* Only use v5 bulkstat/inumbers ioctls. */
#define XFROG_FLAG_BULKSTAT_FORCE_V5	(1 << 1)

/* Static initializers */
#define XFROG_INIT(_fd)		{ .fd = (_fd), }
#define XFROG_INIT_EMPTY	XFROG_INIT(-1)

int xfrog_prepare_geometry(struct xfrog *froggie);
int xfrog_close(struct xfrog *froggie);

/* Convert AG number and AG inode number into fs inode number. */
static inline uint64_t
xfrog_agino_to_ino(
	struct xfrog		*frog,
	uint32_t		agno,
	uint32_t		agino)
{
	return ((uint64_t)agno << frog->aginolog) + agino;
}

/* Convert fs inode number into AG number. */
static inline uint32_t
xfrog_ino_to_agno(
	struct xfrog		*frog,
	uint64_t		ino)
{
	return ino >> frog->aginolog;
}

/* Convert fs inode number into AG inode number. */
static inline uint32_t
xfrog_ino_to_agino(
	struct xfrog		*frog,
	uint64_t		ino)
{
	return ino & ((1ULL << frog->aginolog) - 1);
}

/* Convert fs block number into bytes */
static inline uint64_t
xfrog_fsb_to_b(
	struct xfrog		*frog,
	uint64_t		fsb)
{
	return fsb << frog->blocklog;
}

/* Convert bytes into (rounded down) fs block number */
static inline uint64_t
xfrog_b_to_fsbt(
	struct xfrog		*frog,
	uint64_t		bytes)
{
	return bytes >> frog->blocklog;
}

/* Convert sector number to bytes. */
static inline uint64_t
xfrog_bbtob(
	uint64_t		daddr)
{
	return daddr << BBSHIFT;
}

/* Convert bytes to sector number, rounding down. */
static inline uint64_t
xfrog_btobbt(
	uint64_t		bytes)
{
	return bytes >> BBSHIFT;
}

/* Convert fs block number to sector number. */
static inline uint64_t
xfrog_fsb_to_bb(
	struct xfrog		*frog,
	uint64_t		fsbno)
{
	return fsbno << frog->blkbb_log;
}

/* Convert sector number to fs block number, rounded down. */
static inline uint64_t
xfrog_bb_to_fsbt(
	struct xfrog		*frog,
	uint64_t		daddr)
{
	return daddr >> frog->blkbb_log;
}

/* Convert AG number and AG block to fs block number */
static inline uint64_t
xfrog_agb_to_daddr(
	struct xfrog		*frog,
	uint32_t		agno,
	uint32_t		agbno)
{
	return xfrog_fsb_to_bb(frog,
			(uint64_t)agno * frog->fsgeom.agblocks + agbno);
}

/* Convert sector number to AG number. */
static inline uint32_t
xfrog_daddr_to_agno(
	struct xfrog		*frog,
	uint64_t		daddr)
{
	return xfrog_bb_to_fsbt(frog, daddr) / frog->fsgeom.agblocks;
}

/* Convert sector number to AG block number. */
static inline uint32_t
xfrog_daddr_to_agbno(
	struct xfrog		*frog,
	uint64_t		daddr)
{
	return xfrog_bb_to_fsbt(frog, daddr) % frog->fsgeom.agblocks;
}

/* Bulkstat wrappers */
struct xfs_bstat;
int xfrog_bulkstat_single(struct xfrog *froggie,
		struct xfs_bulkstat_single_req *req);
int xfrog_bulkstat(struct xfrog *froggie, struct xfs_bulkstat_req *req);

struct xfs_bulkstat_req *xfrog_bulkstat_alloc_req(uint32_t nr,
		uint64_t startino);
void xfrog_bulkstat_to_bstat(struct xfrog *froggie, struct xfs_bstat *bs1,
		const struct xfs_bulkstat *bstat);
void xfrog_bstat_to_bulkstat(struct xfrog *froggie, struct xfs_bulkstat *bstat,
		const struct xfs_bstat *bs1);

void xfrog_bulkstat_set_ag(struct xfs_bulkstat_req *req, uint32_t agno);

struct xfs_inogrp;
int xfrog_inumbers(struct xfrog *froggie, struct xfs_inumbers_req *req);

struct xfs_inumbers_req *xfrog_inumbers_alloc_req(uint32_t nr,
		uint64_t startino);
void xfrog_inumbers_set_ag(struct xfs_inumbers_req *req, uint32_t agno);
void xfrog_inumbers_to_inogrp(struct xfs_inogrp *ig1,
		const struct xfs_inumbers *ig);
void xfrog_inogrp_to_inumbers(struct xfs_inumbers *ig,
		const struct xfs_inogrp *ig1);

int xfrog_ag_geometry(int fd, unsigned int agno, struct xfs_ag_geometry *ageo);

#endif	/* __XFROG_H__ */
