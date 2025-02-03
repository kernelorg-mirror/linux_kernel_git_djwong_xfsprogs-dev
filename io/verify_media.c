// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2026 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "command.h"
#include "input.h"
#include "init.h"
#include "io.h"

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
" -s -- Sleep this many usecs between IOs.\n"
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
		.me_start_daddr	= orig_start_daddr,
		.me_end_daddr	= ~0ULL,
		.me_dev		= XFS_DEV_DATA,
	};
	struct timeval		t1, t2;
	long long		l;
	size_t			fsblocksize, fssectsize;
	const char		*verifydev = _("datadev");
	int			verbose = 0;
	int			c, ret;

	init_cvtnum(&fsblocksize, &fssectsize);

	while ((c = getopt(argc, argv, "b:dlrRs:v")) != EOF) {
		switch (c) {
		case 'd':
			me.me_dev = XFS_DEV_DATA;
			verifydev = _("datadev");
			break;
		case 'l':
			me.me_dev = XFS_DEV_LOG;
			verifydev = _("logdev");
			break;
		case 'r':
			me.me_dev = XFS_DEV_RT;
			verifydev = _("rtdev");
			break;
		case 'b':
			l = cvtnum(fsblocksize, fssectsize, optarg);
			if (l < 0 || l > UINT_MAX) {
				printf("non-numeric maxio argument -- %s\n",
						optarg);
				exitcode = 1;
				return 0;
			}
			me.me_max_io_size = l;
			break;
		case 'R':
			me.me_flags |= XFS_VERIFY_MEDIA_REPORT;
			break;
		case 's':
			l = atoi(optarg);
			if (l < 0) {
				printf("non-numeric rest_us argument -- %s\n",
						optarg);
				exitcode = 1;
				return 0;
			}
			me.me_rest_us = l;
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
		me.me_start_daddr = orig_start_daddr;
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

		me.me_end_daddr = ((l + 511) / 512);
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

	if (me.me_ioerror)
		printf("%s: verify error at offset %llu length %llu: %s\n",
				verifydev,
				BBTOB(me.me_start_daddr),
				BBTOB(me.me_end_daddr - me.me_start_daddr),
				strerror(me.me_ioerror));
	else if (verbose) {
		unsigned long long	total;

		if (me.me_end_daddr > orig_start_daddr)
			total = BBTOB(me.me_end_daddr - orig_start_daddr);
		else
			total = 0;
		report_io_times("verified", &t2, BBTOB(orig_start_daddr),
				BBTOB(me.me_start_daddr - orig_start_daddr),
				total, 1, false);
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
verifymedia_init(void)
{
	add_command(&verifymedia_cmd);
}
