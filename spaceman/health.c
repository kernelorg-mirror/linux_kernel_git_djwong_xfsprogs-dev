// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2019 Oracle.
 * All Rights Reserved.
 */

#include "libxfs.h"
#include "command.h"
#include "init.h"
#include "path.h"
#include "space.h"
#include "input.h"
#include "fsgeom.h"

static struct xfs_fsop_geom fsgeo;
static cmdinfo_t health_cmd;

static bool has_realtime(const struct xfs_fsop_geom *g)
{
	return g->rtblocks > 0;
}

static bool has_finobt(const struct xfs_fsop_geom *g)
{
	return g->flags & XFS_FSOP_GEOM_FLAGS_FINOBT;
}

static bool has_rmapbt(const struct xfs_fsop_geom *g)
{
	return g->flags & XFS_FSOP_GEOM_FLAGS_RMAPBT;
}

static bool has_reflink(const struct xfs_fsop_geom *g)
{
	return g->flags & XFS_FSOP_GEOM_FLAGS_REFLINK;
}

struct flag_map {
	unsigned int		mask;
	bool			(*has_fn)(const struct xfs_fsop_geom *g);
	const char		*descr;
};

static const struct flag_map fs_flags[] = {
	{
		.mask = XFS_FSOP_GEOM_HEALTH_FS_COUNTERS,
		.descr = "summary counters",
	},
	{
		.mask = XFS_FSOP_GEOM_HEALTH_FS_UQUOTA,
		.descr = "user quota",
	},
	{
		.mask = XFS_FSOP_GEOM_HEALTH_FS_GQUOTA,
		.descr = "group quota",
	},
	{
		.mask = XFS_FSOP_GEOM_HEALTH_FS_PQUOTA,
		.descr = "project quota",
	},
	{
		.mask = XFS_FSOP_GEOM_HEALTH_RT_BITMAP,
		.descr = "realtime bitmap",
		.has_fn = has_realtime,
	},
	{
		.mask = XFS_FSOP_GEOM_HEALTH_RT_SUMMARY,
		.descr = "realtime summary",
		.has_fn = has_realtime,
	},
	{0},
};

static const struct flag_map ag_flags[] = {
	{
		.mask = XFS_AG_GEOM_HEALTH_AG_SB,
		.descr = "superblock",
	},
	{
		.mask = XFS_AG_GEOM_HEALTH_AG_AGF,
		.descr = "AGF header",
	},
	{
		.mask = XFS_AG_GEOM_HEALTH_AG_AGFL,
		.descr = "AGFL header",
	},
	{
		.mask = XFS_AG_GEOM_HEALTH_AG_AGI,
		.descr = "AGI header",
	},
	{
		.mask = XFS_AG_GEOM_HEALTH_AG_BNOBT,
		.descr = "free space by block btree",
	},
	{
		.mask = XFS_AG_GEOM_HEALTH_AG_CNTBT,
		.descr = "free space by length btree",
	},
	{
		.mask = XFS_AG_GEOM_HEALTH_AG_INOBT,
		.descr = "inode btree",
	},
	{
		.mask = XFS_AG_GEOM_HEALTH_AG_FINOBT,
		.descr = "free inode btree",
		.has_fn = has_finobt,
	},
	{
		.mask = XFS_AG_GEOM_HEALTH_AG_RMAPBT,
		.descr = "reverse mappings btree",
		.has_fn = has_rmapbt,
	},
	{
		.mask = XFS_AG_GEOM_HEALTH_AG_REFCNTBT,
		.descr = "reference count btree",
		.has_fn = has_reflink,
	},
	{0},
};

static const struct flag_map inode_flags[] = {
	{
		.mask = XFS_BS_HEALTH_INODE,
		.descr = "inode core",
	},
	{
		.mask = XFS_BS_HEALTH_BMBTD,
		.descr = "data fork",
	},
	{
		.mask = XFS_BS_HEALTH_BMBTA,
		.descr = "extended attribute fork",
	},
	{
		.mask = XFS_BS_HEALTH_BMBTC,
		.descr = "copy on write fork",
	},
	{
		.mask = XFS_BS_HEALTH_DIR,
		.descr = "directory",
	},
	{
		.mask = XFS_BS_HEALTH_XATTR,
		.descr = "extended attributes",
	},
	{
		.mask = XFS_BS_HEALTH_SYMLINK,
		.descr = "symbolic link target",
	},
	{
		.mask = XFS_BS_HEALTH_PARENT,
		.descr = "parent pointers",
	},
	{0},
};

/* Convert a flag mask to a report. */
static void
report_health(
	const char			*descr,
	const struct flag_map		*maps,
	unsigned int			mask)
{
	const struct flag_map		*f;
	bool				b;

	for (f = maps; f->mask != 0; f++) {
		if (f->has_fn && !f->has_fn(&fsgeo))
			continue;
		b = mask & f->mask;
		printf("%s %s: %s\n", descr, _(f->descr),
				b ? _("unhealthy") : _("ok"));
	}
}

