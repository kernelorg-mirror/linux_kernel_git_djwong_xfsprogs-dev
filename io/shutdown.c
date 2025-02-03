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
verifymedia_help(void)
{
	printf(_(
"\n"
" Verify the media of the devices backing the filesystem.\n"
"\n"
" -d -- Verify the data device (default).\n"
" -l -- Verify the log device.\n"
" -r -- Verify the realtime device.\n"
" -R -- Report media errors to fsnotify.\n"
"\n"
" start is the byte offset of the start of the range to verify.  If the start\n"
" is specified, the end may (optionally) be specified as well."
"\n"
" end is the byte offset of the end of the range to verify.\n"
"\n"
" If neither start nor end are specified, the media verification will\n"
" check the entire device."
"\n"));
}

static int
verifymedia_f(
	int			argc,
	char			**argv)
{
	xfs_daddr_t		orig_start_daddr = 0;
	struct xfs_verify_media me = {
		.start_daddr	= orig_start_daddr,
		.end_daddr	= XFS_VERIFY_TO_EOD,
		.dev		= XFS_VERIFY_DATADEV,
	};
	struct timeval		t1, t2;
	long long		l;
	size_t			fsblocksize, fssectsize;
	const char		*verifydev = _("datadev");
	int			verbose = 0;
	int			c, ret;

	init_cvtnum(&fsblocksize, &fssectsize);

	while ((c = getopt(argc, argv, "dlrRv")) != EOF) {
		switch (c) {
		case 'd':
			me.dev = XFS_VERIFY_DATADEV;
			verifydev = _("datadev");
			break;
		case 'l':
			me.dev = XFS_VERIFY_LOGDEV;
			verifydev = _("logdev");
			break;
		case 'r':
			me.dev = XFS_VERIFY_RTDEV;
			verifydev = _("rtdev");
			break;
		case 'R':
			me.flags |= XFS_VERIFY_REPORT_ERRORS;
			break;
		case 'v':
			verbose++;
			break;
		default:
			verifymedia_help();
			exitcode = 1;
			return 0;
		}
	}

	/* Range start (optional) */
	if (optind < argc) {
		l = cvtnum(fsblocksize, fssectsize, argv[optind]);
		if (l < 0) {
			printf("non-numeric start argument -- %s\n",
					argv[optind]);
			exitcode = 1;
			return 0;
		}

		orig_start_daddr = l / 512;
		me.start_daddr = orig_start_daddr;
		optind++;
	}

	/* Range end (optional if range start was specified) */
	if (optind < argc) {
		l = cvtnum(fsblocksize, fssectsize, argv[optind]);
		if (l < 0) {
			printf("non-numeric end argument -- %s\n",
					argv[optind]);
			exitcode = 1;
			return 0;
		}

		me.end_daddr = ((l + 511) / 512);
		optind++;
	}

	if (optind < argc) {
		printf("too many arguments -- %s\n", argv[optind]);
		exitcode = 1;
		return 0;
	}

	gettimeofday(&t1, NULL);
	ret = ioctl(file->fd, XFS_IOC_VERIFY_MEDIA, &me);
	gettimeofday(&t2, NULL);
	t2 = tsub(t2, t1);
	if (ret < 0) {
		fprintf(stderr,
 "%s: ioctl(XFS_IOC_VERIFY_MEDIA) [\"%s\"]: %s\n",
				progname, file->name, strerror(errno));
		exitcode = 1;
		return 0;
	}

	if (me.ioerror)
		printf("%s: verify error at offset %llu length %llu: %s\n",
				verifydev,
				BBTOB(me.start_daddr),
				BBTOB(me.end_daddr - me.start_daddr),
				strerror(me.ioerror));
	else if (verbose) {
		report_io_times("verified", &t2, BBTOB(orig_start_daddr),
				BBTOB(me.start_daddr - orig_start_daddr),
				BBTOB(me.end_daddr - orig_start_daddr),
				1, false);
	}

	return 0;
}

static struct cmdinfo verifymedia_cmd = {
	.name		= "verifymedia",
	.cfunc		= verifymedia_f,
	.argmin		= 0,
	.argmax		= -1,
	.flags		= CMD_FLAG_ONESHOT | CMD_NOMAP_OK,
	.args		= "[-lr] [start [end]]",
	.help		= verifymedia_help,
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
		add_command(&verifymedia_cmd);
	}
}
