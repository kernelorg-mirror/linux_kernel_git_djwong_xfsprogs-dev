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
#include "libfrog/statmount.h"

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
	/* two paths and a filesystem type string of unknown length */
	size_t			smbuf_size =
		libfrog_statmount_sizeof(PATH_MAX * 2 + NAME_MAX);
	struct statmount	*smbuf = malloc(smbuf_size);
	int			ret;

	if (!smbuf) {
		perror("statmount alloc");
		return;
	}

	ret = libfrog_statmount(mnt_id, mnt_ns_fd, REQUIRED_STATMOUNT_FIELDS,
			smbuf, smbuf_size);
	if (ret) {
		perror("statmount");
		goto out_smbuf;
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
		goto out_smbuf;

	if (!strcmp(smbuf->str + smbuf->fs_type, "xfs") &&
	    !strcmp(smbuf->str + smbuf->mnt_root, "/"))
		start_healer(smbuf->str + smbuf->mnt_point);
out_smbuf:
	free(smbuf);
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

/* Make sure listmount actually works so we can start up existing mounts */
static int
check_listmount(
	int			mnt_ns_fd)
{
	uint64_t		mnt_ids[1];
	uint64_t		cursor = LISTMOUNT_INIT_CURSOR;
	int			ret;

	ret = libfrog_listmount(LSMT_ROOT, mnt_ns_fd, &cursor, mnt_ids, 1);
	if (ret >= 0)
		return 0;

	if (errno == ENOSYS)
		fprintf(stderr, "%s\n",
 _("This program requires the listmount system call."));
	else
		perror("listmount");

	return -1;
}

#define NR_MNT_IDS		(32)

/* Start healer services for existing XFS mounts. */
static int
start_existing_mounts(
	int			mnt_ns_fd)
{
	uint64_t		mnt_ids[NR_MNT_IDS];
	uint64_t		cursor = LISTMOUNT_INIT_CURSOR;
	int			i;
	int			ret;

	while ((ret = libfrog_listmount(LSMT_ROOT, mnt_ns_fd, &cursor,
					mnt_ids, NR_MNT_IDS)) > 0) {
		for (i = 0; i < ret; i++)
			examine_mount(mnt_ns_fd, mnt_ids[i]);
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
		case FAN_Q_OVERFLOW:
			start_existing_mounts(mnt_ns_fd);
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

static void __attribute__((noreturn))
usage(void)
{
	fprintf(stderr, "%s %s %s\n", _("Usage:"), progname, _("[OPTIONS]"));
	fprintf(stderr, "\n");
	fprintf(stderr, _("Options:\n"));
	fprintf(stderr, _("  --debug      Enable debugging messages.\n"));
	fprintf(stderr, _("  --mountns    Path to the mount namespace file.\n"));
	fprintf(stderr, _("  --supported  Make sure we can actually run.\n"));
	fprintf(stderr, _("  -V           Print version.\n"));

	exit(EXIT_FAILURE);
}

enum long_opt_nr {
	LOPT_DEBUG,
	LOPT_HELP,
	LOPT_MOUNTNS,
	LOPT_SUPPORTED,

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
	int		support_check = 0;
	int		ret = 0;

	struct option long_options[] = {
		[LOPT_SUPPORTED] = {"supported", no_argument, &support_check, 1 },
		[LOPT_DEBUG]	 = {"debug", no_argument, &debug, 1 },
		[LOPT_HELP]	 = {"help", no_argument, NULL, 0 },
		[LOPT_MOUNTNS]	 = {"mountns", required_argument, NULL, 0 },
		[LOPT_MAX]	 = {NULL, 0, NULL, 0 },
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
	if (mnt_ns_fd == DEFAULT_MOUNTNS_FD && mntns != NULL) {
		/*
		 * We specified a path to a mount namespace file but got fd 0,
		 * which (for listmount and statmount) means to use the current
		 * process' mount namespace.  That's probably not what the user
		 * wanted.
		 */
		fprintf(stderr,
 _("%s: got bad file descriptor for mount namespace\n"),
				mntns);
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

	if (support_check) {
		ret = check_listmount(mnt_ns_fd);
		if (ret)
			goto out;

		/*
		 * We're being run as an ExecCondition process and we've
		 * decided to start the main service.  There is no need to wait
		 * for journald because the ExecStart version of ourselves will
		 * take care of the waiting for us.
		 */
		return systemd_service_exit_now(0);
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
	return systemd_service_exit(ret);
}
