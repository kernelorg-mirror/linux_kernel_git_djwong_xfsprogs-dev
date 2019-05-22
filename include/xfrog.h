// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2002 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#ifndef __XFROG_H__
#define __XFROG_H__

/*
 * XFS Filesystem Runtime Online Gluecode
 * ======================================
 *
 * These support functions wrap the more complex xfs ioctls so that xfs
 * utilities can take advantage of them without having to deal with graceful
 * degradation in the face of new ioctls.  They will also provide higher level
 * abstractions when possible.
 */

struct xfs_fsop_geom;
int xfrog_geometry(int fd, struct xfs_fsop_geom *fsgeo);

struct xfs_bstat;
int xfrog_bulkstat_single(int fd, struct xfs_fsop_geom *geom,
		struct xfs_bulkstat_single_req *req);
int xfrog_bulkstat(int fd, struct xfs_fsop_geom *geom,
		struct xfs_bulkstat_req *req);
struct xfs_bulkstat_req *xfrog_bulkstat_alloc_req(uint32_t nr,
		uint64_t startino);
void xfrog_bulkstat_to_bstat(const struct xfs_fsop_geom *geo,
		struct xfs_bstat *bs1, const struct xfs_bulkstat *bstat);
void xfrog_bstat_to_bulkstat(const struct xfs_fsop_geom *geo,
		struct xfs_bulkstat *bstat, const struct xfs_bstat *bs1);

struct xfs_inogrp;
int xfrog_inumbers(int fd, uint64_t *lastino, uint32_t icount,
		struct xfs_inogrp *ubuffer, uint32_t *ocount);

int xfrog_ag_geometry(int fd, unsigned int agno, struct xfs_ag_geometry *ageo);

#endif	/* __XFROG_H__ */
