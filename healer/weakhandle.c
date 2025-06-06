// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include <pthread.h>
#include <stdlib.h>
#include <sys/wait.h>

#include "platform_defs.h"
#include "handle.h"
#include "libfrog/fsgeom.h"
#include "libfrog/workqueue.h"
#include "libfrog/getparents.h"
#include "libfrog/paths.h"
#include "xfs_healer.h"

struct weakhandle {
	/*
	 * Shared references to the display name for logging and the fs table
	 * entry for the filesystem.
	 */
	const char		*mntpoint;
	const struct fs_path	*fs_path;

	/* handle to root dir */
	void			*hanp;
	size_t			hlen;
};

/* Capture a handle for a given filesystem, but don't attach to the fd. */
int
weakhandle_alloc(
	int			fd,
	const char		*mountpoint,
	const struct fs_path	*fs_path,
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
	wh->fs_path = fs_path;

	ret = fd_to_handle(fd, &wh->hanp, &wh->hlen);
	if (ret)
		goto out_wh;

	*whp = wh;
	return 0;

out_wh:
	free(wh);
	return -1;
}

/*
 * Reopen a file handle obtained via weak reference, using the given path to a
 * mount point.
 */
static int
weakhandle_reopen_from(
	struct weakhandle	*wh,
	const char		*path,
	int			*fd)
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

	free_handle(hanp, hlen);
	*fd = mnt_fd;
	return 0;

out_handle:
	free_handle(hanp, hlen);
out_mntfd:
	close(mnt_fd);
	return -1;
}

/* Reopen a file handle obtained via weak reference. */
int
weakhandle_reopen(
	struct weakhandle	*wh,
	int			*fd)
{
	FILE			*mtab;
	struct mntent		*mount;
	int			ret;

	ret = weakhandle_reopen_from(wh, wh->mntpoint, fd);
	if (!ret)
		return 0;

	mtab = setmntent(_PATH_PROC_MOUNTS, "r");
	if (!mtab)
		return -1;

	while ((mount = getmntent(mtab)) != NULL) {
		if (strcmp(mount->mnt_type, "xfs"))
			continue;
		if (strcmp(mount->mnt_fsname, wh->fs_path->fs_name))
			continue;

		ret = weakhandle_reopen_from(wh, mount->mnt_dir, fd);
		if (!ret)
			break;
	}

	if (*fd < 0) {
		errno = ESTALE;
		ret = -1;
	}

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
		free_handle(wh->hanp, wh->hlen);
		free(wh);
	}

	*whp = NULL;
}

struct bufvec {
	char	*buf;
	size_t	len;
};

static int
render_path(
	const char		*mntpt,
	const struct path_list	*path,
	void			*arg)
{
	struct bufvec		*args = arg;
	int			mntpt_len = strlen(mntpt);
	ssize_t			ret;

	/* Trim trailing slashes from the mountpoint */
	while (mntpt_len > 0 && mntpt[mntpt_len - 1] == '/')
		mntpt_len--;

	ret = snprintf(args->buf, args->len, "%.*s", mntpt_len, mntpt);
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

	ret = weakhandle_reopen(wh, &mnt_fd);
	if (ret)
		return ret;

	/*
	 * In the common case, files only have one parent; and what's the
	 * chance that we'll need to walk past the second parent to find *one*
	 * path that goes to the rootdir?  With a max filename length of 255
	 * bytes, we pick 600 for the buffer size.
	 */
	ret = handle_walk_paths_fd(wh->mntpoint, mnt_fd, &fakehandle,
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
	FILE			*fp;
	char			*s;
	ssize_t			bytes;
	pid_t			child_pid;
	int			pipe_fds[2];
	int			ret;

	ret = pipe(pipe_fds);
	if (ret)
		return -1;

	child_pid = fork();
	if (child_pid < 0)
		return -1;

	if (!child_pid) {
		/* child process */
		char		*argv[] = {
			"systemd-escape",
			"--template",
			(char *)template,
			"--path",
			(char *)wh->mntpoint,
			NULL,
		};

		ret = dup2(pipe_fds[1], STDOUT_FILENO);
		if (ret < 0) {
			perror(wh->mntpoint);
			goto fail;
		}

		ret = execvp("systemd-escape", argv);
		if (ret)
			perror(wh->mntpoint);

fail:
		exit(EXIT_FAILURE);
	}

	/* parent scrapes the output */
	fp = fdopen(pipe_fds[0], "r");
	s = fgets(unitname, unitnamelen, fp);
	fclose(fp);
	close(pipe_fds[1]);

	waitpid(child_pid, NULL, 0);

	if (!s) {
		errno = ENOENT;
		return -1;
	}

	/* trim off trailing newline */
	bytes = strlen(s);
	if (s[bytes - 1] == '\n')
		s[bytes - 1] = 0;

	return 0;
}
