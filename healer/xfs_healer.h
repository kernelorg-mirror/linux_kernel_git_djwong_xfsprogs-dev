// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025-2026 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef XFS_HEALER_XFS_HEALER_H_
#define XFS_HEALER_XFS_HEALER_H_

extern char *progname;

struct weakhandle;
struct hme_prefix;

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
	int			want_repair;
	int			print_svcname;
	int			support_check;
	int			autofsck;

	/* fd and fs geometry for mount */
	struct xfs_fd		mnt;

	/* Shared reference to the user's mountpoint for logging */
	const char		*mntpoint;

	/* Shared reference to the getmntent fsname for reconnecting */
	const char		*fsname;

	/* Mount id for faster reconnecting */
	uint64_t		mnt_id;

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

void lookup_path(struct healer_ctx *ctx,
		const struct xfs_health_monitor_event *hme,
		struct hme_prefix *pfx);

/* repair.c */
int repair_metadata(struct healer_ctx *ctx, const struct hme_prefix *pfx,
		const struct xfs_health_monitor_event *hme);
bool healer_can_repair(struct healer_ctx *ctx);
void run_full_repair(struct healer_ctx *ctx);

/* weakhandle.c */
int weakhandle_alloc(int fd, const char *mountpoint, uint64_t mnt_id,
		const char *fsname, struct weakhandle **whp);
typedef bool (*weakhandle_fd_t)(int mnt_fd, void *data);
int weakhandle_reopen(struct weakhandle *wh, int *fd,
		weakhandle_fd_t is_acceptable, void *data);
void weakhandle_free(struct weakhandle **whp);
int weakhandle_getpath_for(struct weakhandle *wh, uint64_t ino, uint32_t gen,
		char *path, size_t pathlen);
int weakhandle_instance_unit_name(struct weakhandle *wh, const char *template,
		char *unitname, size_t unitnamelen);

#endif /* XFS_HEALER_XFS_HEALER_H_ */
