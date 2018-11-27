// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "libxfs_priv.h"
#include "libxfs.h"
#include "libxfs_io.h"
#include "init.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_defer.h"
#include "xfs_inode_buf.h"
#include "xfs_inode_fork.h"
#include "xfs_inode.h"
#include "xfs_trans.h"
#include "xfs_bmap.h"
#include "xfs_bmap_btree.h"
#include "xfs_trans_space.h"
#include "xfs_ialloc.h"
#include "xfs_alloc.h"
#include "xfs_bit.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_dir2_priv.h"

/*
 * Change the requested timestamp in the given inode.
 *
 * This was once shared with the kernel, but has diverged to the point
 * where it's no longer worth the hassle of maintaining common code.
 */
void
libxfs_trans_ichgtime(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip,
	int			flags)
{
	struct timespec tv;
	struct timeval	stv;

	gettimeofday(&stv, (struct timezone *)0);
	tv.tv_sec = stv.tv_sec;
	tv.tv_nsec = stv.tv_usec * 1000;
	if (flags & XFS_ICHGTIME_MOD)
		VFS_I(ip)->i_mtime = tv;
	if (flags & XFS_ICHGTIME_CHG)
		VFS_I(ip)->i_ctime = tv;
	if (flags & XFS_ICHGTIME_ACCESS)
		VFS_I(ip)->i_atime = tv;
	if (flags & XFS_ICHGTIME_CREATE) {
		ip->i_d.di_crtime.t_sec = (int32_t)tv.tv_sec;
		ip->i_d.di_crtime.t_nsec = (int32_t)tv.tv_nsec;
	}
}

/* Propagate extended inode properties into the new child. */
static void
xfs_ialloc_fsx_init(
	struct xfs_trans		**tpp,
	struct xfs_inode		*ip,
	const struct fsxattr		*fsx)
{
	ip->i_d.di_extsize = fsx->fsx_extsize;
	ip->i_d.di_flags = xfs_flags2diflags(ip, fsx->fsx_xflags);

	if (ip->i_d.di_version == 3) {
		ip->i_d.di_flags2 = xfs_flags2diflags2(ip, fsx->fsx_xflags);
		ip->i_d.di_cowextsize = fsx->fsx_cowextsize;
	}
	xfs_trans_log_inode(*tpp, ip, XFS_ILOG_CORE);
}

/* Set up the inode ops structure that the libxfs code relies on. */
void
xfs_setup_inode(
	struct xfs_inode	*ip)
{
	if (XFS_ISDIR(ip))
		ip->d_ops = ip->i_mount->m_dir_inode_ops;
	else
		ip->d_ops = ip->i_mount->m_nondir_inode_ops;
}

/*
 * Create in-core inode for a newly allocated on-disk inode.  We don't have
 * any effective inode locking so we don't need to grab anything.
 */
int
xfs_ialloc_iget(
	struct xfs_trans	*tp,
	xfs_ino_t		ino,
	struct xfs_inode	**ipp)
{
	return libxfs_iget(tp->t_mountp, tp, ino, XFS_IGET_CREATE, ipp,
			&xfs_default_ifork_ops);
}

/* Roll transaction after allocating inode chunk. */
int
xfs_dir_ialloc_roll(
	struct xfs_trans	**tpp)
{
	return xfs_trans_roll(tpp);
}

/*
 * Wrapper around call to libxfs_dir_ialloc. Takes care of committing and
 * allocating a new transaction as needed.
 */
int
libxfs_inode_alloc(
	struct xfs_trans	**tpp,
	struct xfs_inode	*pip,
	mode_t			mode,
	nlink_t			nlink,
	xfs_dev_t		rdev,
	struct cred		*cr,
	struct fsxattr		*fsx,
	struct xfs_inode	**ipp)
{
	struct xfs_ialloc_args	args = {
		.pip		= pip,
		.uid		= cr->cr_uid,
		.gid		= cr->cr_gid,
		.prid		= pip ? 0 : fsx->fsx_projid,
		.nlink		= nlink,
		.rdev		= rdev,
		.mode		= mode,
	};
	int			error;

