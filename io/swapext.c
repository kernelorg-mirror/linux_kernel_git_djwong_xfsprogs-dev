// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018 Red Hat, Inc.
 * All Rights Reserved.
 */

#include "command.h"
#include "input.h"
#include "init.h"
#include "io.h"
#include "libfrog/logging.h"
#include "libfrog/fsgeom.h"
#include "libfrog/swapext.h"

static cmdinfo_t swapext_cmd;

static void
swapext_help(void)
{
	printf(_(
"\n"
" Swaps extents between the open file descriptor and the supplied filename.\n"
"\n"
" -a   -- use atomic extent swapping\n"
" -d N -- start swapping extents at this offset in the open file\n"
" -e   -- Swap extents to the ends of both files, including the file sizes\n"
" -f   -- Flush changed file data and metadata to disk\n"
" -l N -- swap this many bytes between the two files\n"
" -s N -- start swapping extents at this offset in the supplied file\n"
" -u   -- do not compare the open file's timestamps\n"
" -v   -- 'xfsv0' for the old XFS ioctl, or 'vfs' for the VFS ioctl\n"));
}

static void
set_xfd_flags(
	struct xfs_fd	*xfd,
	int		api_ver)
{
	switch (api_ver) {
	case 0:
		xfd->flags |= XFROG_FLAG_SWAPEXT_FORCE_V0;
		break;
	case 1:
		xfd->flags |= XFROG_FLAG_SWAPEXT_FORCE_VFS;
		break;
	default:
		break;
	}
}

static int
swapext_f(
	int			argc,
	char			**argv)
{
	struct xfs_fd		xfd = XFS_FD_INIT(file->fd);
	struct file_swap_range	fsr;
	struct stat		stat;
	uint64_t		flags = FILE_SWAP_RANGE_NONATOMIC |
					FILE_SWAP_RANGE_FILE2_FRESH |
					FILE_SWAP_RANGE_FULL_FILES;
	int64_t			src_offset = 0;
	int64_t			dest_offset = 0;
	int64_t			length = -1;
	size_t			fsblocksize, fssectsize;
	int			api_ver = -1;
	int			c;
	int			fd;
	int			ret;

	init_cvtnum(&fsblocksize, &fssectsize);
	while ((c = getopt(argc, argv, "ad:efl:s:uv:")) != -1) {
		switch (c) {
		case 'a':
			flags &= ~FILE_SWAP_RANGE_NONATOMIC;
			break;
		case 'd':
			dest_offset = cvtnum(fsblocksize, fssectsize, optarg);
			if (dest_offset < 0) {
				printf(
			_("non-numeric open file offset argument -- %s\n"),
						optarg);
				return 0;
			}
			flags &= ~FILE_SWAP_RANGE_FULL_FILES;
			break;
		case 'e':
			flags |= FILE_SWAP_RANGE_TO_EOF;
			flags &= ~FILE_SWAP_RANGE_FULL_FILES;
			break;
		case 'f':
			flags |= FILE_SWAP_RANGE_FSYNC;
			break;
		case 'l':
			length = cvtnum(fsblocksize, fssectsize, optarg);
			if (length < 0) {
				printf(
			_("non-numeric length argument -- %s\n"),
						optarg);
				return 0;
			}
			flags &= ~FILE_SWAP_RANGE_FULL_FILES;
			break;
		case 's':
			src_offset = cvtnum(fsblocksize, fssectsize, optarg);
			if (src_offset < 0) {
				printf(
			_("non-numeric supplied file offset argument -- %s\n"),
						optarg);
				return 0;
			}
			flags &= ~FILE_SWAP_RANGE_FULL_FILES;
			break;
		case 'u':
			flags &= ~FILE_SWAP_RANGE_FILE2_FRESH;
			break;
		case 'v':
			if (!strcmp(optarg, "xfsv0"))
				api_ver = 0;
			else if (!strcmp(optarg, "vfs"))
				api_ver = 1;
			else {
				fprintf(stderr,
			_("version must be 'xfsv0' or 'vfs'.\n"));
				return 1;
			}
			break;
		default:
			swapext_help();
			return 0;
		}
	}
	if (optind != argc - 1) {
		swapext_help();
		return 0;
	}

	/* open the donor file */
	fd = openfile(argv[optind], NULL, 0, 0, NULL);
	if (fd < 0)
		return 0;

	ret = -xfd_prepare_geometry(&xfd);
	if (ret) {
		xfrog_perror(ret, "xfd_prepare_geometry");
		exitcode = 1;
		goto out;
	}

	if (length < 0) {
		ret = fstat(file->fd, &stat);
		if (ret) {
			perror("fstat");
			exitcode = 1;
			goto out;
		}

		length = stat.st_size;
	}

	ret = xfrog_swapext_prep(&xfd, flags, dest_offset, fd, src_offset,
			length, &fsr);
	if (ret) {
		xfrog_perror(ret, "xfrog_swapext_prep");
		exitcode = 1;
		goto out;
	}

	set_xfd_flags(&xfd, api_ver);

	ret = xfrog_swapext(&xfd, &fsr);
	if (ret) {
		xfrog_perror(ret, "swapext");
		exitcode = 1;
		goto out;
	}
out:
	close(fd);
	return 0;
}

void
swapext_init(void)
{
	swapext_cmd.name = "swapext";
	swapext_cmd.cfunc = swapext_f;
	swapext_cmd.argmin = 1;
	swapext_cmd.argmax = -1;
	swapext_cmd.flags = CMD_NOMAP_OK;
	swapext_cmd.args = _("[-a] [-e] [-f] [-u] [-d dest_offset] [-s src_offset] [-l length] [-v xfsv0|vfs] <donorfile>");
	swapext_cmd.oneline = _("Swap extents between files.");
	swapext_cmd.help = swapext_help;

	add_command(&swapext_cmd);
}
