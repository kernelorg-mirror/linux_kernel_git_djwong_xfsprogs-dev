// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2020 Oracle.
 * All Rights Reserved.
 */
#include "platform_defs.h"
#include "libxfs.h"
#include "command.h"
#include "input.h"
#include "init.h"
#include "io.h"
#include "libfrog/logging.h"
#include "libfrog/paths.h"
#include "libfrog/fsgeom.h"

static cmdinfo_t aginfo_cmd;

static int
report_aginfo(
	struct xfs_fd		*xfd,
	xfs_agnumber_t		agno,
	int			oflag)
{
	struct xfs_ag_geometry	ageo = { 0 };
	bool			update = false;
	int			ret;

	ret = -xfrog_ag_geometry(xfd->fd, agno, &ageo);
	if (ret) {
		xfrog_perror(ret, "aginfo");
		return 1;
	}

	switch (oflag) {
	case 0:
		ageo.ag_flags |= XFS_AG_FLAG_UPDATE;
		ageo.ag_flags &= ~XFS_AG_FLAG_NOALLOC;
		update = true;
		break;
	case 1:
		ageo.ag_flags |= (XFS_AG_FLAG_UPDATE | XFS_AG_FLAG_NOALLOC);
		update = true;
		break;
	}

	if (update) {
		ret = -xfrog_ag_geometry(xfd->fd, agno, &ageo);
		if (ret) {
			xfrog_perror(ret, "aginfo update");
			return 1;
		}
	}

	printf(_("AG: %u\n"),		ageo.ag_number);
	printf(_("Blocks: %u\n"),	ageo.ag_length);
	printf(_("Free Blocks: %u\n"),	ageo.ag_freeblks);
	printf(_("Inodes: %u\n"),	ageo.ag_icount);
	printf(_("Free Inodes: %u\n"),	ageo.ag_ifree);
	printf(_("Sick: 0x%x\n"),	ageo.ag_sick);
	printf(_("Checked: 0x%x\n"),	ageo.ag_checked);
	printf(_("Flags: 0x%x\n"),	ageo.ag_flags);

	return 0;
}

#define OPT_STRING ("a:o:")

/* Display or modify AG status. */
static int
aginfo_f(
	int			argc,
	char			**argv)
{
	struct xfs_fd		xfd = XFS_FD_INIT(file->fd);
	unsigned long long	x;
	xfs_agnumber_t		agno = NULLAGNUMBER;
	int			oflag = -1;
	int			c;
	int			ret = 0;

	ret = -xfd_prepare_geometry(&xfd);
	if (ret) {
		xfrog_perror(ret, "xfd_prepare_geometry");
		exitcode = 1;
		return 1;
	}

	while ((c = getopt(argc, argv, OPT_STRING)) != EOF) {
		switch (c) {
		case 'a':
			errno = 0;
			x = strtoll(optarg, NULL, 10);
			if (!errno && x >= NULLAGNUMBER)
				errno = ERANGE;
			if (errno) {
				perror("aginfo");
				return 1;
			}
			agno = x;
			break;
		case 'o':
			errno = 0;
			x = strtoll(optarg, NULL, 10);
			if (!errno && x != 0 && x != 1)
				errno = ERANGE;
			if (errno) {
				perror("aginfo");
				return 1;
			}
			oflag = x;
			break;
		default:
			return command_usage(&aginfo_cmd);
		}
	}

	if (agno != NULLAGNUMBER) {
		ret = report_aginfo(&xfd, agno, oflag);
	} else {
		for (agno = 0; !ret && agno < xfd.fsgeom.agcount; agno++) {
			ret = report_aginfo(&xfd, agno, oflag);
		}
	}

	return ret;
}

static void
aginfo_help(void)
{
	printf(_(
"\n"
"Report allocation group geometry.\n"
"\n"
" -a agno  -- Report on the given allocation group.\n"
" -o num   -- Set or clear the NOALLOC flag.\n"
"\n"));

}

static cmdinfo_t aginfo_cmd = {
	.name = "aginfo",
	.cfunc = aginfo_f,
	.argmin = 0,
	.argmax = -1,
	.args = "[-a agno] [-o arg]",
	.flags = CMD_NOMAP_OK,
	.help = aginfo_help,
};

void
aginfo_init(void)
{
	aginfo_cmd.oneline = _("Get/set XFS allocation group state."),
	add_command(&aginfo_cmd);
}
