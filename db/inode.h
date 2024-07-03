// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2001 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

extern const struct field	inode_a_flds[];
extern const struct field	inode_core_flds[];
extern const struct field	inode_v3_flds[];
extern const struct field	inode_flds[];
extern const struct field	inode_crc_flds[];
extern const struct field	inode_hfld[];
extern const struct field	inode_crc_hfld[];
extern const struct field	inode_u_flds[];
extern const struct field	timestamp_flds[];

extern int	fp_dinode_fmt(void *obj, int bit, int count, char *fmtstr,
			      int size, int arg, int base, int array);
extern int	inode_a_size(void *obj, int startoff, int idx);
extern void	inode_init(void);
extern typnm_t	inode_next_type(void);
extern int	inode_size(void *obj, int startoff, int idx);
extern int	inode_u_size(void *obj, int startoff, int idx);
extern void	xfs_inode_set_crc(struct xfs_buf *);
extern void	set_cur_inode(xfs_ino_t ino);

int init_rtmeta_inode_bitmaps(struct xfs_mount *mp);
xfs_rgnumber_t rtgroup_for_rtginode(struct xfs_mount *mp, xfs_ino_t ino,
		enum xfs_rtg_inodes type);

static inline xfs_rgnumber_t
rtgroup_for_rtrmap_ino(struct xfs_mount *mp, xfs_ino_t ino)
{
	return rtgroup_for_rtginode(mp, ino, XFS_RTG_RMAP);
}

bool is_rtgroup_inode(xfs_ino_t ino, enum xfs_rtg_inodes type);

static inline bool is_rtrmap_inode(xfs_ino_t ino)
{
	return is_rtgroup_inode(ino, XFS_RTG_RMAP);
}
