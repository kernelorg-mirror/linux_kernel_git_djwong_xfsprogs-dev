// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025-2026 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include <pthread.h>
#include <stdlib.h>

#include "platform_defs.h"
#include "handle.h"
#include "libfrog/fsgeom.h"
#include "libfrog/workqueue.h"
#include "xfs_healer.h"

struct weakhandle {
	/* Shared reference to the user's mountpoint for logging */
	const char		*mntpoint;

	/* Shared reference to the getmntent fsname for reconnecting */
	const char		*fsname;

	/* handle to root dir */
	void			*hanp;
	size_t			hlen;
};

/* Capture a handle for a given filesystem, but don't attach to the fd. */
int
weakhandle_alloc(
	int			fd,
	const char		*mountpoint,
	const char		*fsname,
	struct weakhandle	**whp)
{
	struct weakhandle	*wh;
	int			ret;

	*whp = NULL;

	if (fd < 0 || !mountpoint) {
		errno = EINVAL;
		return -1;
	}

	wh = calloc(1, sizeof(struct weakhandle));
	if (!wh)
		return -1;

	wh->mntpoint = mountpoint;
	wh->fsname = fsname;

	ret = fd_to_handle(fd, &wh->hanp, &wh->hlen);
	if (ret)
		goto out_wh;

	*whp = wh;
	return 0;

out_wh:
	free(wh);
	return -1;
}

/* Reopen a file handle obtained via weak reference. */
int
weakhandle_reopen(
	struct weakhandle	*wh,
	int			*fd)
{
	void			*hanp;
	size_t			hlen;
	int			mnt_fd;
	int			ret;

	*fd = -1;

	mnt_fd = open(wh->mntpoint, O_RDONLY);
	if (mnt_fd < 0)
		return -1;

	ret = fd_to_handle(mnt_fd, &hanp, &hlen);
	if (ret)
		goto out_mntfd;

	if (hlen != wh->hlen || memcmp(hanp, wh->hanp, hlen)) {
		errno = ESTALE;
		goto out_handle;
	}

	free_handle(hanp, hlen);
	*fd = mnt_fd;
	return 0;

out_handle:
	free_handle(hanp, hlen);
out_mntfd:
	close(mnt_fd);
	return -1;
}

/* Tear down a weak handle */
void
weakhandle_free(
	struct weakhandle	**whp)
{
	struct weakhandle	*wh = *whp;

	if (wh) {
		free_handle(wh->hanp, wh->hlen);
		free(wh);
	}

	*whp = NULL;
}
