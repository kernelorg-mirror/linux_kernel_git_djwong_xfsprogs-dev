// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2001,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#ifndef _XFS_REPAIR_RT_H_
#define _XFS_REPAIR_RT_H_

struct blkmap;

void
rtinit(xfs_mount_t		*mp);

int generate_rtinfo(struct xfs_mount *mp, union xfs_rtword_raw *words,
		union xfs_suminfo_raw *sumcompute);

void check_rtbitmap(struct xfs_mount *mp);
void check_rtsummary(struct xfs_mount *mp);
void check_rtsb(struct xfs_mount *mp);

void rewrite_rtsb(struct xfs_mount *mp);

void discover_rtgroup_inodes(struct xfs_mount *mp);
void free_rtgroup_inodes(struct xfs_mount *mp);

xfs_rgnumber_t rtgroup_for_rtginode(struct xfs_mount *mp, xfs_ino_t ino,
		enum xfs_rtg_inodes type);

static inline xfs_rgnumber_t
rtgroup_for_rtrmap_inode(struct xfs_mount *mp, xfs_ino_t ino)
{
	return rtgroup_for_rtginode(mp, ino, XFS_RTG_RMAP);
}

static inline xfs_rgnumber_t
rtgroup_for_rtrefcount_inode(struct xfs_mount *mp, xfs_ino_t ino)
{
	return rtgroup_for_rtginode(mp, ino, XFS_RTG_REFCOUNT);
}

bool is_rtgroup_inode(xfs_ino_t ino, enum xfs_rtg_inodes type);

static inline bool is_rtrmap_inode(xfs_ino_t ino)
{
	return is_rtgroup_inode(ino, XFS_RTG_RMAP);
}

static inline bool is_rtrefcount_inode(xfs_ino_t ino)
{
	return is_rtgroup_inode(ino, XFS_RTG_REFCOUNT);
}

void rtginode_avoid_check(struct xfs_mount *mp, enum xfs_rtg_inodes type);

#endif /* _XFS_REPAIR_RT_H_ */