	error = xfs_dir_ialloc(tpp, &args, ipp);
	if (error)
		return error;
	if (!pip)
		xfs_ialloc_fsx_init(tpp, *ipp, fsx);
	return 0;
}

/*
 * Writes a modified inode's changes out to the inode's on disk home.
 * Originally based on xfs_iflush_int() from xfs_inode.c in the kernel.
 */
int
libxfs_iflush_int(xfs_inode_t *ip, xfs_buf_t *bp)
{
	xfs_inode_log_item_t	*iip;
	xfs_dinode_t		*dip;
	xfs_mount_t		*mp;

	ASSERT(bp-b_log_item != NULL);
	ASSERT(ip->i_d.di_format != XFS_DINODE_FMT_BTREE ||
		ip->i_d.di_nextents > ip->i_df.if_ext_max);
	ASSERT(ip->i_d.di_version > 1);

	iip = ip->i_itemp;
	mp = ip->i_mount;

	/* set *dip = inode's place in the buffer */
	dip = xfs_buf_offset(bp, ip->i_imap.im_boffset);

	ASSERT(ip->i_d.di_magic == XFS_DINODE_MAGIC);
	if (XFS_ISREG(ip)) {
		ASSERT( (ip->i_d.di_format == XFS_DINODE_FMT_EXTENTS) ||
			(ip->i_d.di_format == XFS_DINODE_FMT_BTREE) );
	} else if (XFS_ISDIR(ip)) {
		ASSERT( (ip->i_d.di_format == XFS_DINODE_FMT_EXTENTS) ||
			(ip->i_d.di_format == XFS_DINODE_FMT_BTREE)   ||
			(ip->i_d.di_format == XFS_DINODE_FMT_LOCAL) );
	}
	ASSERT(ip->i_d.di_nextents+ip->i_d.di_anextents <= ip->i_d.di_nblocks);
	ASSERT(ip->i_d.di_forkoff <= mp->m_sb.sb_inodesize);

	/* bump the change count on v3 inodes */
	if (ip->i_d.di_version == 3)
		VFS_I(ip)->i_version++;

	/* Check the inline fork data before we write out. */
	if (!libxfs_inode_verify_forks(ip, ip->i_fork_ops))
		return -EFSCORRUPTED;

	/*
	 * Copy the dirty parts of the inode into the on-disk
	 * inode.  We always copy out the core of the inode,
	 * because if the inode is dirty at all the core must
	 * be.
	 */
	xfs_inode_to_disk(ip, dip, iip->ili_item.li_lsn);

	xfs_iflush_fork(ip, dip, iip, XFS_DATA_FORK);
	if (XFS_IFORK_Q(ip))
		xfs_iflush_fork(ip, dip, iip, XFS_ATTR_FORK);

	/* generate the checksum. */
	xfs_dinode_calc_crc(mp, dip);

	return 0;
}

/*
 * Inode cache stubs.
 */

kmem_zone_t		*xfs_inode_zone;
extern kmem_zone_t	*xfs_ili_zone;

/*
 * If there are inline format data / attr forks attached to this inode,
 * make sure they're not corrupt.
 */
bool
libxfs_inode_verify_forks(
	struct xfs_inode	*ip,
	struct xfs_ifork_ops	*ops)
{
	struct xfs_ifork	*ifp;
	xfs_failaddr_t		fa;

	if (!ops)
		return true;

	fa = xfs_ifork_verify_data(ip, ops);
	if (fa) {
		ifp = XFS_IFORK_PTR(ip, XFS_DATA_FORK);
		xfs_inode_verifier_error(ip, -EFSCORRUPTED, "data fork",
				ifp->if_u1.if_data, ifp->if_bytes, fa);
		return false;
	}

	fa = xfs_ifork_verify_attr(ip, ops);
	if (fa) {
		ifp = XFS_IFORK_PTR(ip, XFS_ATTR_FORK);
		xfs_inode_verifier_error(ip, -EFSCORRUPTED, "attr fork",
				ifp ? ifp->if_u1.if_data : NULL,
				ifp ? ifp->if_bytes : 0, fa);
		return false;
	}
	return true;
}

