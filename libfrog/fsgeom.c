// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc. All Rights Reserved.
 */
#include "platform_defs.h"
#include "xfs.h"
#include "bitops.h"
#include "fsgeom.h"
#include "util.h"
#include "list.h"
#include "libfrog/fsproperties.h"

static inline const char *
rtdev_name(
	struct xfs_fsop_geom	*geo,
	const char		*rtname)
{
	if (!geo->rtblocks)
		return _("none");
	if (geo->rtstart)
		return _("internal");
	if (!rtname)
		return _("external");
	return rtname;
}

void
xfs_report_geom(
	struct xfs_fsop_geom	*geo,
	const char		*mntpoint,
	const char		*logname,
	const char		*rtname)
{
	int			isint;
	int			lazycount;
	int			dirversion;
	int			logversion;
	int			attrversion;
	int			projid32bit;
	int			crcs_enabled;
	int			cimode;
	int			ftype_enabled;
	int			finobt_enabled;
	int			spinodes;
	int			rmapbt_enabled;
	int			reflink_enabled;
	int			bigtime_enabled;
	int			inobtcount;
	int			nrext64;
	int			exchangerange;
	int			parent;
	int			metadir;
	int			zoned;

	isint = geo->logstart > 0;
	lazycount = geo->flags & XFS_FSOP_GEOM_FLAGS_LAZYSB ? 1 : 0;
	dirversion = geo->flags & XFS_FSOP_GEOM_FLAGS_DIRV2 ? 2 : 1;
	logversion = geo->flags & XFS_FSOP_GEOM_FLAGS_LOGV2 ? 2 : 1;
	attrversion = geo->flags & XFS_FSOP_GEOM_FLAGS_ATTR2 ? 2 : \
			(geo->flags & XFS_FSOP_GEOM_FLAGS_ATTR ? 1 : 0);
	cimode = geo->flags & XFS_FSOP_GEOM_FLAGS_DIRV2CI ? 1 : 0;
	projid32bit = geo->flags & XFS_FSOP_GEOM_FLAGS_PROJID32 ? 1 : 0;
	crcs_enabled = geo->flags & XFS_FSOP_GEOM_FLAGS_V5SB ? 1 : 0;
	ftype_enabled = geo->flags & XFS_FSOP_GEOM_FLAGS_FTYPE ? 1 : 0;
	finobt_enabled = geo->flags & XFS_FSOP_GEOM_FLAGS_FINOBT ? 1 : 0;
	spinodes = geo->flags & XFS_FSOP_GEOM_FLAGS_SPINODES ? 1 : 0;
	rmapbt_enabled = geo->flags & XFS_FSOP_GEOM_FLAGS_RMAPBT ? 1 : 0;
	reflink_enabled = geo->flags & XFS_FSOP_GEOM_FLAGS_REFLINK ? 1 : 0;
	bigtime_enabled = geo->flags & XFS_FSOP_GEOM_FLAGS_BIGTIME ? 1 : 0;
	inobtcount = geo->flags & XFS_FSOP_GEOM_FLAGS_INOBTCNT ? 1 : 0;
	nrext64 = geo->flags & XFS_FSOP_GEOM_FLAGS_NREXT64 ? 1 : 0;
	exchangerange = geo->flags & XFS_FSOP_GEOM_FLAGS_EXCHANGE_RANGE ? 1 : 0;
	parent = geo->flags & XFS_FSOP_GEOM_FLAGS_PARENT ? 1 : 0;
	metadir = geo->flags & XFS_FSOP_GEOM_FLAGS_METADIR ? 1 : 0;
	zoned = geo->flags & XFS_FSOP_GEOM_FLAGS_ZONED ? 1 : 0;

	printf(_(
"meta-data=%-22s isize=%-6d agcount=%u, agsize=%u blks\n"
"         =%-22s sectsz=%-5u attr=%u, projid32bit=%u\n"
"         =%-22s crc=%-8u finobt=%u, sparse=%u, rmapbt=%u\n"
"         =%-22s reflink=%-4u bigtime=%u inobtcount=%u nrext64=%u\n"
"         =%-22s exchange=%-3u metadir=%u\n"
"data     =%-22s bsize=%-6u blocks=%llu, imaxpct=%u\n"
"         =%-22s sunit=%-6u swidth=%u blks\n"
"naming   =version %-14u bsize=%-6u ascii-ci=%d, ftype=%d, parent=%d\n"
"log      =%-22s bsize=%-6d blocks=%u, version=%d\n"
"         =%-22s sectsz=%-5u sunit=%d blks, lazy-count=%d\n"
"realtime =%-22s extsz=%-6d blocks=%lld, rtextents=%lld\n"
"         =%-22s rgcount=%-4d rgsize=%u extents\n"
"         =%-22s zoned=%-6d start=%llu reserved=%llu\n"),
		mntpoint, geo->inodesize, geo->agcount, geo->agblocks,
		"", geo->sectsize, attrversion, projid32bit,
		"", crcs_enabled, finobt_enabled, spinodes, rmapbt_enabled,
		"", reflink_enabled, bigtime_enabled, inobtcount, nrext64,
		"", exchangerange, metadir,
		"", geo->blocksize, (unsigned long long)geo->datablocks,
			geo->imaxpct,
		"", geo->sunit, geo->swidth,
		dirversion, geo->dirblocksize, cimode, ftype_enabled, parent,
		isint ? _("internal log") : logname ? logname : _("external"),
			geo->blocksize, geo->logblocks, logversion,
		"", geo->logsectsize, geo->logsunit / geo->blocksize, lazycount,
		rtdev_name(geo, rtname),
		geo->rtextsize * geo->blocksize, (unsigned long long)geo->rtblocks,
			(unsigned long long)geo->rtextents,
		"", geo->rgcount, geo->rgextents,
		"", zoned, geo->rtstart, geo->rtreserved);
}

