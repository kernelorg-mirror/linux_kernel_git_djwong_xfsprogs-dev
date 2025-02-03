// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2004-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "command.h"
#include "input.h"
#include "init.h"
#include "io.h"

static cmdinfo_t shutdown_cmd;

static int
shutdown_f(
	int		argc,
	char		**argv)
{
	int		c, flag = XFS_FSOP_GOING_FLAGS_NOLOGFLUSH;

	while ((c = getopt(argc, argv, "fv")) != -1) {
		switch (c) {
		case 'f':
			flag = XFS_FSOP_GOING_FLAGS_LOGFLUSH;
			break;
		default:
			exitcode = 1;
			return command_usage(&shutdown_cmd);
		}
	}

	if ((xfsctl(file->name, file->fd, XFS_IOC_GOINGDOWN, &flag)) < 0) {
		perror("XFS_IOC_GOINGDOWN");
		exitcode = 1;
		return 0;
	}
	return 0;
}

static void
shutdown_help(void)
{
	printf(_(
"\n"
" Shuts down the filesystem and prevents any further IO from occurring.\n"
"\n"
" By default, shutdown will not flush completed transactions to disk\n"
" before shutting the filesystem down, simulating a disk failure or crash.\n"
" With -f, the log will be flushed to disk, matching XFS behavior when\n"
" metadata corruption is encountered.\n"
"\n"
" -f -- Flush completed transactions to disk before shut down.\n"
"\n"));
}

static void
mediaerror_help(void)
{
	printf(_(
"\n"
" Report a media error on the data device to the filesystem.\n"
"\n"
" -l -- Report against the log device.\n"
" -r -- Report against the realtime device.\n"
"\n"
" offset is the byte offset of the start of the failed range.  If offset is\n"
" specified, mapping length may (optionally) be specified as well."
"\n"
" length is the byte length of the failed range.\n"
"\n"
" If neither offset nor length are specified, the media error report will\n"
" be made against the entire device."
"\n"));
}

static int
mediaerror_f(
	int			argc,
	char			**argv)
{
	struct xfs_media_error	me = {
		.daddr		= 0,
		.bbcount	= -1ULL,
		.flags		= XFS_MEDIA_ERROR_DATADEV,
	};
	long long		l;
	size_t			fsblocksize, fssectsize;
	int			c, ret;

	init_cvtnum(&fsblocksize, &fssectsize);

	while ((c = getopt(argc, argv, "lr")) != EOF) {
		switch (c) {
		case 'l':
			me.flags = (me.flags & ~XFS_MEDIA_ERROR_DEVMASK) |
						XFS_MEDIA_ERROR_LOGDEV;
			break;
		case 'r':
			me.flags = (me.flags & ~XFS_MEDIA_ERROR_DEVMASK) |
						XFS_MEDIA_ERROR_RTDEV;
			break;
		default:
			mediaerror_help();
			exitcode = 1;
			return 0;
		}
	}

	/* Range start (optional) */
	if (optind < argc) {
		l = cvtnum(fsblocksize, fssectsize, argv[optind]);
		if (l < 0) {
			printf("non-numeric offset argument -- %s\n",
					argv[optind]);
			exitcode = 1;
			return 0;
		}

		me.daddr = l / 512;
		optind++;
	}

	/* Range length (optional if range start was specified) */
	if (optind < argc) {
		l = cvtnum(fsblocksize, fssectsize, argv[optind]);
		if (l < 0) {
			printf("non-numeric len argument -- %s\n",
					argv[optind]);
			exitcode = 1;
			return 0;
		}

		me.bbcount = howmany(l, 512);
		optind++;
	}

	if (optind < argc) {
		printf("too many arguments -- %s\n", argv[optind]);
		exitcode = 1;
		return 0;
	}

	ret = ioctl(file->fd, XFS_IOC_MEDIA_ERROR, &me);
	if (ret) {
		fprintf(stderr,
 "%s: ioctl(XFS_IOC_MEDIA_ERROR) [\"%s\"]: %s\n",
				progname, file->name, strerror(errno));
		exitcode = 1;
		return 0;
	}

	return 0;
}

static struct cmdinfo mediaerror_cmd = {
	.name		= "mediaerror",
	.cfunc		= mediaerror_f,
	.argmin		= 0,
	.argmax		= -1,
	.flags		= CMD_FLAG_ONESHOT | CMD_NOMAP_OK,
	.args		= "[-lr] [offset [length]]",
	.help		= mediaerror_help,
};

void
shutdown_init(void)
{
	shutdown_cmd.name = "shutdown";
	shutdown_cmd.cfunc = shutdown_f;
	shutdown_cmd.argmin = 0;
	shutdown_cmd.argmax = 1;
	shutdown_cmd.flags = CMD_NOMAP_OK | CMD_FLAG_ONESHOT | CMD_FLAG_FOREIGN_OK;
	shutdown_cmd.args = _("[-f]");
	shutdown_cmd.help = shutdown_help;
	shutdown_cmd.oneline =
		_("shuts down the filesystem where the current file resides");

	if (expert) {
		add_command(&shutdown_cmd);
		add_command(&mediaerror_cmd);
	}
}
