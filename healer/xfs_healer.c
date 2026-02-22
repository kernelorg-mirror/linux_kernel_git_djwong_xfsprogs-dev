// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025-2026 Oracle.  All Rights Reserved.
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
#include "libfrog/systemd.h"
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

/*
 * Find the filesystem source name for the mount that we're monitoring.  We
 * don't use the fs_table_ helpers because we might be running in a restricted
 * environment where we cannot access device files at all.
 */
static int
try_capture_fsinfo(
	struct healer_ctx	*ctx)
{
	struct mntent		*mnt;
	FILE			*mtp;
	char			rpath[PATH_MAX], rmnt_dir[PATH_MAX];

	if (!realpath(ctx->mntpoint, rpath))
		return -1;

	mtp = setmntent(_PATH_PROC_MOUNTS, "r");
	if (mtp == NULL)
		return -1;

	while ((mnt = getmntent(mtp)) != NULL) {
		if (strcmp(mnt->mnt_type, "xfs"))
			continue;
		if (!realpath(mnt->mnt_dir, rmnt_dir))
			continue;

		if (!strcmp(rpath, rmnt_dir)) {
			ctx->fsname = strdup(mnt->mnt_fsname);
			break;
		}
	}

	endmntent(mtp);

	return ctx->fsname ? 0 : -1;
}

static unsigned int
healer_nproc(
	const struct healer_ctx	*ctx)
{
	/*
	 * By default, use one event handler thread.  In foreground mode,
	 * create one thread per cpu.
	 */
	return ctx->foreground ? platform_nproc() : 1;
}

enum mon_state {
	MON_START,
	MON_EXIT,
	MON_ERROR,
};

/* Set ourselves up to monitor the given mountpoint for health events. */
static enum mon_state
setup_monitor(
	struct healer_ctx	*ctx)
{
	const long		BUF_SIZE = sysconf(_SC_PAGE_SIZE) * 2;
	int			mon_fd;
	int			ret;

	ret = xfd_open(&ctx->mnt, ctx->mntpoint, O_RDONLY);
	if (ret) {
		perror(ctx->mntpoint);
		return MON_ERROR;
	}

	ret = try_capture_fsinfo(ctx);
	if (ret) {
		fprintf(stderr, "%s: %s\n", ctx->mntpoint,
				_("Not a XFS mount point."));
		goto out_mnt_fd;
	}