/* Try to obtain the xfs geometry.  On error returns a negative error code. */
int
xfrog_geometry(
	int			fd,
	struct xfs_fsop_geom	*fsgeo)
{
	int			ret;

	memset(fsgeo, 0, sizeof(*fsgeo));

	ret = ioctl(fd, XFS_IOC_FSGEOMETRY, fsgeo);
	if (!ret)
		return 0;

	ret = ioctl(fd, XFS_IOC_FSGEOMETRY_V4, fsgeo);
	if (!ret)
		return 0;

	ret = ioctl(fd, XFS_IOC_FSGEOMETRY_V1, fsgeo);
	if (!ret)
		return 0;

	return -errno;
}

/* Compute conversion factors of an xfs_fd structure. */
static void
xfd_compute_conversion_factors(
	struct xfs_fd		*xfd)
{
	xfd->agblklog = log2_roundup(xfd->fsgeom.agblocks);
	xfd->blocklog = highbit32(xfd->fsgeom.blocksize);
	xfd->inodelog = highbit32(xfd->fsgeom.inodesize);
	xfd->inopblog = xfd->blocklog - xfd->inodelog;
	xfd->aginolog = xfd->agblklog + xfd->inopblog;
	xfd->blkbb_log = xfd->blocklog - BBSHIFT;
}

/*
 * Prepare xfs_fd structure for future ioctl operations by computing the xfs
 * geometry for @xfd->fd.  Returns zero or a negative error code.
 */
int
xfd_prepare_geometry(
	struct xfs_fd		*xfd)
{
	int			ret;

	ret = xfrog_geometry(xfd->fd, &xfd->fsgeom);
	if (ret)
		return ret;

	xfd_compute_conversion_factors(xfd);
	return 0;
}

/*
 * Prepare xfs_fd structure for future ioctl operations by computing the xfs
 * geometry for @xfd->fd.  Returns zero or a negative error code.
 */
void
xfd_install_geometry(
	struct xfs_fd		*xfd,
	struct xfs_fsop_geom	*fsgeom)
{
	memcpy(&xfd->fsgeom, fsgeom, sizeof(*fsgeom));
	xfd_compute_conversion_factors(xfd);
}

