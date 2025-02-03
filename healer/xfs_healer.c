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
#include "libfrog/healthevent.h"
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
		.format			= XFS_HEALTH_MONITOR_FMT_V0,
	};

	if (ctx->everything)
		hmo.flags |= XFS_HEALTH_MONITOR_VERBOSE;

	return ioctl(mnt_fd, XFS_IOC_HEALTH_MONITOR, &hmo);
}

/* Report either the file handle or its path, if we can. */
void
lookup_path(
	struct healer_ctx			*ctx,
	const struct xfs_health_monitor_event	*hme,
	struct hme_prefix			*pfx)
{
	uint64_t				ino = 0;
	uint32_t				gen = 0;
	int					ret;

	if (!healer_has_parent(ctx))
		return;

	switch (hme->domain) {
	case XFS_HEALTH_MONITOR_DOMAIN_INODE:
		ino = hme->e.inode.ino;
		gen = hme->e.inode.gen;
		break;
	case XFS_HEALTH_MONITOR_DOMAIN_FILERANGE:
		ino = hme->e.filerange.ino;
		gen = hme->e.filerange.gen;
		break;
	default:
		return;
	}

	ret = weakhandle_getpath_for(ctx->wh, ino, gen, pfx->path,
			sizeof(pfx->path));
	if (ret)
		hme_prefix_clear_path(pfx);
}

/* Decide if this event can only be reported upon, and not acted upon. */
static bool
event_not_actionable(
	const struct xfs_health_monitor_event	*hme)
{
	switch (hme->type) {
	case XFS_HEALTH_MONITOR_TYPE_LOST:
	case XFS_HEALTH_MONITOR_TYPE_RUNNING:
	case XFS_HEALTH_MONITOR_TYPE_UNMOUNT:
	case XFS_HEALTH_MONITOR_TYPE_SHUTDOWN:
		return true;
	}

	return false;
}

/* Should this event be logged? */
static bool
event_loggable(
	const struct healer_ctx			*ctx,
	const struct xfs_health_monitor_event	*hme)
{
	return ctx->log || event_not_actionable(hme);
}

/* Are we going to try a repair? */
static inline bool
event_repairable(
	const struct healer_ctx			*ctx,
	const struct xfs_health_monitor_event	*hme)
{
	if (event_not_actionable(hme))
		return false;

	return ctx->want_repair && hme->type == XFS_HEALTH_MONITOR_TYPE_SICK;
}

/* Handle an event asynchronously. */
static void
handle_event(
	struct workqueue		*wq,
	uint32_t			index,
	void				*arg)
{
	struct hme_prefix		pfx;
	struct xfs_health_monitor_event	*hme = arg;
	struct healer_ctx		*ctx = wq->wq_ctx;
	const bool loggable = event_loggable(ctx, hme);
	const bool will_repair = event_repairable(ctx, hme);

	hme_prefix_init(&pfx, ctx->mntpoint);

	/*
	 * Try to look up the file name for the file we're about to log or
	 * about to repair (which always logs).
	 */
	if (loggable || will_repair)
		lookup_path(ctx, hme, &pfx);

	/*
	 * Non-actionable events should always be logged, because they are 100%
	 * informational.
	 */
	if (loggable) {
		pthread_mutex_lock(&ctx->conlock);
		hme_report_event(&pfx, hme);
		pthread_mutex_unlock(&ctx->conlock);
	}

	/* Initiate a repair if appropriate. */
	if (will_repair)
		repair_metadata(ctx, &pfx, hme);

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

	if (ctx->want_repair) {
		/* Check that the kernel supports repairs at all. */
		if (!healer_can_repair(ctx)) {
			fprintf(stderr, "%s: %s\n", ctx->mntpoint,
 _("XFS online repair is not supported, exiting"));
			close(ctx->mnt.fd);
			return -1;
		}

		/* Check for backref metadata that makes repair effective. */
		if (!healer_has_rmapbt(ctx))
			fprintf(stderr, "%s: %s\n", ctx->mntpoint,
 _("XFS online repair is less effective without rmap btrees."));

		if (!healer_has_parent(ctx))
			fprintf(stderr, "%s: %s\n", ctx->mntpoint,
 _("XFS online repair is less effective without parent pointers."));

	}

	/*
	 * Open weak-referenced file handle to mountpoint before we go any
	 * further.  Needed for repairs or file path lookups.
	 */
	if (ctx->want_repair || healer_has_parent(ctx)) {
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
	 * Now that we know that we can repair if the user wanted to, make sure
	 * that the kernel supports reporting events if that was as far as the
	 * user wanted us to go.
	 */
	if (ctx->check) {
		close(mon_fd);
		return 0;
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
	fprintf(stderr, _("  --check      Check that health monitoring is supported.\n"));
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
		{"check",	no_argument,	&ctx.check, 1 },
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

	/*
	 * If we're being run as a service, the return code must fit the LSB
	 * init script action error guidelines, which is to say that we
	 * compress all errors to 1 ("generic or unspecified error", LSB 5.0
	 * section 22.2) and hope the admin will scan the log for what
	 * actually happened.
	 *
	 * We have to sleep 2 seconds here because journald uses the pid to
	 * connect our log messages to the systemd service.  This is critical
	 * for capturing all the log messages if the scrub fails, because the
	 * fail service uses the service name to gather log messages for the
	 * error report.
	 */
	if (getenv("SERVICE_MODE") != NULL)
		sleep(2);

	return ret != 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