	if (ctx->want_repair) {
		/* Check that the kernel supports repairs at all. */
		if (!healer_can_repair(ctx)) {
			fprintf(stderr, "%s: %s\n", ctx->mntpoint,
 _("XFS online repair is not supported, exiting"));
			goto out_mnt_fd;
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
	 * Open weak-referenced file handle to mountpoint so that we can
	 * reconnect to the mountpoint to start repairs or to look up file
	 * paths for logging.
	 */
	if (ctx->want_repair || healer_has_parent(ctx)) {
		ret = weakhandle_alloc(ctx->mnt.fd, ctx->mntpoint,
				ctx->fsname, &ctx->wh);
		if (ret) {
			fprintf(stderr, "%s: %s: %s\n", ctx->mntpoint,
					_("creating weak fshandle"),
					strerror(errno));
			goto out_mnt_fd;
		}
	}

	/*
	 * Open the health monitor, then close the mountpoint to avoid pinning
	 * it.  We can reconnect later if need be.
	 */
	mon_fd = open_health_monitor(ctx, ctx->mnt.fd);
	if (mon_fd < 0) {
		switch (errno) {
		case ENOTTY:
		case EOPNOTSUPP:
			fprintf(stderr, "%s: %s\n", ctx->mntpoint,
 _("XFS health monitoring not supported."));
			break;
		case EEXIST:
			fprintf(stderr, "%s: %s\n", ctx->mntpoint,
 _("XFS health monitoring already running."));
			break;
		default:
			perror(ctx->mntpoint);
			break;
		}

		goto out_mnt_fd;
	}
	close(ctx->mnt.fd);
	ctx->mnt.fd = -1;

	/*
	 * At this point, we know that the kernel is capable of repairing the
	 * filesystem and telling us that it needs repairs.  If the user only
	 * wanted us to check for the capability, we're done.
	 */
	if (ctx->support_check) {
		close(mon_fd);
		return MON_EXIT;
	}

	/*
	 * mon_fp consumes mon_fd.  We intentionally leave mon_fp attached to
	 * the context so that we keep the monitoring fd open until we've torn
	 * down all the background threads.
	 */
	ctx->mon_fp = fdopen(mon_fd, "r");
	if (!ctx->mon_fp) {
		perror(ctx->mntpoint);
		goto out_mon_fd;
	}

	/* Increase the buffer size so that we can reduce kernel calls */
	ctx->mon_buf = malloc(BUF_SIZE);
	if (ctx->mon_buf)
		setvbuf(ctx->mon_fp, ctx->mon_buf, _IOFBF, BUF_SIZE);

	/*
	 * Queue up to 1MB of events before we stop trying to read events from
	 * the kernel as quickly as we can.  Note that the kernel won't accrue
	 * more than 32K of internal events before it starts dropping them.
	 */
	ret = workqueue_create_bound(&ctx->event_queue, ctx, healer_nproc(ctx),
			1048576 / sizeof(struct xfs_health_monitor_event));
	if (ret) {
		errno = ret;
		fprintf(stderr, "%s: %s: %s\n", ctx->mntpoint,
				_("worker threadpool setup"), strerror(errno));
		goto out_mon_fp;
	}
	ctx->queue_active = true;

	return MON_START;

out_mon_fp:
	if (ctx->mon_fp)
		fclose(ctx->mon_fp);
	ctx->mon_fp = NULL;
out_mon_fd:
	if (mon_fd >= 0)
		close(mon_fd);
out_mnt_fd:
	if (ctx->mnt.fd >= 0)
		close(ctx->mnt.fd);
	ctx->mnt.fd = -1;
	return MON_ERROR;
}

/* Monitor the given mountpoint for health events. */
static void
monitor(
	struct healer_ctx	*ctx)
{
	bool			mounted = true;
	size_t			nr;

	do {
		struct xfs_health_monitor_event	*hme;
		int		ret;

		hme = malloc(sizeof(*hme));
		if (!hme) {
			pthread_mutex_lock(&ctx->conlock);
			fprintf(stderr, "%s: %s\n", ctx->mntpoint,
					_("could not allocate event object"));
			pthread_mutex_unlock(&ctx->conlock);
			break;
		}

		nr = fread(hme, sizeof(*hme), 1, ctx->mon_fp);
		if (nr == 0) {
			free(hme);
			break;
		}

		if (hme->type == XFS_HEALTH_MONITOR_TYPE_UNMOUNT)
			mounted = false;

		/* handle_event owns hme if the workqueue_add succeeds */
		ret = workqueue_add(&ctx->event_queue, handle_event, 0, hme);
		if (ret) {
			pthread_mutex_lock(&ctx->conlock);
			fprintf(stderr, "%s: %s: %s\n", ctx->mntpoint,
					_("could not queue event object"),
					strerror(ret));
			pthread_mutex_unlock(&ctx->conlock);
			free(hme);
			break;
		}
	} while (nr > 0 && mounted);
}

/* Tear down all the resources that we created for monitoring */
static void
teardown_monitor(
	struct healer_ctx	*ctx)
{
	if (ctx->queue_active) {
		workqueue_terminate(&ctx->event_queue);
		workqueue_destroy(&ctx->event_queue);
	}
	if (ctx->mon_fp) {
		fclose(ctx->mon_fp);
		ctx->mon_fp = NULL;
	}
	free(ctx->mon_buf);
	weakhandle_free(&ctx->wh);
	ctx->mon_buf = NULL;
}

static void __attribute__((noreturn))
usage(void)
{
	fprintf(stderr, "%s %s %s\n", _("Usage:"), progname,
			_("[OPTIONS] mountpoint"));
	fprintf(stderr, "\n");
	fprintf(stderr, _("Options:\n"));
	fprintf(stderr, _("  --debug       Enable debugging messages.\n"));
	fprintf(stderr, _("  --everything  Capture all events.\n"));
	fprintf(stderr, _("  --foreground  Process events as soon as possible.\n"));
	fprintf(stderr, _("  --quiet       Do not log health events to stdout.\n"));
	fprintf(stderr, _("  --repair      Always repair corrupt metadata.\n"));
	fprintf(stderr, _("  --supported   Check that health monitoring is supported.\n"));
	fprintf(stderr, _("  -V            Print version.\n"));

	exit(EXIT_FAILURE);
}

enum long_opt_nr {
	LOPT_DEBUG,
	LOPT_EVERYTHING,
	LOPT_FOREGROUND,
	LOPT_HELP,
	LOPT_QUIET,
	LOPT_REPAIR,
	LOPT_SUPPORTED,
	LOPT_SVCNAME,

	LOPT_MAX,
};

int
main(
	int			argc,
	char			**argv)
{
	struct healer_ctx	ctx = {
		.conlock	= (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER,
		.log		= 1,
		.mnt.fd		= -1,
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
		[LOPT_DEBUG]	   = {"debug", no_argument, &ctx.debug, 1 },
		[LOPT_EVERYTHING]  = {"everything", no_argument, &ctx.everything, 1 },
		[LOPT_FOREGROUND]  = {"foreground", no_argument, &ctx.foreground, 1 },
		[LOPT_HELP]	   = {"help", no_argument, NULL, 0 },
		[LOPT_QUIET]	   = {"quiet", no_argument, &ctx.log, 0 },
		[LOPT_REPAIR]	   = {"repair", no_argument, &ctx.want_repair, 1 },
		[LOPT_SUPPORTED]   = {"supported", no_argument, &ctx.support_check, 1 },
		[LOPT_SVCNAME]	   = {"svcname", no_argument, &ctx.print_svcname, 1 },

		[LOPT_MAX]	   = {NULL, 0, NULL, 0 },
	};

	while ((c = getopt_long(argc, argv, "V", long_options, &option_index))
			!= EOF) {
		switch (c) {
		case 0:
			switch (option_index) {
			case LOPT_HELP:
				usage();
				break;
			default:
				break;
			}
			break;
		case 'V':
			vflag++;
			break;
		default:
			usage();
			break;
		}
	}

	if (vflag) {
		fprintf(stdout, "%s %s %s\n", progname, _("version"), VERSION);
		fflush(stdout);
		return EXIT_SUCCESS;
	}

	if (optind != argc - 1)
		usage();

	ctx.mntpoint = argv[optind];

	if (ctx.print_svcname) {
		char	unitname[PATH_MAX];

		ret = systemd_path_instance_unit_name(XFS_HEALER_SVCNAME,
				ctx.mntpoint, unitname, sizeof(unitname));
		if (ret) {
			perror(ctx.mntpoint);
			return EXIT_FAILURE;
		}

		printf("%s\n", unitname);
		return EXIT_SUCCESS;
	}

	switch (setup_monitor(&ctx)) {
	case MON_ERROR:
		ret = -1;
		break;
	case MON_EXIT:
		ret = 0;
		break;
	case MON_START:
		ret = 0;
		monitor(&ctx);
		break;
	}

	teardown_monitor(&ctx);
	free((char *)ctx.fsname);
	if (ctx.support_check)
		return systemd_service_exit_now(ret);
	return systemd_service_exit(ret);
}
