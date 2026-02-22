// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025-2026 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef XFS_HEALER_XFS_HEALER_H_
#define XFS_HEALER_XFS_HEALER_H_

extern char *progname;

/*
 * When running in environments with restrictive security policies, healer
 * might not be allowed to access the global mount tree.  However, processes
 * are usually still allowed to see their own mount tree, so use this path for
 * all mount table queries.
 */
#define _PATH_PROC_MOUNTS	"/proc/self/mounts"

struct healer_ctx {
	/* CLI options, must be int */
	int			debug;
	int			log;
	int			everything;
	int			foreground;

	/* fd and fs geometry for mount */
	struct xfs_fd		mnt;

	/* Shared reference to the user's mountpoint for logging */
	const char		*mntpoint;

	/* Shared reference to the getmntent fsname for reconnecting */
	const char		*fsname;

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
