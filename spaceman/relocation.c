// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 Red Hat, Inc.
 * All Rights Reserved.
 */

#include "libxfs.h"
#include "libfrog/fsgeom.h"
#include "libfrog/radix-tree.h"
#include "libfrog/paths.h"
#include "command.h"
#include "init.h"
#include "space.h"
#include "input.h"
#include "relocation.h"
#include "handle.h"

static struct cmdinfo relocate_cmd;

RADIX_TREE(relocation_data, 0);

struct inode_path *
ipath_alloc(
	const char		*path,
	const struct stat	*stat)
{
	struct inode_path	*ipath;
	int			pathlen = strlen(path);

	/* Allocate a new inode path and record the path in it. */
	ipath = calloc(1, sizeof(*ipath) + pathlen + 1);
	if (!ipath) {
		fprintf(stderr,
_("Failed to allocate ipath %s for inode 0x%llx failed: %s\n"),
			path, (unsigned long long)stat->st_ino,
			strerror(-errno));
		return NULL;
	}
	INIT_LIST_HEAD(&ipath->path_list);
	memcpy(&ipath->path[0], path, pathlen);
	ipath->ino = stat->st_ino;

	return ipath;
}

static int
relocate_targets_to_ag(
	const char		*mnt,
	xfs_agnumber_t		dst_agno)
{
	struct inode_path	*ipath;
	uint64_t		idx = 0;
	int			ret;

	do {
		struct xfs_fd	xfd = {0};
		struct stat	st;

		/* lookup first relocation target */
		ret = radix_tree_gang_lookup_tag(&relocation_data,
					(void **)&ipath, idx, 1, INODE_PATH);
		if (!ret)
			break;

		ret = stat(ipath->path, &st);
		if (ret) {
			fprintf(stderr, _("stat(%s) failed: %s\n"),
				ipath->path, strerror(errno));
			goto next;
		}

		if (!S_ISREG(st.st_mode)) {
			fprintf(stderr,
		_("FIXME! Skipping %s: not a regular file.\n"),
				ipath->path);
			goto next;
		}

		ret = xfd_open(&xfd, ipath->path, O_RDONLY);
		if (ret) {
			fprintf(stderr, _("xfd_open(%s) failed: %s\n"),
				ipath->path, strerror(-ret));
			goto next;
		}

		/* move to destination AG */
		ret = relocate_file_to_ag(mnt, ipath, &xfd, dst_agno);
		xfd_close(&xfd);

		/*
		 * If the destination AG has run out of space, we do not remove
		 * this inode from relocation data so it will be immediately
		 * retried in the next AG. Other errors will be fatal.
		 */
		if (ret < 0)
			return ret;
next:
		/* remove from relocation data */
		idx = ipath->ino + 1;
		radix_tree_delete(&relocation_data, ipath->ino);
	} while (ret != -ENOSPC);

	return ret;
}

static int
relocate_targets(
	const char		*mnt,
	xfs_agnumber_t		highest_agno)
{
	xfs_agnumber_t		dst_agno = 0;
	int			ret;

	for (dst_agno = 0; dst_agno <= highest_agno; dst_agno++) {
		ret = relocate_targets_to_ag(mnt, dst_agno);
		if (ret == -ENOSPC)
			continue;
		break;
	}
	return ret;
}

/*
 * Relocate all the user objects in an AG to lower numbered AGs.
 */
static int
relocate_f(
	int		argc,
	char		**argv)
{
	xfs_agnumber_t	target_agno = -1;
	xfs_agnumber_t	highest_agno = -1;
	xfs_agnumber_t	log_agno;
	void		*fshandle;
	size_t		fshdlen;
	int		c;
	int		ret;

	while ((c = getopt(argc, argv, "a:h:")) != EOF) {
		switch (c) {
		case 'a':
			target_agno = cvt_u32(optarg, 10);
			if (errno) {
				fprintf(stderr, _("bad target agno value %s\n"),
					optarg);
				return command_usage(&relocate_cmd);
			}
			break;
		case 'h':
			highest_agno = cvt_u32(optarg, 10);
			if (errno) {
				fprintf(stderr, _("bad highest agno value %s\n"),
					optarg);
				return command_usage(&relocate_cmd);
			}
			break;
		default:
			return command_usage(&relocate_cmd);
		}
	}

	if (optind != argc)
		return command_usage(&relocate_cmd);

	if (target_agno == -1) {
		fprintf(stderr, _("Target AG must be specified!\n"));
		return command_usage(&relocate_cmd);
	}

	log_agno = cvt_fsb_to_agno(&file->xfd, file->xfd.fsgeom.logstart);
	if (target_agno <= log_agno) {
		fprintf(stderr,
_("Target AG %d must be higher than the journal AG (AG %d). Aborting.\n"),
			target_agno, log_agno);
		goto out_fail;
	}

	if (target_agno >= file->xfd.fsgeom.agcount) {
		fprintf(stderr,
_("Target AG %d does not exist. Filesystem only has %d AGs\n"),
			target_agno, file->xfd.fsgeom.agcount);
		goto out_fail;
	}

	if (highest_agno == -1)
		highest_agno = target_agno - 1;

	if (highest_agno >= target_agno) {
		fprintf(stderr,
_("Highest destination AG %d must be less than target AG %d. Aborting.\n"),
			highest_agno, target_agno);
		goto out_fail;
	}

	if (relocation_data.rnode) {
		fprintf(stderr,
_("Relocation data populated from previous commands. Aborting.\n"));
		goto out_fail;
	}

	/* this is so we can use fd_to_handle() later on */
	ret = path_to_fshandle(file->fs_path.fs_dir, &fshandle, &fshdlen);
	if (ret < 0) {
		fprintf(stderr, _("Cannot get fshandle for mount %s: %s\n"),
			file->fs_path.fs_dir, strerror(errno));
		goto out_fail;
	}

	ret = find_relocation_targets(target_agno);
	if (ret) {
		fprintf(stderr,
_("Failure during target discovery. Aborting.\n"));
		goto out_fail;
	}

	ret = resolve_target_paths(file->fs_path.fs_dir);
	if (ret) {
		fprintf(stderr,
_("Failed to resolve all paths from mount point %s: %s\n"),
			file->fs_path.fs_dir, strerror(-ret));
		goto out_fail;
	}

	ret = relocate_targets(file->fs_path.fs_dir, highest_agno);
	if (ret) {
		fprintf(stderr,
_("Failed to relocate all targets out of AG %d: %s\n"),
			target_agno, strerror(-ret));
		goto out_fail;
	}

	return 0;
out_fail:
	exitcode = 1;
	return 0;
}

static void
relocate_help(void)
{
	printf(_(
"\n"
"Relocate all the user data and metadata in an AG.\n"
"\n"
"This function will discover all the relocatable objects in a single AG and\n"
"move them to a lower AG as preparation for a shrink operation.\n"
"\n"
"	-a <agno>	Allocation group to empty\n"
"	-h <agno>	Highest target AG allowed to relocate into\n"
"\n"));

}

void
relocate_init(void)
{
	relocate_cmd.name = "relocate";
	relocate_cmd.altname = "relocate";
	relocate_cmd.cfunc = relocate_f;
	relocate_cmd.argmin = 2;
	relocate_cmd.argmax = 4;
	relocate_cmd.args = "-a agno [-h agno]";
	relocate_cmd.flags = CMD_FLAG_ONESHOT;
	relocate_cmd.oneline = _("Relocate data in an AG.");
	relocate_cmd.help = relocate_help;

	add_command(&relocate_cmd);
}

