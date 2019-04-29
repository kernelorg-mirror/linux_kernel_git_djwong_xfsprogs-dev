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
int xfrog_bulkstat_single(int fd, uint64_t ino, struct xfs_bstat *ubuffer);
int xfrog_bulkstat(int fd, uint64_t *lastino, uint32_t icount,
		struct xfs_bstat *ubuffer, uint32_t *ocount);

struct xfs_inogrp;
int xfrog_inumbers(int fd, uint64_t *lastino, uint32_t icount,
		struct xfs_inogrp *ubuffer, uint32_t *ocount);

#endif	/* __XFROG_H__ */
