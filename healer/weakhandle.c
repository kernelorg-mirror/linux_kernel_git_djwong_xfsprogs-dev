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
#include "libfrog/getparents.h"
#include "libfrog/paths.h"
#include "libfrog/systemd.h"
#include "libfrog/statmount.h"
#include "xfs_healer.h"

struct weakhandle {
	/* Synchronizes accesses to mntpoint */
	pthread_rwlock_t	lock;

	/* Owned reference to the user's mountpoint for logging */
	char			*mntpoint;

	/* Shared reference to the getmntent fsname for reconnecting */
	const char		*fsname;

	/* Mount id for faster reconnecting */
	uint64_t		mnt_id;

	/* handle to root dir */
	void			*hanp;
	size_t			hlen;
};

/* Capture a handle for a given filesystem, but don't attach to the fd. */
int
weakhandle_alloc(
	int			fd,
	const char		*mountpoint,
	uint64_t		mnt_id,
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

	wh->mntpoint = strdup(mountpoint);
	if (!wh->mntpoint)
		goto out_wh;
	wh->mnt_id = mnt_id;
	wh->fsname = fsname;

	ret = pthread_rwlock_init(&wh->lock, NULL);
	if (ret) {
		errno = ret;
		goto out_mntpt;
	}

	ret = fd_to_handle(fd, &wh->hanp, &wh->hlen);
	if (ret)
		goto out_rwlock;

	*whp = wh;
	return 0;

out_rwlock:
	pthread_rwlock_destroy(&wh->lock);
out_mntpt:
	free(wh->mntpoint);
out_wh:
	free(wh);
	return -1;
}

static void
try_update_mntpoint(
	struct weakhandle	*wh,
	const char		*path)
{
	char			*s = strdup(path);

	if (!s)
		return;

	pthread_rwlock_wrlock(&wh->lock);
	if (path != wh->mntpoint) {
		free(wh->mntpoint);
		wh->mntpoint = s;
		s = NULL;
	}
	pthread_rwlock_unlock(&wh->lock);

	free(s);
}

/*
 * Reopen a file handle obtained via weak reference, using the given path to a
 * mount point.
 */