/* Open a file on an XFS filesystem.  Returns zero or a negative error code. */
int
xfd_open(
	struct xfs_fd		*xfd,
	const char		*pathname,
	int			flags)
{
	int			ret;

	xfd->fd = open(pathname, flags);
	if (xfd->fd < 0)
		return -errno;

	ret = xfd_prepare_geometry(xfd);
	if (ret) {
		xfd_close(xfd);
		return ret;
	}

	return 0;
}

/*
 * Release any resources associated with this xfs_fd structure.  Returns zero
 * or a negative error code.
 */
int
xfd_close(
	struct xfs_fd		*xfd)
{
	int			ret = 0;

	if (xfd->fd < 0)
		return 0;

	ret = close(xfd->fd);
	xfd->fd = -1;
	if (ret < 0)
		return -errno;

	return 0;
}

/* Try to obtain an AG's geometry.  Returns zero or a negative error code. */
int
xfrog_ag_geometry(
	int			fd,
	unsigned int		agno,
	struct xfs_ag_geometry	*ageo)
{
	int			ret;

	ageo->ag_number = agno;
	ret = ioctl(fd, XFS_IOC_AG_GEOMETRY, ageo);
	if (ret)
		return -errno;
	return 0;
}

/*
 * Try to obtain a rt group's geometry.  Returns zero or a negative error code.
 */
int
xfrog_rtgroup_geometry(
	int			fd,
	unsigned int		rgno,
	struct xfs_rtgroup_geometry	*rgeo)
{
	int			ret;

	rgeo->rg_number = rgno;
	ret = ioctl(fd, XFS_IOC_RTGROUP_GEOMETRY, rgeo);
	if (ret)
		return -errno;
	return 0;
}

enum {
	M_CRC = 0,
	M_FINOBT,
	M_RMAPBT,
	M_REFLINK,
	M_INOBTCNT,
	M_BIGTIME,
	M_METADIR,
	M_AUTOFSCK,
	M_MAX_OPTS,
};

enum {
	D_RTINHERIT = 0,
	D_PROJINHERIT,
	D_EXTSZINHERIT,
	D_COWEXTSIZE,
	D_DAXINHERIT,
	D_MAX_OPTS,
};

enum {
	I_SPINODES = 0,
	I_NREXT64,
	I_EXCHANGE,
	I_MAX_OPTS,
};

enum {
	N_PARENT = 0,
	N_FTYPE,
	N_VERSION,
	N_MAX_OPTS,
};

enum {
	R_EXTSIZE = 0,
	R_MAX_OPTS,
};

struct mkfs_config_opt;

struct mkfs_config_data {
	const struct xfs_fsop_geom	*fsgeo;
	const struct fsxattr		*fsx;
	enum fsprop_autofsck		autofsck;
};

typedef int (*opt_print_fn)(const struct mkfs_config_opt *opt,
			    const struct mkfs_config_data *data,
			    FILE *fp);

struct mkfs_config_opt {
	const char		*name;
	uint64_t		fsgeom_flag;
	uint64_t		xflags_flag;
	opt_print_fn		print_fn;
};

struct mkfs_config_section {
	const char		*ini_section;
	struct mkfs_config_opt	subopts[16];
};

static int
print_fsgeom(
	const struct mkfs_config_opt	*opt,
	const struct mkfs_config_data	*data,
	FILE				*fp)
{
	const struct xfs_fsop_geom	*fsgeo = data->fsgeo;
	int				ret;

	ret = fprintf(fp, "%s=%d\n", opt->name,
			!!(fsgeo->flags & opt->fsgeom_flag));
	if (ret <= 0)
		return ret;
	return 0;
}

static int
print_xflag(
	const struct mkfs_config_opt	*opt,
	const struct mkfs_config_data	*data,
	FILE				*fp)
{
	const struct fsxattr		*fsx = data->fsx;
	int				ret;

	if (!(fsx->fsx_xflags & opt->xflags_flag))
		return 0;

	ret = fprintf(fp, "%s=%d\n", opt->name, 1);
	if (ret <= 0)
		return ret;
	return 0;
}

