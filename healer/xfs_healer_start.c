// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"

#include <errno.h>
#include <err.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/fanotify.h>
#include <sys/types.h>
#include <unistd.h>
#include <linux/mount.h>
#include <sys/syscall.h>
#include <string.h>
#include <limits.h>

#include "platform_defs.h"
#include "libfrog/systemd.h"

#define DEFAULT_MOUNTNS_FILE		"/proc/self/ns/mnt"

static int debug = 0;
static const char *progname = "xfs_healer_start";

/* Start the xfs_healer service for a given mountpoint. */
static void
start_healer(
	const char	*mntpoint)
{
	char		unitname[PATH_MAX];
	int		ret;

	ret = systemd_path_instance_unit_name(XFS_HEALER_SVCNAME, mntpoint,
			unitname, PATH_MAX);
	if (ret) {
		fprintf(stderr, "%s: %s\n", mntpoint,
				_("Could not determine xfs_healer unit name."));
		return;
	}

	/*
	 * Restart so that we aren't foiled by an existing unit that's slowly
	 * working its way off a cycled mount.
	 */
	ret = systemd_manage_unit(UM_RESTART, unitname);
	if (ret) {
		fprintf(stderr, "%s: %s: %s\n", mntpoint,
				_("Could not start xfs_healer service unit"),
				unitname);
		return;
	}

	printf("%s: %s\n", mntpoint, _("xfs_healer service started."));
	fflush(stdout);
}

#define REQUIRED_STATMOUNT_FIELDS (STATMOUNT_FS_TYPE | \
				   STATMOUNT_MNT_POINT | \
				   STATMOUNT_MNT_ROOT)

/* Process a newly discovered mountpoint. */
static void
examine_mount(
	int			mnt_ns_fd,
	uint64_t		mnt_id)
{
	struct mnt_id_req	req = {
		.size		= sizeof(req),
		.mnt_id		= mnt_id,
#ifdef HAVE_LISTMOUNT_NS_FD
		.mnt_ns_fd	= mnt_ns_fd,
#else
		.spare		= mnt_ns_fd,
#endif
		.param		= REQUIRED_STATMOUNT_FIELDS,
	};
	size_t			smbuf_size = sizeof(struct statmount) + 4096;
	struct statmount	*smbuf = alloca(smbuf_size);
	int			ret;

	ret = syscall(SYS_statmount, &req, smbuf, smbuf_size, 0);
	if (ret) {
		perror("statmount");
		return;
	}

	if (debug) {
		printf("mount: id 0x%llx fstype %s mountpoint %s mntroot %s\n",
				(unsigned long long)mnt_id,
				(smbuf->mask & STATMOUNT_FS_TYPE) ?
					smbuf->str + smbuf->fs_type : "null",
				(smbuf->mask & STATMOUNT_MNT_POINT) ?
					smbuf->str + smbuf->mnt_point : "null",
				(smbuf->mask & STATMOUNT_MNT_ROOT) ?
					smbuf->str + smbuf->mnt_root : "null");
		fflush(stdout);
	}

	/* Look for mount points for the root dir of an XFS filesystem. */
	if ((smbuf->mask & REQUIRED_STATMOUNT_FIELDS) !=
			   REQUIRED_STATMOUNT_FIELDS)
		return;

	if (!strcmp(smbuf->str + smbuf->fs_type, "xfs") &&
	    !strcmp(smbuf->str + smbuf->mnt_root, "/"))
		start_healer(smbuf->str + smbuf->mnt_point);
}

/* Translate fanotify mount events into something we can process. */
static void
handle_mount_event(
	const struct fanotify_event_metadata	*event,
	int					mnt_ns_fd)
{
	const struct fanotify_event_info_header	*info;
	const struct fanotify_event_info_mnt	*mnt;
	int					off;

	if (event->fd != FAN_NOFD) {
		if (debug)
			fprintf(stderr, "Expected FAN_NOFD, got fd=%d\n",
					event->fd);
		return;
	}

	switch (event->mask) {
	case FAN_MNT_ATTACH:
		if (debug) {
			printf("FAN_MNT_ATTACH (len=%d)\n", event->event_len);
			fflush(stdout);
		}
		break;
	default:
		/* should never get here */
		return;
	}

	for (off = sizeof(*event) ; off < event->event_len;
	     off += info->len) {
		info = (struct fanotify_event_info_header *)
			((char *) event + off);

		switch (info->info_type) {
		case FAN_EVENT_INFO_TYPE_MNT:
			mnt = (struct fanotify_event_info_mnt *) info;

			if (debug) {
				printf( "Mount record: len=%d mnt_id=0x%llx\n",
						mnt->hdr.len, mnt->mnt_id);
				fflush(stdout);
			}

			examine_mount(mnt_ns_fd, mnt->mnt_id);
			break;

		default:
			if (debug)
				fprintf(stderr,
 "Unexpected fanotify event info_type=%d len=%d\n",
						info->info_type, info->len);
			break;
		}
	}
}

