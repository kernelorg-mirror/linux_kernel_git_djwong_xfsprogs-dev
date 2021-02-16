// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2019 Oracle, Inc.
 * All Rights Reserved.
 */
#include "xfs.h"
#include "fsgeom.h"
#include "scrub.h"

/* These must correspond to XFS_SCRUB_TYPE_ */
const struct xfrog_scrub_descr xfrog_scrubbers[XFS_SCRUB_TYPE_NR] = {
	[XFS_SCRUB_TYPE_PROBE] = {
		.name	= "probe",
		.descr	= "metadata",
		.group	= XFROG_SCRUB_GROUP_NONE,
	},
	[XFS_SCRUB_TYPE_SB] = {
		.name	= "sb",
		.descr	= "superblock",
		.group	= XFROG_SCRUB_GROUP_AGHEADER,
	},
	[XFS_SCRUB_TYPE_AGF] = {
		.name	= "agf",
		.descr	= "free space header",
		.group	= XFROG_SCRUB_GROUP_AGHEADER,
	},
	[XFS_SCRUB_TYPE_AGFL] = {
		.name	= "agfl",
		.descr	= "free list",
		.group	= XFROG_SCRUB_GROUP_AGHEADER,
	},
	[XFS_SCRUB_TYPE_AGI] = {
		.name	= "agi",
		.descr	= "inode header",
		.group	= XFROG_SCRUB_GROUP_AGHEADER,
	},
	[XFS_SCRUB_TYPE_BNOBT] = {
		.name	= "bnobt",
		.descr	= "freesp by block btree",
		.group	= XFROG_SCRUB_GROUP_PERAG,
	},
	[XFS_SCRUB_TYPE_CNTBT] = {
		.name	= "cntbt",
		.descr	= "freesp by length btree",
		.group	= XFROG_SCRUB_GROUP_PERAG,
	},
	[XFS_SCRUB_TYPE_INOBT] = {
		.name	= "inobt",
		.descr	= "inode btree",
		.group	= XFROG_SCRUB_GROUP_PERAG,
	},
	[XFS_SCRUB_TYPE_FINOBT] = {
		.name	= "finobt",
		.descr	= "free inode btree",
		.group	= XFROG_SCRUB_GROUP_PERAG,
	},
	[XFS_SCRUB_TYPE_RMAPBT] = {
		.name	= "rmapbt",
		.descr	= "reverse mapping btree",
		.group	= XFROG_SCRUB_GROUP_PERAG,
	},
	[XFS_SCRUB_TYPE_REFCNTBT] = {
		.name	= "refcountbt",
		.descr	= "reference count btree",
		.group	= XFROG_SCRUB_GROUP_PERAG,
	},
	[XFS_SCRUB_TYPE_INODE] = {
		.name	= "inode",
		.descr	= "inode record",
		.group	= XFROG_SCRUB_GROUP_INODE,
	},
	[XFS_SCRUB_TYPE_BMBTD] = {
		.name	= "bmapbtd",
		.descr	= "data block map",
		.group	= XFROG_SCRUB_GROUP_INODE,
	},
	[XFS_SCRUB_TYPE_BMBTA] = {
		.name	= "bmapbta",
		.descr	= "attr block map",
		.group	= XFROG_SCRUB_GROUP_INODE,
	},
	[XFS_SCRUB_TYPE_BMBTC] = {
		.name	= "bmapbtc",
		.descr	= "CoW block map",
		.group	= XFROG_SCRUB_GROUP_INODE,
	},
	[XFS_SCRUB_TYPE_DIR] = {
		.name	= "directory",
		.descr	= "directory entries",
		.group	= XFROG_SCRUB_GROUP_INODE,
	},
	[XFS_SCRUB_TYPE_XATTR] = {
		.name	= "xattr",
		.descr	= "extended attributes",
		.group	= XFROG_SCRUB_GROUP_INODE,
	},
	[XFS_SCRUB_TYPE_SYMLINK] = {
		.name	= "symlink",
		.descr	= "symbolic link",
		.group	= XFROG_SCRUB_GROUP_INODE,
	},
	[XFS_SCRUB_TYPE_PARENT] = {
		.name	= "parent",
		.descr	= "parent pointer",
		.group	= XFROG_SCRUB_GROUP_INODE,
	},
	[XFS_SCRUB_TYPE_RTBITMAP] = {
		.name	= "rtbitmap",
		.descr	= "realtime bitmap",
		.group	= XFROG_SCRUB_GROUP_FS,
	},
	[XFS_SCRUB_TYPE_RTSUM] = {
		.name	= "rtsummary",
		.descr	= "realtime summary",
		.group	= XFROG_SCRUB_GROUP_FS,
	},
	[XFS_SCRUB_TYPE_UQUOTA] = {
		.name	= "usrquota",
		.descr	= "user quotas",
		.group	= XFROG_SCRUB_GROUP_FS,
	},
	[XFS_SCRUB_TYPE_GQUOTA] = {
		.name	= "grpquota",
		.descr	= "group quotas",
		.group	= XFROG_SCRUB_GROUP_FS,
	},
	[XFS_SCRUB_TYPE_PQUOTA] = {
		.name	= "prjquota",
		.descr	= "project quotas",
		.group	= XFROG_SCRUB_GROUP_FS,
	},
	[XFS_SCRUB_TYPE_FSCOUNTERS] = {
		.name	= "fscounters",
		.descr	= "filesystem summary counters",
		.group	= XFROG_SCRUB_GROUP_SUMMARY,
	},
	[XFS_SCRUB_TYPE_QUOTACHECK] = {
		.name	= "quotacheck",
		.descr	= "quota counters",
		.group	= XFROG_SCRUB_GROUP_SUMMARY,
	},
	[XFS_SCRUB_TYPE_HEALTHY] = {
		.name	= "healthy",
		.descr	= "retained health records",
		.group	= XFROG_SCRUB_GROUP_NONE,
	},
	[XFS_SCRUB_TYPE_RTRMAPBT] = {
		.name	= "rtrmapbt",
		.descr	= "realtime reverse mapping btree",
		.group	= XFROG_SCRUB_GROUP_FS,
	},
	[XFS_SCRUB_TYPE_RTREFCBT] = {
		.name	= "rtrefcountbt",
		.descr	= "realtime reference count btree",
		.group	= XFROG_SCRUB_GROUP_FS,
	},
};