static int
print_projinherit(
	const struct mkfs_config_opt	*opt,
	const struct mkfs_config_data	*data,
	FILE				*fp)
{
	const struct fsxattr		*fsx = data->fsx;
	int				ret;

	if (fsx->fsx_xflags & FS_XFLAG_PROJINHERIT) {
		ret = fprintf(fp, "%s=%u\n", opt->name, fsx->fsx_projid);
		if (ret <= 0)
			return ret;
	}

	return 0;
}

static int
print_extszinherit(
	const struct mkfs_config_opt	*opt,
	const struct mkfs_config_data	*data,
	FILE				*fp)
{
	const struct fsxattr		*fsx = data->fsx;
	int				ret;

	if (fsx->fsx_xflags & FS_XFLAG_EXTSZINHERIT) {
		ret = fprintf(fp, "%s=%u\n", opt->name, fsx->fsx_extsize);
		if (ret <= 0)
			return ret;
	}

	return 0;
}

static int
print_cowextszinherit(
	const struct mkfs_config_opt	*opt,
	const struct mkfs_config_data	*data,
	FILE				*fp)
{
	const struct fsxattr		*fsx = data->fsx;
	int				ret;

	if (fsx->fsx_xflags & FS_XFLAG_COWEXTSIZE) {
		ret = fprintf(fp, "%s=%u\n", opt->name, fsx->fsx_cowextsize);
		if (ret <= 0)
			return ret;
	}

	return 0;
}

static int
print_rtextsize(
	const struct mkfs_config_opt	*opt,
	const struct mkfs_config_data	*data,
	FILE				*fp)
{
	const struct xfs_fsop_geom	*fsgeo = data->fsgeo;
	int				ret;

	if (fsgeo->rtextsize > 1) {
		ret = fprintf(fp, "%s=%u\n", opt->name,
				fsgeo->rtextsize * fsgeo->blocksize);
		if (ret <= 0)
			return ret;
	}

	return 0;
}

static int
print_dirversion(
	const struct mkfs_config_opt	*opt,
	const struct mkfs_config_data	*data,
	FILE				*fp)
{
	const struct xfs_fsop_geom	*fsgeo = data->fsgeo;
	int				ret;

	if (fsgeo->flags & XFS_FSOP_GEOM_FLAGS_DIRV2CI)
		ret = fprintf(fp, "%s=ci\n", opt->name);
	else
		ret = fprintf(fp, "%s=2\n", opt->name);
	if (ret <= 0)
		return ret;
	return 0;
}

static int
print_autofsck(
	const struct mkfs_config_opt	*opt,
	const struct mkfs_config_data	*data,
	FILE				*fp)
{
	int				ret;
	const char			*value =
		fsprop_autofsck_write(data->autofsck);

	if (value) {
		ret = fprintf(fp, "%s=%s\n", opt->name, value);
		if (ret <= 0)
			return ret;
	}

	return 0;
}

