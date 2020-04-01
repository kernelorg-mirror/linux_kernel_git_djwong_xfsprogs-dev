// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2020 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "platform_defs.h"
#include "command.h"
#include "init.h"
#include "io.h"
#include "libfrog/logging.h"
#include "libfrog/fsgeom.h"
#include "libfrog/swapext.h"

struct update_info {
	/* File object for the file that we're updating. */
	struct xfs_fd		file_fd;

	/* Extent swap request to commit the changes. */
	struct file_swap_range	swap_req;

	/* filetable index of the file that we're updating. */
	unsigned int		file_nr;
};

static struct update_info *updates;
static unsigned int nr_updates;

static void
startupdate_help(void)
{
	printf(_(
"\n"
" Prepare for an atomic file update, if supported by the filesystem.\n"
" A temporary file will be opened for writing and inserted into the file\n"
" table.  The current file will be changed to this temporary file.  Neither\n"
" file can be closed for the duration of the update.\n"
"\n"));
}

int
startupdate_f(
	int			argc,
	char			*argv[])
{
	struct fsxattr		attr;
	struct xfs_fsop_geom	fsgeom;
	struct fs_path		fspath;
	struct stat		stat;
	struct update_info	*p;
	char			*path = NULL, *c;
	int			flags = IO_TMPFILE | IO_ATOMICUPDATE;
	int			temp_fd = -1;
	int			ret;

	/* Allocate a new slot. */
	p = realloc(updates, (++nr_updates) * sizeof(*p));
	if (!p) {
		perror("startupdate realloc");
		goto fail;
	}
	updates = p;

	/* Fill out the update information so that we can commit later. */
	p = &updates[nr_updates - 1];
	memset(p, 0, sizeof(*p));
	p->file_fd.fd = file->fd;
	ret = xfd_prepare_geometry(&p->file_fd);
	if (ret) {
		xfrog_perror(ret, file->name);
		goto fail;
	}

	ret = fstat(file->fd, &stat);
	if (ret) {
		perror(file->name);
		goto fail;
	}

	/* Is the current file realtime?  If so, the temp file must match. */
	ret = ioctl(file->fd, FS_IOC_FSGETXATTR, &attr);
	if (ret == 0 && attr.fsx_xflags & FS_XFLAG_REALTIME)
		flags |= IO_REALTIME;

	/* Compute path to the directory that the current file is in. */
	path = strdup(file->name);
	c = strrchr(path, '/');
	if (!c) {
		fprintf(stderr, _("%s: cannot compute dirname?"), path);
		goto fail;
	}
	*c = 0;

	/* Open a temporary file to stage the extents. */
	temp_fd = openfile(path, &fsgeom, flags, 0600, &fspath);
	if (temp_fd < 0) {
		perror(path);
		goto fail;
	}

	/* Clone all the data from the original file into the temporary file. */
	ret = ioctl(temp_fd, XFS_IOC_CLONE, p->file_fd.fd);
	if (ret) {
		perror(path);
		goto fail;
	}

	/*
	 * Snapshot the original file metadata in anticipation of the later
	 * extent swap request.
	 */
	ret = xfrog_swapext_prep(&p->file_fd,
			FILE_SWAP_RANGE_FILE2_FRESH | FILE_SWAP_RANGE_FULL_FILES,
			0, temp_fd, 0, stat.st_size, &p->swap_req);
	if (ret) {
		perror("update prep");
		goto fail;
	}

	/*
	 * Install the temporary file into the same slot of the file table as
	 * the original file.  Ensure that the original file cannot be closed.
	 */
	file->flags |= IO_ATOMICUPDATE;
	file->fd = temp_fd;
	p->file_nr = file - filetable;

	free(path);
	return 0;
fail:
	if (temp_fd >= 0)
		close(temp_fd);
	free(path);
	nr_updates--;
	exitcode = 1;
	return 1;
}

static int
finish_update(
	bool			commit)
{
	struct update_info	*p;
	size_t			length;
	unsigned int		idx = file - filetable;
	unsigned int		i;
	unsigned int		offset;
	int			temp_fd;
	int			ret;

	/* Find our update descriptor. */
	for (i = 0, p = updates; i < nr_updates; i++, p++) {
		if (p->file_nr == idx)
			break;
	}

	if (i == nr_updates) {
		fprintf(stderr,
	_("Current file is not the staging file for an atomic update.\n"));
		exitcode = 1;
		return 0;
	}

	/*
	 * Commit our changes, if desired.  If the extent swap fails, we stop
	 * processing immediately so that we can run more xfs_io commands.
	 */
	if (commit) {
		ret = xfrog_swapext(&p->file_fd, &p->swap_req);
		if (ret) {
			xfrog_perror(ret, _("committing update"));
			exitcode = 1;
			return 0;
		}
	}

	/*
	 * Reset the filetable to point to the original file, and close the
	 * temporary file.
	 */
	file->flags &= ~IO_ATOMICUPDATE;
	temp_fd = file->fd;
	file->fd = p->file_fd.fd;
	ret = close(temp_fd);
	if (ret)
		perror(_("closing temporary file"));

	/* Remove the atomic update context, shifting things down. */
	offset = p - updates;
	length = nr_updates * sizeof(struct update_info);
	length -= (offset + 1) * sizeof(struct update_info);
	if (length)
		memmove(p, p + 1, length);
	nr_updates--;
	return 0;
}

static void
cancelupdate_help(void)
{
	printf(_(
"\n"
" Cancels an atomic file update.  The temporary file will be closed, and the\n"
" current file set back to the original file.\n"
"\n"));
}

int
cancelupdate_f(
	int		argc,
	char		*argv[])
{
	return finish_update(false);
}

static void
commitupdate_help(void)
{
	printf(_(
"\n"
" Commits an atomic file update.  File contents written to the temporary file\n"
" will be swapped atomically with the corresponding range in the original\n"
" file.  The temporary file will be closed, and the current file set back to\n"
" the original file.\n"
"\n"));
}

int
commitupdate_f(
	int		argc,
	char		*argv[])
{
	return finish_update(true);
}

static struct cmdinfo startupdate_cmd = {
	.name		= "startupdate",
	.cfunc		= startupdate_f,
	.argmin		= 0,
	.argmax		= 0,
	.flags		= CMD_FLAG_ONESHOT | CMD_NOMAP_OK,
	.help		= startupdate_help,
};

static struct cmdinfo cancelupdate_cmd = {
	.name		= "cancelupdate",
	.cfunc		= cancelupdate_f,
	.argmin		= 0,
	.argmax		= 0,
	.flags		= CMD_FLAG_ONESHOT | CMD_NOMAP_OK,
	.help		= cancelupdate_help,
};

static struct cmdinfo commitupdate_cmd = {
	.name		= "commitupdate",
	.cfunc		= commitupdate_f,
	.argmin		= 0,
	.argmax		= 0,
	.flags		= CMD_FLAG_ONESHOT | CMD_NOMAP_OK,
	.help		= commitupdate_help,
};

void
atomicupdate_init(void)
{
	startupdate_cmd.oneline = _("start an atomic update of a file");

	cancelupdate_cmd.oneline = _("cancel an atomic update");

	commitupdate_cmd.oneline = _("commit a file update atomically");

	add_command(&startupdate_cmd);
	add_command(&cancelupdate_cmd);
	add_command(&commitupdate_cmd);
}

