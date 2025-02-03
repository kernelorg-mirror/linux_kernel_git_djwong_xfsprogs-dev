// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include <pthread.h>
#include <stdlib.h>

#include "platform_defs.h"
#include "libfrog/fsgeom.h"
#include "libfrog/paths.h"
#include "libfrog/workqueue.h"
#include "xfs_healer.h"

/* Program name; needed for libfrog error reports. */
char				*progname = "xfs_healer";

/* Return a health monitoring fd. */
static int
open_health_monitor(
	struct healer_ctx		*ctx,
	int				mnt_fd)
{
	struct xfs_health_monitor	hmo = {
		.format			= XFS_HEALTH_MONITOR_FMT_CSTRUCT,
	};

	if (ctx->everything)
		hmo.flags |= XFS_HEALTH_MONITOR_VERBOSE;

	return ioctl(mnt_fd, XFS_IOC_HEALTH_MONITOR, &hmo);
}

/* Handle an event asynchronously. */
static void
handle_event(
	struct workqueue		*wq,
	uint32_t			index,
	void				*arg)
{
	struct xfs_health_monitor_event	*hme = arg;
	struct healer_ctx		*ctx = wq->wq_ctx;

	report_event(ctx, hme);

	if (ctx->want_repair && hme->type == XFS_HEALTH_MONITOR_TYPE_SICK)
		repair_metadata(ctx, hme);

	free(hme);
}

/* Monitor the given mountpoint for health events. */
static int
monitor(
	struct healer_ctx	*ctx)
{
	const long		BUF_SIZE = sysconf(_SC_PAGE_SIZE) * 2;
	size_t			nr;
	int			mon_fd;
	int			ret;

	ret = xfd_open(&ctx->mnt, ctx->mntpoint, O_RDONLY);
	if (ret) {
		perror(ctx->mntpoint);
		return 1;
	}

	/*
	 * Open weak-referenced file handle to mountpoint before we go any
	 * further.
	 */
	if (ctx->want_repair) {
		ret = weakhandle_alloc(ctx->mnt.fd, ctx->mntpoint,
				ctx->fs_path, &ctx->wh);
		if (ret) {
			fprintf(stderr, "%s: %s: %s\n", ctx->mntpoint,
					_("creating weak fshandle"),
					strerror(errno));
			return -1;
		}
	}

	/*
	 * Open the health monitor, then close the mountpoint to avoid pinning
	 * it.  We can reconnect later if need be.
	 */
	mon_fd = open_health_monitor(ctx, ctx->mnt.fd);
	close(ctx->mnt.fd);
	ctx->mnt.fd = -1;
	if (mon_fd < 0) {
		switch (errno) {
		case ENOTTY:
		case EOPNOTSUPP:
			fprintf(stderr, "%s: %s\n", ctx->mntpoint,
 _("XFS health monitoring not supported."));
			return -1;
		default:
			perror(ctx->mntpoint);
			return -1;
		}
	}

	/*
	 * mon_fp consumes mon_fd.  We intentionally leave mon_fp attached to
	 * the context so that we keep the monitoring fd open until we've torn
	 * down all the background threads.
	 */
	ctx->mon_fp = fdopen(mon_fd, "r");
	if (!ctx->mon_fp) {
		close(mon_fd);
		perror(ctx->mntpoint);
		return -1;
	}

	/* Increase the buffer size so that we can reduce kernel calls */
	ctx->mon_buf = malloc(BUF_SIZE);
	if (ctx->mon_buf)
		setvbuf(ctx->mon_fp, ctx->mon_buf, _IOFBF, BUF_SIZE);

	do {
		struct xfs_health_monitor_event	*hme;

		hme = malloc(sizeof(*hme));
		if (!hme) {
			perror("events");
			return -1;
		}

		nr = fread(hme, sizeof(*hme), 1, ctx->mon_fp);
		if (nr == 0) {
			free(hme);
			break;
		}

		ret = workqueue_add(&ctx->event_queue, handle_event, 0, hme);
		if (ret) {
			free(hme);
			errno = ret;
			perror("event queue");
			return -1;
		}
	} while (nr > 0);

	return 0;
}

static void __attribute__((noreturn))
usage(void)
{
	fprintf(stderr, _("Usage: %s [OPTIONS] mountpoint\n"), progname);
	fprintf(stderr, "\n");
	fprintf(stderr, _("Options:\n"));
	fprintf(stderr, _("  --debug      Enable debugging messages.\n"));
	fprintf(stderr, _("  --everything Capture all events.\n"));
	fprintf(stderr, _("  --log        Log health events to stdout.\n"));
	fprintf(stderr, _("  --repair     Always repair corrupt metadata.\n"));
	fprintf(stderr, _("  -V           Print version.\n"));

	exit(EXIT_FAILURE);
}

int
main(
	int			argc,
	char			**argv)
{
	struct healer_ctx	ctx = {
		.conlock	= (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER,
	};
	int			option_index;
	int			vflag = 0;
	int			c;
	int			ret;

	progname = basename(argv[0]);
	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);

	struct option long_options[] = {
		{"debug",	no_argument,	&ctx.debug, 1 },
		{"log",		no_argument,	&ctx.log, 1 },
		{"everything",	no_argument,	&ctx.everything, 1 },
		{"repair",	no_argument,	&ctx.want_repair, 1 },
		{NULL,		0,		NULL, 0 },
	};

	while ((c = getopt_long(argc, argv, "V", long_options, &option_index))
			!= EOF) {
		switch (c) {
		case 0:
			break;
		case 'V':
			vflag++;
			break;
		default:
			usage();
		}
	}

	if (vflag) {
		fprintf(stdout, _("%s version %s\n"), progname, VERSION);
		fflush(stdout);
		return EXIT_SUCCESS;
	}

	if (optind != argc - 1)
		usage();

	ctx.mntpoint = argv[optind];

	fs_table_initialise(0, NULL, 0, NULL);
	ctx.fs_path = fs_table_lookup_mount(ctx.mntpoint);
	if (!ctx.fs_path) {
		fprintf(stderr, _("%s: Not a XFS mount point.\n"),
				ctx.mntpoint);
		return EXIT_FAILURE;
	}

	/*
	 * The kernel won't allow more than 32K of events to accrue before it
	 * starts handing out lost events.  Userspace can't know the size of
	 * the internal event objects, but we'll allow twice that much memory
	 * usage in userspace.
	 */
	ret = workqueue_create_bound(&ctx.event_queue, &ctx, platform_nproc(),
			65536 / sizeof(struct xfs_health_monitor_event));
	if (ret) {
		errno = ret;
		perror("workqueue");
		goto out;
	}

	ret = monitor(&ctx);
	if (ret)
		goto out_events;

	weakhandle_free(&ctx.wh);
out_events:
	workqueue_terminate(&ctx.event_queue);
	workqueue_destroy(&ctx.event_queue);
	if (ctx.mon_fp)
		fclose(ctx.mon_fp);
	free(ctx.mon_buf);
	fs_table_destroy();
out:
	return ret != 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
