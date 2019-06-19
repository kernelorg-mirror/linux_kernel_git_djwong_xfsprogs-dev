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
};

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
int xfrog_bulkstat_single(struct xfrog *froggie, uint64_t ino,
		struct xfs_bstat *ubuffer);
int xfrog_bulkstat(struct xfrog *froggie, uint64_t *lastino, uint32_t icount,
		struct xfs_bstat *ubuffer, uint32_t *ocount);

struct xfs_inogrp;
int xfrog_inumbers(struct xfrog *froggie, uint64_t *lastino, uint32_t icount,
		struct xfs_inogrp *ubuffer, uint32_t *ocount);

#endif	/* __XFROG_H__ */