/*
 * Bitmap showing the full correctness dependencies of each scrub type.
 * Note that scrub types for one fs object type (ag, inode, fs) cannot declare
 * dependencies on scrub types for a different object type.
 */
#define B(x) (1U << (x))
const unsigned int xfrog_scrubber_deps[XFS_SCRUB_TYPE_NR] = {
	[XFS_SCRUB_TYPE_PROBE]		= 0,
	[XFS_SCRUB_TYPE_SB]		= 0,
	[XFS_SCRUB_TYPE_AGF]		= B(XFS_SCRUB_TYPE_SB),
	[XFS_SCRUB_TYPE_AGFL]		= B(XFS_SCRUB_TYPE_SB) |
					  B(XFS_SCRUB_TYPE_AGF),
	[XFS_SCRUB_TYPE_AGI]		= B(XFS_SCRUB_TYPE_SB),
	[XFS_SCRUB_TYPE_BNOBT]		= B(XFS_SCRUB_TYPE_AGF),
	[XFS_SCRUB_TYPE_CNTBT]		= B(XFS_SCRUB_TYPE_AGF),
	[XFS_SCRUB_TYPE_INOBT]		= B(XFS_SCRUB_TYPE_AGI),
	[XFS_SCRUB_TYPE_FINOBT]		= B(XFS_SCRUB_TYPE_AGI),
	[XFS_SCRUB_TYPE_RMAPBT]		= B(XFS_SCRUB_TYPE_AGF),
	[XFS_SCRUB_TYPE_REFCNTBT]	= B(XFS_SCRUB_TYPE_AGF),

	[XFS_SCRUB_TYPE_INODE]		= 0,
	[XFS_SCRUB_TYPE_BMBTD]		= B(XFS_SCRUB_TYPE_INODE),
	[XFS_SCRUB_TYPE_BMBTA]		= B(XFS_SCRUB_TYPE_INODE),
	[XFS_SCRUB_TYPE_BMBTC]		= B(XFS_SCRUB_TYPE_INODE),
	[XFS_SCRUB_TYPE_DIR]		= B(XFS_SCRUB_TYPE_BMBTD),
	[XFS_SCRUB_TYPE_XATTR]		= B(XFS_SCRUB_TYPE_BMBTA),
	[XFS_SCRUB_TYPE_SYMLINK]	= B(XFS_SCRUB_TYPE_BMBTD),
	[XFS_SCRUB_TYPE_PARENT]		= B(XFS_SCRUB_TYPE_BMBTD),

	[XFS_SCRUB_TYPE_RTBITMAP]	= 0,
	[XFS_SCRUB_TYPE_RTSUM]		= 0,
	[XFS_SCRUB_TYPE_UQUOTA]		= 0,
	[XFS_SCRUB_TYPE_GQUOTA]		= 0,
	[XFS_SCRUB_TYPE_PQUOTA]		= 0,
	[XFS_SCRUB_TYPE_FSCOUNTERS]	= 0,
	[XFS_SCRUB_TYPE_QUOTACHECK]	= B(XFS_SCRUB_TYPE_UQUOTA) |
					  B(XFS_SCRUB_TYPE_GQUOTA) |
					  B(XFS_SCRUB_TYPE_PQUOTA),
	[XFS_SCRUB_TYPE_HEALTHY]	= 0,
	[XFS_SCRUB_TYPE_RTRMAPBT]	= 0,
	[XFS_SCRUB_TYPE_RTREFCBT]	= 0,
};
#undef B

/* Invoke the scrub ioctl.  Returns zero or negative error code. */
int
xfrog_scrub_metadata(
	struct xfs_fd			*xfd,
	struct xfs_scrub_metadata	*meta)
{
	int				ret;

	ret = ioctl(xfd->fd, XFS_IOC_SCRUB_METADATA, meta);
	if (ret)
		return -errno;

	return 0;
}