static const struct mkfs_config_section config_sections[] = {
	{
		.ini_section = "metadata",
		.subopts = {
			[M_CRC] = {
				.name		= "crc",
				.fsgeom_flag	= XFS_FSOP_GEOM_FLAGS_V5SB,
			},
			[M_FINOBT] = {
				.name		= "finobt",
				.fsgeom_flag	= XFS_FSOP_GEOM_FLAGS_FINOBT,
			},
			[M_RMAPBT] = {
				.name		= "rmapbt",
				.fsgeom_flag	= XFS_FSOP_GEOM_FLAGS_RMAPBT,
			},
			[M_REFLINK] = {
				.name		= "reflink",
				.fsgeom_flag	= XFS_FSOP_GEOM_FLAGS_REFLINK,
			},
			[M_INOBTCNT] = {
				.name		= "inobtcount",
				.fsgeom_flag	= XFS_FSOP_GEOM_FLAGS_INOBTCNT,
			},
			[M_BIGTIME] = {
				.name		= "bigtime",
				.fsgeom_flag	= XFS_FSOP_GEOM_FLAGS_BIGTIME,
			},
			[M_METADIR] = {
				.name		= "metadir",
				.fsgeom_flag	= XFS_FSOP_GEOM_FLAGS_METADIR,
			},
			[M_AUTOFSCK] = {
				.name		= "autofsck",
				.print_fn	= print_autofsck,
			},
			[M_MAX_OPTS] = { },
		},
	},
	{
		.ini_section			= "data",
		.subopts = {
			[D_RTINHERIT] = {
				.name		= "rtinherit",
				.xflags_flag	= FS_XFLAG_RTINHERIT,
			},
			[D_PROJINHERIT] = {
				.name		= "projinherit",
				.print_fn	= print_projinherit,
			},
			[D_EXTSZINHERIT] = {
				.name		= "extszinherit",
				.print_fn	= print_extszinherit,
			},
			[D_COWEXTSIZE] = {
				.name		= "cowextsize",
				.print_fn	= print_cowextszinherit,
			},
			[D_DAXINHERIT] = {
				.name		= "daxinherit",
				.xflags_flag	= FS_XFLAG_DAX,
			},
			[D_MAX_OPTS] = { },
		},
	},
	{
		.ini_section			= "inode",
		.subopts = {
			[I_SPINODES] = {
				.name		= "sparse",
				.fsgeom_flag	= XFS_FSOP_GEOM_FLAGS_SPINODES,
			},
			[I_NREXT64] = {
				.name		= "nrext64",
				.fsgeom_flag	= XFS_FSOP_GEOM_FLAGS_NREXT64,
			},
			[I_EXCHANGE] = {
				.name		= "exchange",
				.fsgeom_flag	= XFS_FSOP_GEOM_FLAGS_EXCHANGE_RANGE,
			},
			[I_MAX_OPTS] = { },
		},
	},
	{
		.ini_section = "naming",
		.subopts = {
			[N_PARENT] = {
				.name		= "parent",
				.fsgeom_flag	= XFS_FSOP_GEOM_FLAGS_PARENT,
			},
			[N_FTYPE] = {
				.name		= "ftype",
				.fsgeom_flag	= XFS_FSOP_GEOM_FLAGS_FTYPE,
			},
			[N_VERSION] = {
				.name		= "version",
				.print_fn	= print_dirversion,
			},
		},
	},
	{
		.ini_section = "realtime",
		.subopts = {
			[R_EXTSIZE] = {
				.name		= "extsize",
				.print_fn	= print_rtextsize,
			},
			[R_MAX_OPTS] = { },
		},
	},
};

/*
 * Write a mkfs.xfs configuration file for the user-visible filesystem features
 * enabled in the corresponding fs geometry and root directory file attribute
 * structures.
 *
 * Note: The regular and COW extent size hints must be in units of fsblocks,
 * not bytes.
 */
int
xfrog_write_mkfs_config(
	const struct xfs_fsop_geom		*fsgeo,
	const struct fsxattr			*fsx,
	int					autofsck,
	FILE					*fp)
{
	struct mkfs_config_data			d = {
		.fsgeo				= fsgeo,
		.fsx				= fsx,
		.autofsck			= autofsck,
	};
	const struct mkfs_config_section	*section = config_sections;
	const struct mkfs_config_opt		*opt;
	int					i, j;
	int					error;

	for (i = 0; i < ARRAY_SIZE(config_sections); i++, section++) {
		if (i > 0) {
			error = fprintf(fp, "\n");
			if (error <= 0)
				return error;
		}

		error = fprintf(fp, "[%s]\n", section->ini_section);
		if (error <= 0)
			return error;

		opt = section->subopts;
		for (j = 0;
		     j < ARRAY_SIZE(section->subopts) && opt->name;
		     j++, opt++) {
			if (opt->print_fn)
				error = opt->print_fn(opt, &d, fp);
			else if (opt->xflags_flag)
				error = print_xflag(opt, &d, fp);
			else if (opt->fsgeom_flag)
				error = print_fsgeom(opt, &d, fp);
			if (error)
				return error;
		}
	}

	return fflush(fp);
}
