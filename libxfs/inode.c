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

/* Propagate extended inode properties into the new child. */
static void
xfs_ialloc_fsx_init(
	struct xfs_trans		**tpp,
	struct xfs_inode		*ip,
	const struct fsxattr		*fsx)
{
	ip->i_d.di_extsize = fsx->fsx_extsize;
	ip->i_d.di_flags = xfs_flags2diflags(ip, fsx->fsx_xflags);

	if (xfs_sb_version_has_v3inode(&ip->i_mount->m_sb)) {
		ip->i_d.di_flags2 = xfs_flags2diflags2(ip, fsx->fsx_xflags);
		ip->i_d.di_cowextsize = fsx->fsx_cowextsize;
	}
	xfs_trans_log_inode(*tpp, ip, XFS_ILOG_CORE);
}

/*
 * Create in-core inode for a newly allocated on-disk inode.  We don't have
 * any effective inode locking so we don't need to grab anything.
 */
static int
xfs_ialloc_iget(
	struct xfs_trans	*tp,
	xfs_ino_t		ino,
	struct xfs_inode	**ipp)
{
	return libxfs_iget(tp->t_mountp, tp, ino, 0, ipp);
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
int
libxfs_ialloc(
	struct xfs_trans		*tp,
	const struct xfs_ialloc_args	*args,
	struct xfs_buf			**ialloc_context,
	struct xfs_inode		**ipp)
{
	struct xfs_inode		*ip;
	struct xfs_inode		*pip = args->pip;
	xfs_ino_t			ino;
	uint				flags;
	int				times;
	int				error;

	/*
	 * Call the space management code to pick
	 * the on-disk inode to be allocated.
	 */
	error = xfs_dialloc(tp, pip ? pip->i_ino : 0, args->mode,
			    ialloc_context, &ino);
	if (error != 0)
		return error;
	if (*ialloc_context || ino == NULLFSINO) {
		*ipp = NULL;
		return 0;
	}
	ASSERT(*ialloc_context == NULL);

	error = xfs_ialloc_iget(tp, ino, &ip);
	if (error != 0)
		return error;
	ASSERT(ip != NULL);

	VFS_I(ip)->i_mode = args->mode;
	set_nlink(VFS_I(ip), args->nlink);
	VFS_I(ip)->i_uid = args->uid;
	VFS_I(ip)->i_rdev = args->rdev;
	ip->i_d.di_projid = args->prid;

	if (pip && (VFS_I(pip)->i_mode & S_ISGID)) {
		VFS_I(ip)->i_gid = VFS_I(pip)->i_gid;
		if ((VFS_I(pip)->i_mode & S_ISGID) && S_ISDIR(args->mode))
			VFS_I(ip)->i_mode |= S_ISGID;
	} else
		VFS_I(ip)->i_gid = args->gid;

	ip->i_d.di_size = 0;
	ip->i_df.if_nextents = 0;
	ASSERT(ip->i_d.di_nblocks == 0);

	times = XFS_ICHGTIME_CHG | XFS_ICHGTIME_MOD | XFS_ICHGTIME_ACCESS;
	ip->i_d.di_extsize = 0;
	ip->i_d.di_dmevmask = 0;
	ip->i_d.di_dmstate = 0;
	ip->i_d.di_flags = 0;

	if (xfs_sb_version_has_v3inode(&ip->i_mount->m_sb)) {
		ASSERT(ip->i_d.di_ino == ino);
		ASSERT(uuid_equal(&ip->i_d.di_uuid, &mp->m_sb.sb_meta_uuid));
		VFS_I(ip)->i_version = 1;
		ip->i_d.di_flags2 = ip->i_mount->m_ino_geo.new_diflags2;
		times |= XFS_ICHGTIME_CREATE;
		ip->i_d.di_cowextsize = 0;
	}

	xfs_trans_ichgtime(tp, ip, times);