/* Extract mount attachment notifications from fanotify. */
static void
handle_notifications(
	char				*buffer,
	ssize_t				len,
	int				mnt_ns_fd)
{
	struct fanotify_event_metadata	*event =
		(struct fanotify_event_metadata *) buffer;

	for (; FAN_EVENT_OK(event, len); event = FAN_EVENT_NEXT(event, len)) {

		switch (event->mask) {
		case FAN_MNT_ATTACH:
			handle_mount_event(event, mnt_ns_fd);
			break;
		default:
			if (debug)
				fprintf(stderr,
 "Unexpected fanotify mark: 0x%llx\n",
					(unsigned long long)event->mask);
			break;
		}
	}
}

/* Start healer services for existing XFS mounts. */
static int
start_existing_mounts(
	int			mnt_ns_fd)
{
	struct mnt_id_req	req = {
		.size		= sizeof(struct mnt_id_req),
#ifdef HAVE_LISTMOUNT_NS_FD
		.mnt_ns_fd	= mnt_ns_fd,
#else
		.spare		= mnt_ns_fd,
#endif
		.mnt_id		= LSMT_ROOT,
	};
	uint64_t		mnt_ids[32];
	int			i;
	int			ret;

	while ((ret = syscall(SYS_listmount, &req, &mnt_ids, 32, 0)) > 0) {
		for (i = 0; i < ret; i++)
			examine_mount(mnt_ns_fd, mnt_ids[i]);

		req.param = mnt_ids[ret - 1];
	}

	if (ret < 0) {
		if (errno == ENOSYS)
			fprintf(stderr, "%s\n",
 _("This program requires the listmount system call."));
		else
			perror("listmount");
		return -1;
	}

	return 0;
}

static void __attribute__((noreturn))
usage(void)
{
	fprintf(stderr, "%s %s %s\n", _("Usage:"), progname, _("[OPTIONS]"));
	fprintf(stderr, "\n");
	fprintf(stderr, _("Options:\n"));
	fprintf(stderr, _("  --check      Make sure we can actually run.\n"));
	fprintf(stderr, _("  --debug      Enable debugging messages.\n"));
	fprintf(stderr, _("  --mountns    Path to the mount namespace file.\n"));
	fprintf(stderr, _("  -V           Print version.\n"));

	exit(EXIT_FAILURE);
}

enum long_opt_nr {
	LOPT_CHECK,
	LOPT_DEBUG,
	LOPT_HELP,
	LOPT_MOUNTNS,

	LOPT_MAX,
};

int
main(
	int		argc,
	char		*argv[])
{
	char		buffer[BUFSIZ];
	const char	*mntns = NULL;
	int		mnt_ns_fd;
	int		fan_fd;
	int		c;
	int		option_index;
	int		check = 0;
	int		ret = 0;

	struct option long_options[] = {
		[LOPT_CHECK]	= {"check", no_argument, &check, 1 },
		[LOPT_DEBUG]	= {"debug", no_argument, &debug, 1 },
		[LOPT_HELP]	= {"help", no_argument, NULL, 0 },
		[LOPT_MOUNTNS]	= {"mountns", required_argument, NULL, 0 },
		[LOPT_MAX]	= {NULL, 0, NULL, 0 },
	};

	while ((c = getopt_long(argc, argv, "V", long_options, &option_index))
			!= EOF) {
		switch (c) {
		case 0:
			switch (option_index) {
			case LOPT_MOUNTNS:
				mntns = optarg;
				break;
			case LOPT_HELP:
				usage();
				break;
			default:
				break;
			}
			break;
		case 'V':
			fprintf(stdout, "%s %s %s\n", progname, _("version"),
					VERSION);
			fflush(stdout);
			return EXIT_SUCCESS;
		default:
			usage();
			break;
		}
	}

	/*
	 * Try to open the mount namespace file for the current process.
	 * fanotify requires this mount namespace file to send mount attachment
	 * events, so this is required for correct functionality.
	 */
	mnt_ns_fd = open(mntns ? mntns : DEFAULT_MOUNTNS_FILE, O_RDONLY);
	if (mnt_ns_fd < 0) {
		if (errno == ENOENT && !mntns) {
			perror(DEFAULT_MOUNTNS_FILE);
			fprintf(stderr, "%s\n",
 _("This program requires mount namespace support."));
		} else {
			perror(mntns ? mntns : DEFAULT_MOUNTNS_FILE);
		}
		ret = 1;
		goto out;
	}

	fan_fd = fanotify_init(FAN_REPORT_MNT, O_RDONLY);
	if (fan_fd < 0) {
		perror("fanotify_init");
		if (errno == EINVAL)
			fprintf(stderr, "%s\n",
 _("This program requires fanotify mount event support."));
		ret = 1;
		goto out;
	}

	ret = fanotify_mark(fan_fd, FAN_MARK_ADD | FAN_MARK_MNTNS,
			FAN_MNT_ATTACH, mnt_ns_fd, NULL);
	if (ret) {
		perror("fanotify_mark");
		goto out;
	}

	if (check) {
		ret = 0;
		goto out;
	}

	if (debug) {
		printf("fanotify active\n");
		fflush(stdout);
	}

	ret = start_existing_mounts(mnt_ns_fd);
	if (ret)
		goto out;

	while (1) {
		ssize_t bytes_read = read(fan_fd, buffer, BUFSIZ);

		if (bytes_read < 0) {
			perror("fanotify");
			ret = 1;
			break;
		}

		handle_notifications(buffer, bytes_read, mnt_ns_fd);
	}

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
	if (!check && getenv("SERVICE_MODE") != NULL)
		sleep(2);

	return ret != 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