int
libxfs_iget(
	struct xfs_mount	*mp,
	struct xfs_trans	*tp,
	xfs_ino_t		ino,
	uint			iget_flags,
	struct xfs_inode	**ipp,
	struct xfs_ifork_ops	*ifork_ops)
{
	struct xfs_inode	*ip;
	int			error = 0;

	ip = kmem_zone_zalloc(xfs_inode_zone, 0);
	if (!ip)
		return -ENOMEM;

	ip->i_ino = ino;
	ip->i_mount = mp;
	error = xfs_iread(mp, tp, ip, iget_flags);
	if (error) {
		kmem_zone_free(xfs_inode_zone, ip);
		*ipp = NULL;
		return error;
	}

	ip->i_fork_ops = ifork_ops;
	if (!libxfs_inode_verify_forks(ip, ifork_ops)) {
		libxfs_irele(ip);
		return -EFSCORRUPTED;
	}

	/*
	 * set up the inode ops structure that the libxfs code relies on
	 */
	if (XFS_ISDIR(ip))
		ip->d_ops = mp->m_dir_inode_ops;
	else
		ip->d_ops = mp->m_nondir_inode_ops;

	*ipp = ip;
	return 0;
}

/* Get a metadata inode.  The ftype must match exactly. */
int
libxfs_imeta_iget(
	struct xfs_mount	*mp,
	xfs_ino_t		ino,
	unsigned char		ftype,
	struct xfs_inode	**ipp)
{
	struct xfs_inode	*ip;
	int			error;

	error = libxfs_iget(mp, NULL, ino, 0, &ip, &xfs_default_ifork_ops);
	if (error)
		return error;

	if (ftype == XFS_DIR3_FT_UNKNOWN ||
	    xfs_mode_to_ftype(VFS_I(ip)->i_mode) != ftype) {
		libxfs_irele(ip);
		return -EFSCORRUPTED;
	}

	*ipp = ip;
	return 0;
}

static void
libxfs_idestroy(xfs_inode_t *ip)
{
	switch (VFS_I(ip)->i_mode & S_IFMT) {
		case S_IFREG:
		case S_IFDIR:
		case S_IFLNK:
			libxfs_idestroy_fork(ip, XFS_DATA_FORK);
			break;
	}
	if (ip->i_afp)
		libxfs_idestroy_fork(ip, XFS_ATTR_FORK);
	if (ip->i_cowfp)
		xfs_idestroy_fork(ip, XFS_COW_FORK);
}

void
libxfs_irele(
	struct xfs_inode	*ip)
{
	ASSERT(ip->i_itemp == NULL);
	libxfs_idestroy(ip);
	kmem_zone_free(xfs_inode_zone, ip);
}

void
libxfs_imeta_irele(
	struct xfs_inode	*ip)
{
	libxfs_irele(ip);
}

/*
 * Stub of fast in-core unlinked list lookups.  We store nothing and lookups
 * return -ENOENT which will cause us to fall back to slow lookups.  Nothing
 * calls the iunlink functions so this is not a big deal.
 */

int
xfs_iunlink_init(
	struct xfs_perag	*pag)
{
	return 0;
}

void
xfs_iunlink_destroy(
	struct xfs_perag	*pag)
{
}

/* Not implemented; we'll have to search the AGI unlinked list. */
xfs_agino_t
xfs_iunlink_lookup_backref(
	struct xfs_perag	*pag,
	xfs_agino_t		agino)
{
	return NULLAGINO;
}

/* Remember that @prev_agino.next_unlinked = @this_agino. */
int
xfs_iunlink_add_backref(
	struct xfs_perag	*pag,
	xfs_agino_t		prev_agino,
	xfs_agino_t		this_agino)
{
	return 0;
}

/* Forget that X.next_unlinked = @agino. */
int
xfs_iunlink_forget_backref(
	struct xfs_perag	*pag,
	xfs_agino_t		agino)
{
	return 0;
}


/* Replace X.next_unlinked = @agino with X.next_unlinked = @next_unlinked. */
int
xfs_iunlink_change_backref(
	struct xfs_perag	*pag,
	xfs_agino_t		agino,
	xfs_agino_t		next_unlinked)
{
	return 0;
}