	flags = XFS_ILOG_CORE;
	switch (args->mode & S_IFMT) {
	case S_IFIFO:
	case S_IFSOCK:
	case S_IFCHR:
	case S_IFBLK:
		ip->i_df.if_format = XFS_DINODE_FMT_DEV;
		flags |= XFS_ILOG_DEV;
		VFS_I(ip)->i_rdev = args->rdev;
		break;
	case S_IFREG:
	case S_IFDIR:
		if (pip && (pip->i_d.di_flags & XFS_DIFLAG_ANY)) {
			uint	di_flags = 0;

			if ((args->mode & S_IFMT) == S_IFDIR) {
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
		ip->i_df.if_format = XFS_DINODE_FMT_EXTENTS;
		ip->i_df.if_flags = XFS_IFEXTENTS;
		ip->i_df.if_bytes = 0;
		ip->i_df.if_u1.if_root = NULL;
		break;
	default:
		ASSERT(0);
	}

	/*
	 * Log the new values stuffed into the inode.
	 */
	xfs_trans_ijoin(tp, ip, 0);
	xfs_trans_log_inode(tp, ip, flags);

	*ipp = ip;
	return 0;
}

/*
 * Writes a modified inode's changes out to the inode's on disk home.
 * Originally based on xfs_iflush_int() from xfs_inode.c in the kernel.
 */
int
libxfs_iflush_int(
	struct xfs_inode		*ip,
	struct xfs_buf			*bp)
{
	struct xfs_inode_log_item	*iip;
	struct xfs_dinode		*dip;
	struct xfs_mount		*mp;

	ASSERT(ip->i_df.if_format != XFS_DINODE_FMT_BTREE ||
		ip->i_df.if_nextents > ip->i_df.if_ext_max);

	iip = ip->i_itemp;
	mp = ip->i_mount;

	/* set *dip = inode's place in the buffer */
	dip = xfs_buf_offset(bp, ip->i_imap.im_boffset);

	ASSERT(ip->i_d.di_magic == XFS_DINODE_MAGIC);
	if (XFS_ISREG(ip)) {
		ASSERT( (ip->i_df.if_format == XFS_DINODE_FMT_EXTENTS) ||
			(ip->i_df.if_format == XFS_DINODE_FMT_BTREE) );
	} else if (XFS_ISDIR(ip)) {
		ASSERT( (ip->i_df.if_format == XFS_DINODE_FMT_EXTENTS) ||
			(ip->i_df.if_format == XFS_DINODE_FMT_BTREE)   ||
			(ip->i_df.if_format == XFS_DINODE_FMT_LOCAL) );
	}
	ASSERT(ip->i_df.if_nextents+ip->i_afp->if_nextents <= ip->i_d.di_nblocks);
	ASSERT(ip->i_d.di_forkoff <= mp->m_sb.sb_inodesize);
	ASSERT(!(ip->i_d.di_flags2 & XFS_DIFLAG2_BIGTIME) &&
	       xfs_sb_version_hasbigtime(&mp->m_sb));

	/* bump the change count on v3 inodes */
	if (xfs_sb_version_has_v3inode(&mp->m_sb))
		VFS_I(ip)->i_version++;

	/*
	 * If there are inline format data / attr forks attached to this inode,
	 * make sure they are not corrupt.
	 */
	if (ip->i_df.if_format == XFS_DINODE_FMT_LOCAL &&
	    xfs_ifork_verify_local_data(ip))
		return -EFSCORRUPTED;
	if (ip->i_afp && ip->i_afp->if_format == XFS_DINODE_FMT_LOCAL &&
	    xfs_ifork_verify_local_attr(ip))
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
	struct xfs_ialloc_args	args = {
		.pip		= pip,
		.uid		= make_kuid(cr->cr_uid),
		.gid		= make_kgid(cr->cr_gid),
		.prid		= pip ? 0 : fsx->fsx_projid,
		.nlink		= nlink,
		.rdev		= rdev,
		.mode		= mode,
	};
	xfs_buf_t	*ialloc_context;
	xfs_inode_t	*ip;
	int		error;

	ialloc_context = (xfs_buf_t *)0;
	error = libxfs_ialloc(*tp, &args, &ialloc_context, &ip);
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
		error = libxfs_ialloc(*tp, &args, &ialloc_context, &ip);
		if (!ip)
			error = -ENOSPC;
		if (error)
			return error;
	}

	*ipp = ip;
	if (!pip)
		xfs_ialloc_fsx_init(tp, ip, fsx);
	return error;
}

/*
 * Inode cache stubs.
 */

struct kmem_zone		*xfs_inode_zone;
extern struct kmem_zone		*xfs_ili_zone;

int
libxfs_iget(
	struct xfs_mount	*mp,
	struct xfs_trans	*tp,
	xfs_ino_t		ino,
	uint			flags,
	struct xfs_inode	**ipp)
{
	struct xfs_inode	*ip;
	int			error = 0;

	ip = kmem_cache_zalloc(xfs_inode_zone, 0);
	if (!ip)
		return -ENOMEM;

	ip->i_ino = ino;
	ip->i_mount = mp;
	error = xfs_imap(mp, tp, ip->i_ino, &ip->i_imap, 0);
	if (error)
		goto out_destroy;

	/*
	 * For version 5 superblocks, if we are initialising a new inode and we
	 * are not utilising the XFS_MOUNT_IKEEP inode cluster mode, we can
	 * simply build the new inode core with a random generation number.
	 *
	 * For version 4 (and older) superblocks, log recovery is dependent on
	 * the di_flushiter field being initialised from the current on-disk
	 * value and hence we must also read the inode off disk even when
	 * initializing new inodes.
	 */
	if (xfs_sb_version_has_v3inode(&mp->m_sb) &&
	    (flags & XFS_IGET_CREATE) && !(mp->m_flags & XFS_MOUNT_IKEEP)) {
		VFS_I(ip)->i_generation = prandom_u32();
	} else {
		struct xfs_dinode	*dip;
		struct xfs_buf		*bp;

		error = xfs_imap_to_bp(mp, tp, &ip->i_imap, &dip, &bp, 0);
		if (error)
			goto out_destroy;

		error = xfs_inode_from_disk(ip, dip);
		if (!error)
			xfs_buf_set_ref(bp, XFS_INO_REF);
		xfs_trans_brelse(tp, bp);

		if (error)
			goto out_destroy;
	}

	*ipp = ip;
	return 0;

out_destroy:
	kmem_cache_free(xfs_inode_zone, ip);
	*ipp = NULL;
	return error;
}

static void
libxfs_idestroy(
	struct xfs_inode	*ip)
{
	switch (VFS_I(ip)->i_mode & S_IFMT) {
		case S_IFREG:
		case S_IFDIR:
		case S_IFLNK:
			libxfs_idestroy_fork(&ip->i_df);
			break;
	}
	if (ip->i_afp) {
		libxfs_idestroy_fork(ip->i_afp);
		kmem_cache_free(xfs_ifork_zone, ip->i_afp);
	}
	if (ip->i_cowfp) {
		libxfs_idestroy_fork(ip->i_cowfp);
		kmem_cache_free(xfs_ifork_zone, ip->i_cowfp);
	}
}

void
libxfs_irele(
	struct xfs_inode	*ip)
{
	ASSERT(ip->i_itemp == NULL);
	libxfs_idestroy(ip);
	kmem_cache_free(xfs_inode_zone, ip);
}
