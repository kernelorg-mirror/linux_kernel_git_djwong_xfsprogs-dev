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
	if (flags & XFS_ICHGTIME_CREATE) {
		ip->i_d.di_crtime.t_sec = (int32_t)tv.tv_sec;
		ip->i_d.di_crtime.t_nsec = (int32_t)tv.tv_nsec;
	}
}

/*
 * Allocate an inode on disk and return a copy of its in-core version.
 * Set mode, nlink, and rdev appropriately within the inode.
 * The uid and gid for the inode are set according to the contents of
 * the given cred structure.
 *
 * This was once shared with the kernel, but has diverged to the point
 * where it's no longer worth the hassle of maintaining common code.
 */
static int
libxfs_ialloc(
	xfs_trans_t	*tp,
	xfs_inode_t	*pip,
	mode_t		mode,
	nlink_t		nlink,
	xfs_dev_t	rdev,
	struct cred	*cr,
	struct fsxattr	*fsx,
	xfs_buf_t	**ialloc_context,
	xfs_inode_t	**ipp)
{
	xfs_ino_t	ino;
	xfs_inode_t	*ip;
	uint		flags;
	int		error;

	/*
	 * Call the space management code to pick
	 * the on-disk inode to be allocated.
	 */
	error = xfs_dialloc(tp, pip ? pip->i_ino : 0, mode,
			    ialloc_context, &ino);
	if (error != 0)
		return error;
	if (*ialloc_context || ino == NULLFSINO) {
		*ipp = NULL;
		return 0;
	}
	ASSERT(*ialloc_context == NULL);

	error = xfs_trans_iget(tp->t_mountp, tp, ino, 0, 0, &ip);
	if (error != 0)
		return error;
	ASSERT(ip != NULL);

	VFS_I(ip)->i_mode = mode;
	set_nlink(VFS_I(ip), nlink);
	ip->i_d.di_uid = cr->cr_uid;
	ip->i_d.di_gid = cr->cr_gid;
	libxfs_set_projid(ip, pip ? 0 : fsx->fsx_projid);
	xfs_trans_ichgtime(tp, ip, XFS_ICHGTIME_CHG | XFS_ICHGTIME_MOD);

	/*
	 * We only support filesystems that understand v2 format inodes. So if
	 * this is currently an old format inode, then change the inode version
	 * number now.  This way we only do the conversion here rather than here
	 * and in the flush/logging code.
	 */
	if (ip->i_d.di_version == 1) {
		ip->i_d.di_version = 2;
		/*
		 * old link count, projid_lo/hi field, pad field
		 * already zeroed
		 */
	}

	if (pip && (VFS_I(pip)->i_mode & S_ISGID)) {
		ip->i_d.di_gid = pip->i_d.di_gid;
		if ((VFS_I(pip)->i_mode & S_ISGID) && (mode & S_IFMT) == S_IFDIR)
			VFS_I(ip)->i_mode |= S_ISGID;
	}

	ip->i_d.di_size = 0;
	ip->i_d.di_nextents = 0;
	ASSERT(ip->i_d.di_nblocks == 0);
	ip->i_d.di_extsize = pip ? 0 : fsx->fsx_extsize;
	ip->i_d.di_dmevmask = 0;
	ip->i_d.di_dmstate = 0;
	ip->i_d.di_flags = pip ? 0 : xfs_flags2diflags(ip, fsx->fsx_xflags);

	if (ip->i_d.di_version == 3) {
		ASSERT(ip->i_d.di_ino == ino);
		ASSERT(uuid_equal(&ip->i_d.di_uuid, &mp->m_sb.sb_meta_uuid));
		VFS_I(ip)->i_version = 1;
		ip->i_d.di_flags2 = pip ? 0 : xfs_flags2diflags2(ip,
				fsx->fsx_xflags);
		ip->i_d.di_crtime.t_sec = (int32_t)VFS_I(ip)->i_mtime.tv_sec;
		ip->i_d.di_crtime.t_nsec = (int32_t)VFS_I(ip)->i_mtime.tv_nsec;
		ip->i_d.di_cowextsize = pip ? 0 : fsx->fsx_cowextsize;
	}

	flags = XFS_ILOG_CORE;
	switch (mode & S_IFMT) {
	case S_IFIFO:
	case S_IFSOCK:
		/* doesn't make sense to set an rdev for these */
		rdev = 0;
		/* FALLTHROUGH */
	case S_IFCHR:
	case S_IFBLK:
		ip->i_d.di_format = XFS_DINODE_FMT_DEV;
		flags |= XFS_ILOG_DEV;
		VFS_I(ip)->i_rdev = rdev;
		break;
	case S_IFREG:
	case S_IFDIR:
		if (pip && (pip->i_d.di_flags & XFS_DIFLAG_ANY)) {
			uint	di_flags = 0;

			if ((mode & S_IFMT) == S_IFDIR) {
				if (pip->i_d.di_flags & XFS_DIFLAG_RTINHERIT)
					di_flags |= XFS_DIFLAG_RTINHERIT;
				if (pip->i_d.di_flags & XFS_DIFLAG_EXTSZINHERIT) {
					di_flags |= XFS_DIFLAG_EXTSZINHERIT;
					ip->i_d.di_extsize = pip->i_d.di_extsize;
				}
			} else {
				if (pip->i_d.di_flags & XFS_DIFLAG_RTINHERIT) {
					di_flags |= XFS_DIFLAG_REALTIME;
				}
				if (pip->i_d.di_flags & XFS_DIFLAG_EXTSZINHERIT) {
					di_flags |= XFS_DIFLAG_EXTSIZE;
					ip->i_d.di_extsize = pip->i_d.di_extsize;
				}
			}
			if (pip->i_d.di_flags & XFS_DIFLAG_PROJINHERIT)
				di_flags |= XFS_DIFLAG_PROJINHERIT;
			ip->i_d.di_flags |= di_flags;
		}
		/* FALLTHROUGH */
	case S_IFLNK:
		ip->i_d.di_format = XFS_DINODE_FMT_EXTENTS;
		ip->i_df.if_flags = XFS_IFEXTENTS;
		ip->i_df.if_bytes = 0;
		ip->i_df.if_u1.if_root = NULL;
		break;
	default:
		ASSERT(0);
	}
	/* Attribute fork settings for new inode. */
	ip->i_d.di_aformat = XFS_DINODE_FMT_EXTENTS;
	ip->i_d.di_anextents = 0;

	/*
	 * set up the inode ops structure that the libxfs code relies on
	 */
	if (XFS_ISDIR(ip))
		ip->d_ops = ip->i_mount->m_dir_inode_ops;
	else
		ip->d_ops = ip->i_mount->m_nondir_inode_ops;

	/*
	 * Log the new values stuffed into the inode.
	 */
	xfs_trans_log_inode(tp, ip, flags);
	*ipp = ip;
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
	if (!libxfs_inode_verify_forks(ip, &xfs_default_ifork_ops))
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
 * Wrapper around call to libxfs_ialloc. Takes care of committing and
 * allocating a new transaction as needed.
 *
 * Originally there were two copies of this code - one in mkfs, the
 * other in repair - now there is just the one.
 */
int
libxfs_inode_alloc(
	xfs_trans_t	**tp,
	xfs_inode_t	*pip,
	mode_t		mode,
	nlink_t		nlink,
	xfs_dev_t	rdev,
	struct cred	*cr,
	struct fsxattr	*fsx,
	xfs_inode_t	**ipp)
{
	xfs_buf_t	*ialloc_context;
	xfs_inode_t	*ip;
	int		error;

	ialloc_context = (xfs_buf_t *)0;
	error = libxfs_ialloc(*tp, pip, mode, nlink, rdev, cr, fsx,
			   &ialloc_context, &ip);
	if (error) {
		*ipp = NULL;
		return error;
	}
	if (!ialloc_context && !ip) {
		*ipp = NULL;
		return -ENOSPC;
	}

	if (ialloc_context) {

		xfs_trans_bhold(*tp, ialloc_context);

		error = xfs_trans_roll(tp);
		if (error) {
			fprintf(stderr, _("%s: cannot duplicate transaction: %s\n"),
				progname, strerror(error));
			exit(1);
		}
		xfs_trans_bjoin(*tp, ialloc_context);
		error = libxfs_ialloc(*tp, pip, mode, nlink, rdev, cr,
				   fsx, &ialloc_context, &ip);
		if (!ip)
			error = -ENOSPC;
		if (error)
			return error;
	}

	*ipp = ip;
	return error;
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
	uint			lock_flags,
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
	error = xfs_iread(mp, tp, ip, 0);
	if (error) {
		kmem_zone_free(xfs_inode_zone, ip);
		*ipp = NULL;
		return error;
	}

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
	if (ip->i_itemp)
		kmem_zone_free(xfs_ili_zone, ip->i_itemp);
	ip->i_itemp = NULL;
	libxfs_idestroy(ip);
	kmem_zone_free(xfs_inode_zone, ip);
}
