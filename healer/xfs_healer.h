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
struct weakhandle;
struct hme_prefix;

struct healer_ctx {
	/* CLI options, must be int */
	int			debug;
	int			log;
	int			everything;
	int			want_repair;

	/* fd and fs geometry for mount */
	struct xfs_fd		mnt;

	/*
	 * shared references to the display name for logging and the fs table
	 * entry
	 */
	const char		*mntpoint;
	struct fs_path		*fs_path;

	/* weak file handle so we can reattach to filesystem */
	struct weakhandle	*wh;

	/* file stream of monitor and buffer */
	FILE			*mon_fp;
	char			*mon_buf;

	/* coordinates logging printfs */
	pthread_mutex_t		conlock;

	/* event queue */
	struct workqueue	event_queue;
	bool			queue_active;
};

static inline bool healer_has_rmapbt(const struct healer_ctx *ctx)
{
	return ctx->mnt.fsgeom.flags & XFS_FSOP_GEOM_FLAGS_RMAPBT;
}

static inline bool healer_has_parent(const struct healer_ctx *ctx)
{
	return ctx->mnt.fsgeom.flags & XFS_FSOP_GEOM_FLAGS_PARENT;
}

/* repair.c */
int repair_metadata(struct healer_ctx *ctx, const struct hme_prefix *pfx,
		const struct xfs_health_monitor_event *hme);
bool healer_can_repair(struct healer_ctx *ctx);

/* weakhandle.c */
int weakhandle_alloc(int fd, const char *mountpoint, const struct fs_path *fsp,
		struct weakhandle **whp);
int weakhandle_reopen(struct weakhandle *wh, int *fd);
void weakhandle_free(struct weakhandle **whp);

#endif /* XFS_HEALER_XFS_HEALER_H_ */
