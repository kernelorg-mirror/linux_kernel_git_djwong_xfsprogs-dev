// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2017-2023 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "platform_defs.h"
#include "xfs.h"
#include "xfs_arch.h"
#include "list.h"
#include "libfrog/paths.h"
#include "handle.h"
#include "libfrog/pptrs.h"

/* Allocate a buffer large enough for some parent pointer records. */
static inline struct xfs_getparents *
alloc_pptr_buf(
	size_t			bufsize)
{
	struct xfs_getparents	*pi;

	pi = calloc(bufsize, 1);
	if (!pi)
		return NULL;
	pi->gp_ptrs_size = bufsize;
	return pi;
}

/*
 * Walk all parents of the given file handle.  Returns 0 on success or positive
 * errno.
 */
static int
handle_walk_parents(
	int			fd,
	struct xfs_handle	*handle,
	walk_pptr_fn		fn,
	void			*arg)
{
	struct xfs_getparents	*pi;
	struct xfs_parent_ptr	*p;
	unsigned int		i;
	ssize_t			ret = -1;

	pi = alloc_pptr_buf(XFS_XATTR_LIST_MAX);
	if (!pi)
		return errno;

	if (handle) {
		memcpy(&pi->gp_handle, handle, sizeof(struct xfs_handle));
		pi->gp_flags = XFS_GETPARENTS_IFLAG_HANDLE;
	}

	ret = ioctl(fd, XFS_IOC_GETPARENTS, pi);
	while (!ret) {
		if (pi->gp_flags & XFS_GETPARENTS_OFLAG_ROOT) {
			ret = fn(pi, NULL, arg);
			goto out_pi;
		}

		for (i = 0; i < pi->gp_count; i++) {
			p = xfs_getparents_rec(pi, i);
			ret = fn(pi, p, arg);
			if (ret)
				goto out_pi;
		}

		if (pi->gp_flags & XFS_GETPARENTS_OFLAG_DONE)
			break;

		ret = ioctl(fd, XFS_IOC_GETPARENTS, pi);
	}
	if (ret)
		ret = errno;

out_pi:
	free(pi);
	return ret;
}

/* Walk all parent pointers of this handle.  Returns 0 or positive errno. */
int
handle_walk_pptrs(
	void			*hanp,
	size_t			hlen,
	walk_pptr_fn		fn,
	void			*arg)
{
	char			*mntpt;
	int			fd;

	if (hlen != sizeof(struct xfs_handle))
		return EINVAL;

	fd = handle_to_fsfd(hanp, &mntpt);
	if (fd < 0)
		return errno;

	return handle_walk_parents(fd, hanp, fn, arg);
}

/* Walk all parent pointers of this fd.  Returns 0 or positive errno. */
int
fd_walk_pptrs(
	int			fd,
	walk_pptr_fn		fn,
	void			*arg)
{
	return handle_walk_parents(fd, NULL, fn, arg);
}

struct walk_ppaths_info {
	walk_ppath_fn			fn;
	void				*arg;
	char				*mntpt;
	struct path_list		*path;
	int				fd;
};

struct walk_ppath_level_info {
	struct xfs_handle		newhandle;
	struct path_component		*pc;
	struct walk_ppaths_info		*wpi;
};

static int handle_walk_parent_paths(struct walk_ppaths_info *wpi,
		struct xfs_handle *handle);

static int
handle_walk_parent_path_ptr(
	struct xfs_getparents		*pi,
	struct xfs_parent_ptr		*p,
	void				*arg)
{
	struct walk_ppath_level_info	*wpli = arg;
	struct walk_ppaths_info		*wpi = wpli->wpi;
	int				ret = 0;

	if (pi->gp_flags & XFS_GETPARENTS_OFLAG_ROOT)
		return wpi->fn(wpi->mntpt, wpi->path, wpi->arg);

	ret = path_component_change(wpli->pc, p->xpp_name,
				strlen((char *)p->xpp_name), p->xpp_ino);
	if (ret)
		return ret;

	wpli->newhandle.ha_fid.fid_ino = p->xpp_ino;
	wpli->newhandle.ha_fid.fid_gen = p->xpp_gen;

	path_list_add_parent_component(wpi->path, wpli->pc);
	ret = handle_walk_parent_paths(wpi, &wpli->newhandle);
	path_list_del_component(wpi->path, wpli->pc);

	return ret;
}

