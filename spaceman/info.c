// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "libxfs.h"
#include "command.h"
#include "init.h"
#include "libfrog/paths.h"
#include "libfrog/fsgeom.h"
#include "libfrog/fsproperties.h"
#include "libfrog/fsprops.h"
#include "space.h"

static void
info_help(void)
{
	printf(_(
"\n"
" Pretty-prints the filesystem geometry as derived from the superblock.\n"
" The output has the same format as mkfs.xfs, xfs_info, and other utilities.\n"
" The opened file must be an XFS mount point.\n"
"\n"
));

}

static int
info_f(
	int			argc,
	char			**argv)
{
	if (fs_table_lookup_mount(file->name) == NULL) {
		fprintf(stderr, _("%s: Not a XFS mount point.\n"), file->name);
		return 1;
	}

	xfs_report_geom(&file->xfd.fsgeom, file->fs_path.fs_name,
			file->fs_path.fs_log, file->fs_path.fs_rt);
	return 0;
}

static const struct cmdinfo info_cmd = {
	.name =		"info",
	.altname =	"i",
	.cfunc =	info_f,
	.argmin =	0,
	.argmax =	0,
	.canpush =	0,
	.args =		NULL,
	.flags =	CMD_FLAG_ONESHOT,
	.oneline =	N_("pretty-print superblock geometry info"),
	.help =		info_help,
};

static void
cfgfile_help(void)
{
	printf(_(
"\n"
" Print a mkfs.xfs configuration file for user-visible filesystem features\n"
" of the current filesystem.  Geometry information are not printed.\n"
"\n"
));

}

static int
get_autofsck(
	struct fileio		*f,
	enum fsprop_autofsck	*autofsck)
{
	struct fsprops_handle	fph = { };
	char			valuebuf[FSPROP_MAX_VALUELEN + 1] = { 0 };
	size_t			valuelen = FSPROP_MAX_VALUELEN;
	int			ret;

	*autofsck = FSPROP_AUTOFSCK_UNSET;

	ret = fsprops_open_handle(&f->xfd, &f->fs_path, &fph);
	if (ret)
		return ret;

	ret = fsprops_get(&fph, FSPROP_AUTOFSCK_NAME, valuebuf, &valuelen);
	if (ret == -1 && errno == ENODATA) {
		ret = 0;
		goto out_fph;
	}
	if (ret)
		goto out_fph;

	*autofsck = fsprop_autofsck_read(valuebuf);

out_fph:
	fsprops_free_handle(&fph);
	return ret;
}

static int
cfgfile_f(
	int			argc,
	char			**argv)
{
	struct fsxattr		fsx = { };
	enum fsprop_autofsck	autofsck;
	int			ret;

	if (fs_table_lookup_mount(file->name) == NULL) {
		fprintf(stderr, _("%s: Not a XFS mount point.\n"), file->name);
		return 1;
	}

	ret = ioctl(file->xfd.fd, FS_IOC_FSGETXATTR, &fsx);
	if (ret) {
		perror(file->name);
		return 1;
	}

	ret = get_autofsck(file, &autofsck);
	if (ret) {
		perror("autofsck");
		return 1;
	}

	xfrog_write_mkfs_config(&file->xfd.fsgeom, &fsx, autofsck, stdout);
	return 0;
}

static const struct cmdinfo cfgfile_cmd = {
	.name =		"cfgfile",
	.cfunc =	cfgfile_f,
	.argmin =	0,
	.argmax =	0,
	.canpush =	0,
	.args =		NULL,
	.flags =	CMD_FLAG_ONESHOT,
	.oneline =	N_("print mkfs.xfs configuration file"),
	.help =		cfgfile_help,
};

void
info_init(void)
{
	add_command(&info_cmd);
	add_command(&cfgfile_cmd);
}
