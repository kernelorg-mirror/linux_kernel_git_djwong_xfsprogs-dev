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

struct healer_ctx {
	/* CLI options */
	int			debug;
	int			log;
	int			everything;
	int			want_repair;
	int			check;

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
};

static inline bool healer_has_rmapbt(const struct healer_ctx *ctx)
{
	return ctx->mnt.fsgeom.flags & XFS_FSOP_GEOM_FLAGS_RMAPBT;
}

static inline bool healer_has_parent(const struct healer_ctx *ctx)
{
	return ctx->mnt.fsgeom.flags & XFS_FSOP_GEOM_FLAGS_PARENT;
}

/* eventlog.c */
void report_event(struct healer_ctx *ctx,
		const struct xfs_health_monitor_event *hme);
const char *fs_structname(uint32_t what);
const char *ag_structname(uint32_t what);
const char *rtg_structname(uint32_t what);
const char *inode_structname(uint32_t what);
void report_inode_location(struct healer_ctx *ctx,
		const struct xfs_health_monitor_event *hme, char *path,
		size_t pathlen);

/* repair.c */
int repair_metadata(struct healer_ctx *ctx,
		const struct xfs_health_monitor_event *hme);
bool healer_can_repair(struct healer_ctx *ctx);

/* weakhandle.c */
int weakhandle_alloc(int fd, const char *mountpoint, const struct fs_path *fsp,
		struct weakhandle **whp);
int weakhandle_reopen(struct weakhandle *wh, int *fd);
void weakhandle_free(struct weakhandle **whp);
int weakhandle_getpath_for(struct weakhandle *wh, uint64_t ino, uint32_t gen,
		char *path, size_t pathlen);

#endif /* XFS_HEALER_XFS_HEALER_H_ */