/*
 * Recursively walk all parents of the given file handle; if we hit the
 * fs root then we call the associated function with the constructed path.
 * Returns 0 for success or positive errno.
 */
static int
handle_walk_parent_paths(
	struct walk_ppaths_info		*wpi,
	struct xfs_handle		*handle)
{
	struct walk_ppath_level_info	*wpli;
	int				ret;

	wpli = malloc(sizeof(struct walk_ppath_level_info));
	if (!wpli)
		return errno;
	wpli->pc = path_component_init("", 0);
	if (!wpli->pc) {
		ret = errno;
		free(wpli);
		return ret;
	}
	wpli->wpi = wpi;
	memcpy(&wpli->newhandle, handle, sizeof(struct xfs_handle));

	ret = handle_walk_parents(wpi->fd, handle, handle_walk_parent_path_ptr,
			wpli);

	path_component_free(wpli->pc);
	free(wpli);
	return ret;
}

/*
 * Call the given function on all known paths from the vfs root to the inode
 * described in the handle.  Returns 0 for success or positive errno.
 */
int
handle_walk_ppaths(
	void			*hanp,
	size_t			hlen,
	walk_ppath_fn		fn,
	void			*arg)
{
	struct walk_ppaths_info	wpi;
	ssize_t			ret;

	if (hlen != sizeof(struct xfs_handle))
		return EINVAL;

	wpi.fd = handle_to_fsfd(hanp, &wpi.mntpt);
	if (wpi.fd < 0)
		return errno;
	wpi.path = path_list_init();
	if (!wpi.path)
		return errno;
	wpi.fn = fn;
	wpi.arg = arg;

	ret = handle_walk_parent_paths(&wpi, hanp);
	path_list_free(wpi.path);

	return ret;
}

/*
 * Call the given function on all known paths from the vfs root to the inode
 * referred to by the file description.  Returns 0 or positive errno.
 */
int
fd_walk_ppaths(
	int			fd,
	walk_ppath_fn		fn,
	void			*arg)
{
	struct walk_ppaths_info	wpi;
	void			*hanp;
	size_t			hlen;
	int			fsfd;
	int			ret;

	ret = fd_to_handle(fd, &hanp, &hlen);
	if (ret)
		return errno;

	fsfd = handle_to_fsfd(hanp, &wpi.mntpt);
	if (fsfd < 0)
		return errno;
	wpi.fd = fd;
	wpi.path = path_list_init();
	if (!wpi.path)
		return errno;
	wpi.fn = fn;
	wpi.arg = arg;

	ret = handle_walk_parent_paths(&wpi, hanp);
	path_list_free(wpi.path);

	return ret;
}

struct path_walk_info {
	char			*buf;
	size_t			len;
};

/* Helper that stringifies the first full path that we find. */
static int
handle_to_path_walk(
	const char		*mntpt,
	struct path_list	*path,
	void			*arg)
{
	struct path_walk_info	*pwi = arg;
	int			mntpt_len = strlen(mntpt);
	int			ret;

	/* Trim trailing slashes from the mountpoint */
	while (mntpt_len > 0 && mntpt[mntpt_len - 1] == '/')
		mntpt_len--;

	ret = snprintf(pwi->buf, pwi->len, "%.*s", mntpt_len, mntpt);
	if (ret != mntpt_len)
		return ENAMETOOLONG;

	ret = path_list_to_string(path, pwi->buf + ret, pwi->len - ret);
	if (ret < 0)
		return ENAMETOOLONG;

	return ECANCELED;
}

/*
 * Return any eligible path to this file handle.  Returns 0 for success or
 * positive errno.
 */
int
handle_to_path(
	void			*hanp,
	size_t			hlen,
	char			*path,
	size_t			pathlen)
{
	struct path_walk_info	pwi;
	int			ret;

	pwi.buf = path;
	pwi.len = pathlen;
	ret = handle_walk_ppaths(hanp, hlen, handle_to_path_walk, &pwi);
	if (ret == ECANCELED)
		return 0;
	return ret;
}

/*
 * Return any eligible path to this file description.  Returns 0 for success
 * or positive errno.
 */
int
fd_to_path(
	int			fd,
	char			*path,
	size_t			pathlen)
{
	struct path_walk_info	pwi;
	int			ret;

	pwi.buf = path;
	pwi.len = pathlen;
	ret = fd_walk_ppaths(fd, handle_to_path_walk, &pwi);
	if (ret == ECANCELED)
		return 0;
	return ret;
}