/* Report on an AG's health. */
static int
report_ag_health(
	xfs_agnumber_t		agno)
{
	struct xfs_ag_geometry	ageo;
	char			descr[256];
	int			ret;

	ageo.ag_number = agno;
	ret = ioctl(file->fd, XFS_IOC_AG_GEOMETRY, &ageo);
	if (ret) {
		perror("ag_geometry");
		return 1;
	}
	snprintf(descr, sizeof(descr) - 1, _("AG %u"), agno);
	report_health(descr, ag_flags, ageo.ag_health);
	return 0;
}

/* Report on an inode's health. */
static int
report_inode_health(
	unsigned long long	ino,
	const char		*descr)
{
	struct xfs_bstat	bs;
	char			d[256];
	__u64			oneino = ino;
	__s32			onelen = 0;
	int			ret;
	struct xfs_fsop_bulkreq	onereq = {
		.lastip		= &oneino,
		.icount		= 1,
		.ocount		= &onelen,
		.ubuffer	= &bs,
	};

	if (!descr) {
		snprintf(d, sizeof(d) - 1, _("inode %llu"), ino);
		descr = d;
	}

	ret = ioctl(file->fd, XFS_IOC_FSBULKSTAT_SINGLE, &onereq);
	if (ret) {
		perror(descr);
		return 1;
	}

	report_health(descr, inode_flags, bs.bs_health);
	return 0;
}

/* Report on a file's health. */
static int
report_file_health(
	const char	*path)
{
	struct stat	stata, statb;
	int		ret;

	ret = lstat(path, &statb);
	if (ret) {
		perror(path);
		return 1;
	}

	ret = fstat(file->fd, &stata);
	if (ret) {
		perror(file->name);
		return 1;
	}

	if (stata.st_dev != statb.st_dev) {
		fprintf(stderr, _("%s: not on the open filesystem"), path);
		return 1;
	}

	return report_inode_health(statb.st_ino, path);
}

/* Report on health problems in XFS filesystem. */
static int
health_f(
	int			argc,
	char			**argv)
{
	unsigned long long	x;
	xfs_agnumber_t		agno;
	bool			default_report = true;
	int			c;
	int			ret;

	ret = xfs_fsgeometry(file->fd, &fsgeo);
	if (ret) {
		perror("fsgeometry");
		return 1;
	}

	if (fsgeo.version != XFS_FSOP_GEOM_V5) {
		fprintf(stderr, "health: reporting not supported by kernel.\n");
		return 1;
	}

	while ((c = getopt(argc, argv, "a:fi:")) != EOF) {
		switch (c) {
		case 'a':
			default_report = false;
			errno = 0;
			x = strtoll(optarg, NULL, 10);
			if (!errno && x >= NULLAGNUMBER)
				errno = ERANGE;
			if (errno) {
				perror("ag health");
				return 1;
			}
			agno = x;
			ret = report_ag_health(agno);
			break;
		case 'f':
			default_report = false;
			report_health(_("filesystem"), fs_flags, fsgeo.health);
			break;
		case 'i':
			default_report = false;
			errno = 0;
			x = strtoll(optarg, NULL, 10);
			if (errno) {
				perror("inode health");
				return 1;
			}
			ret = report_inode_health(x, NULL);
			break;
	case '?':
		default:
			return command_usage(&health_cmd);
		}
	}

	for (c = optind; c < argc; c++) {
		default_report = false;
		ret = report_file_health(argv[c]);
		if (ret)
			return 1;
	}

	/* No arguments gets us a summary of fs state. */
	if (!default_report)
		return 0;

	report_health(_("filesystem"), fs_flags, fsgeo.health);

	for (agno = 0; agno < fsgeo.agcount; agno++) {
		ret = report_ag_health(agno);
		if (ret)
			return 1;
	}

	return 0;
}

static void
health_help(void)
{
	printf(_(
"\n"
"Report all observed filesystem health problems.\n"
"\n"
" -a agno  -- Report health of the given allocation group.\n"
" -f       -- Report health of the overall filesystem.\n"
" -i inum  -- Report health of a given inode number.\n"
" paths    -- Report health of the given file path.\n"
"\n"));

}

static cmdinfo_t health_cmd = {
	.name = "health",
	.cfunc = health_f,
	.argmin = 0,
	.argmax = -1,
	.args = "[-a agno] [-f] [-i inum] [paths]",
	.flags = CMD_FLAG_ONESHOT,
	.help = health_help,
};

void
health_init(void)
{
	health_cmd.oneline = _("Report observed XFS health problems."),
	add_command(&health_cmd);
}
