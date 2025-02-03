// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef XFS_HEALER_XFS_HEALER_H_
#define XFS_HEALER_XFS_HEALER_H_

extern char *progname;

#define _PATH_PROC_MOUNTS	"/proc/mounts"

struct fs_path;

struct healer_ctx {
	/* CLI options, must be int */
	int			debug;
	int			log;
	int			everything;

	/* fd and fs geometry for mount */
	struct xfs_fd		mnt;

	/*
	 * shared references to the display name for logging and the fs table
	 * entry
	 */
	const char		*mntpoint;
	struct fs_path		*fs_path;

	/* file stream of monitor and buffer */
	FILE			*mon_fp;
	char			*mon_buf;

	/* coordinates logging printfs */
	pthread_mutex_t		conlock;

	/* event queue */
	struct workqueue	event_queue;
	bool			queue_active;
};

#endif /* XFS_HEALER_XFS_HEALER_H_ */