static int
weakhandle_reopen_from(
	struct weakhandle	*wh,
	const char		*path,
	int			*fd,
	weakhandle_fd_t		is_acceptable,
	void			*data)
{
	void			*hanp;
	size_t			hlen;
	int			mnt_fd;
	int			ret;

	*fd = -1;

	mnt_fd = open(path, O_RDONLY);
	if (mnt_fd < 0)
		return -1;

	ret = fd_to_handle(mnt_fd, &hanp, &hlen);
	if (ret)
		goto out_mntfd;

	if (hlen != wh->hlen || memcmp(hanp, wh->hanp, hlen)) {
		errno = ESTALE;
		goto out_handle;
	}

	if (is_acceptable && !is_acceptable(mnt_fd, data)) {
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

/* three paths, a filesystem type string of unknown length, and options */
#define MNTENT_BUFLEN		(4 * PATH_MAX + NAME_MAX)

/* Reopen a file handle obtained via weak reference. */
int
weakhandle_reopen(
	struct weakhandle	*wh,
	int			*fd,
	weakhandle_fd_t		is_acceptable,
	void			*data)
{
	struct mntent		ent;
	const size_t		smbuf_size =
		libfrog_statmount_sizeof(PATH_MAX);
	struct statmount	*smbuf = alloca(smbuf_size);
	FILE			*mtab;
	struct mntent		*mnt;
	char			*buf;
	int			ret;

	/* First try reopening using the original mountpoint */
	pthread_rwlock_rdlock(&wh->lock);
	ret = weakhandle_reopen_from(wh, wh->mntpoint, fd, is_acceptable, data);
	pthread_rwlock_unlock(&wh->lock);
	if (!ret)
		return 0;

	/*
	 * The original mountpoint didn't work, which means the mount might
	 * have been moved.  Look up the mountpoint for the mount id that we
	 * captured earlier, which is a quick lookup if there are many mounts.
	 * Note that @ret is nonzero here.
	 */
	ret = libfrog_statmount(wh->mnt_id, DEFAULT_MOUNTNS_FD,
			STATMOUNT_MNT_POINT, smbuf, smbuf_size);
	if (ret || !(smbuf->mask & STATMOUNT_MNT_POINT))
		goto fallback;
	ret = weakhandle_reopen_from(wh, smbuf->str + smbuf->mnt_point, fd,
			is_acceptable, data);
	if (!ret) {
		try_update_mntpoint(wh, smbuf->str + smbuf->mnt_point);
		return 0;
	}

fallback:
	/*
	 * That didn't work, so now walk /proc/mounts to find a mount with the
	 * same fsname (aka xfs data device path) as when we started.
	 */
	mtab = setmntent(_PATH_PROC_MOUNTS, "r");
	if (!mtab)
		return -1;

	buf = malloc(MNTENT_BUFLEN);
	if (!buf) {
		ret = -1;
		goto out_mtab;
	}

	while ((mnt = getmntent_r(mtab, &ent, buf, MNTENT_BUFLEN)) != NULL) {
		if (strcmp(mnt->mnt_type, "xfs"))
			continue;
		if (strcmp(mnt->mnt_fsname, wh->fsname))
			continue;

		ret = weakhandle_reopen_from(wh, mnt->mnt_dir, fd,
				is_acceptable, data);
		if (!ret) {
			try_update_mntpoint(wh, mnt->mnt_dir);
			break;
		}
	}

	if (*fd < 0) {
		errno = ESTALE;
		ret = -1;
	}

	free(buf);
out_mtab:
	endmntent(mtab);
	return ret;
}

/* Tear down a weak handle */
void
weakhandle_free(
	struct weakhandle	**whp)
{
	struct weakhandle	*wh = *whp;

	if (wh) {
		pthread_rwlock_destroy(&wh->lock);
		free_handle(wh->hanp, wh->hlen);
		free(wh->mntpoint);
		free(wh);
	}

	*whp = NULL;
}

struct bufvec {
	struct weakhandle	*wh;
	char			*buf;
	size_t			len;
};

static int
render_path(
	const char		*donotuse,
	const struct path_list	*path,
	void			*arg)
{
	struct bufvec		*args = arg;
	struct weakhandle	*wh = args->wh;
	int			mntpt_len;
	ssize_t			ret;

	/* Trim trailing slashes from the mountpoint */
	pthread_rwlock_rdlock(&wh->lock);
	mntpt_len = strlen(wh->mntpoint);
	while (mntpt_len > 0 && wh->mntpoint[mntpt_len - 1] == '/')
		mntpt_len--;

	ret = snprintf(args->buf, args->len, "%.*s", mntpt_len, wh->mntpoint);
	pthread_rwlock_unlock(&wh->lock);
	if (ret < 0 || ret >= args->len)
		return 0;

	ret = path_list_to_string(path, args->buf + ret, args->len - ret);
	if (ret < 0)
		return 0;

	/* magic code that means we found one */
	return ECANCELED;
}

/* Render any path to this weakhandle into the specified buffer. */
int
weakhandle_getpath_for(
	struct weakhandle	*wh,
	uint64_t		ino,
	uint32_t		gen,
	char			*path,
	size_t			pathlen)
{
	struct xfs_handle	fakehandle;
	struct bufvec		bv = {
		.wh		= wh,
		.buf		= path,
		.len		= pathlen,
	};
	int			mnt_fd;
	int			ret;

	if (wh->hlen != sizeof(fakehandle)) {
		errno = EINVAL;
		return -1;
	}
	memcpy(&fakehandle, wh->hanp, sizeof(fakehandle));
	fakehandle.ha_fid.fid_ino = ino;
	fakehandle.ha_fid.fid_gen = gen;

	ret = weakhandle_reopen(wh, &mnt_fd, NULL, NULL);
	if (ret)
		return ret;

	/*
	 * In the common case, files only have one parent; and what's the
	 * chance that we'll need to walk past the second parent to find *one*
	 * path that goes to the rootdir?  With a max filename length of 255
	 * bytes, we pick 600 for the buffer size.
	 */
	ret = handle_walk_paths_fd(NULL, mnt_fd, &fakehandle,
			sizeof(fakehandle), 600, render_path, &bv);
	switch (ret) {
	case ECANCELED:
		/* found a path */
		ret = 0;
		break;
	default:
		/* didn't find one */
		errno = ENOENT;
		ret = -1;
		break;
	}

	close(mnt_fd);
	return ret;
}

/* Compute the systemd instance unit name for this mountpoint. */
int
weakhandle_instance_unit_name(
	struct weakhandle	*wh,
	const char		*template,
	char			*unitname,
	size_t			unitnamelen)
{
	int			ret;

	pthread_rwlock_rdlock(&wh->lock);
	ret = systemd_path_instance_unit_name(template, wh->mntpoint,
			unitname, unitnamelen);
	pthread_rwlock_unlock(&wh->lock);
	return ret;
}
