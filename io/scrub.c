// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2017 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */

#include <sys/uio.h>
#include <xfs/xfs.h>
#include "command.h"
#include "input.h"
#include "init.h"
#include "libfrog/paths.h"
#include "libfrog/fsgeom.h"
#include "libfrog/scrub.h"
#include "libfrog/logging.h"
#include "io.h"
#include "list.h"

static struct cmdinfo scrub_cmd;
static struct cmdinfo repair_cmd;
static const struct cmdinfo scrubv_cmd;

static void
scrub_help(void)
{
	const struct xfrog_scrub_descr	*d;
	int				i;

	printf(_(
"\n"
" Scrubs a piece of XFS filesystem metadata.  The first argument is the type\n"
" of metadata to examine.  Allocation group metadata types take one AG number\n"
" as the second parameter.  Inode metadata types act on the currently open file\n"
" or (optionally) take an inode number and generation number to act upon as\n"
" the second and third parameters.\n"
"\n"
" Example:\n"
" 'scrub inobt 3' - scrub the inode btree in AG 3.\n"
" 'scrub bmapbtd 128 13525' - scrubs the extent map of inode 128 gen 13525.\n"
"\n"
" Known metadata scrub types are:"));
	for (i = 0, d = xfrog_scrubbers; i < XFS_SCRUB_TYPE_NR; i++, d++)
		printf(" %s", d->name);
	printf("\n");
}

static void
report_scrub_outcome(
	uint32_t	flags)
{
	if (flags & XFS_SCRUB_OFLAG_CORRUPT)
		printf(_("Corruption detected.\n"));
	if (flags & XFS_SCRUB_OFLAG_PREEN)
		printf(_("Optimization possible.\n"));
	if (flags & XFS_SCRUB_OFLAG_XFAIL)
		printf(_("Cross-referencing failed.\n"));
	if (flags & XFS_SCRUB_OFLAG_XCORRUPT)
		printf(_("Corruption detected during cross-referencing.\n"));
	if (flags & XFS_SCRUB_OFLAG_INCOMPLETE)
		printf(_("Scan was not complete.\n"));
}

static void
scrub_ioctl(
	int				fd,
	int				type,
	uint64_t			control,
	uint32_t			control2,
	uint32_t			flags)
{
	struct xfs_scrub_metadata	meta;
	const struct xfrog_scrub_descr	*sc;
	int				error;

	sc = &xfrog_scrubbers[type];
	memset(&meta, 0, sizeof(meta));
	meta.sm_type = type;
	switch (sc->group) {
	case XFROG_SCRUB_GROUP_AGHEADER:
	case XFROG_SCRUB_GROUP_PERAG:
		meta.sm_agno = control;
		break;
	case XFROG_SCRUB_GROUP_INODE:
		meta.sm_ino = control;
		meta.sm_gen = control2;
		break;
	case XFROG_SCRUB_GROUP_NONE:
	case XFROG_SCRUB_GROUP_RT:
	case XFROG_SCRUB_GROUP_QUOTA:
	case XFROG_SCRUB_GROUP_SUMMARY:
	case XFROG_SCRUB_GROUP_ISCAN:
		/* no control parameters */
		break;
	}
	meta.sm_flags = flags;

	error = ioctl(fd, XFS_IOC_SCRUB_METADATA, &meta);
	if (error)
		perror("scrub");
	report_scrub_outcome(meta.sm_flags);
}

static int
parse_args(
	int				argc,
	char				**argv,
	struct cmdinfo			*cmdinfo,
	void				(*fn)(int, int, uint64_t, uint32_t, uint32_t))
{
	char				*p;
	int				type = -1;
	int				i, c;
	uint64_t			control = 0;
	uint32_t			control2 = 0;
	uint32_t			flags = 0;
	const struct xfrog_scrub_descr	*d = NULL;

	while ((c = getopt(argc, argv, "R")) != EOF) {
		switch (c) {
		case 'R':
			flags |= XFS_SCRUB_IFLAG_FORCE_REBUILD;
			break;
		default:
			return command_usage(cmdinfo);
		}
	}
	if (optind > argc - 1)
		return command_usage(cmdinfo);

	for (i = 0, d = xfrog_scrubbers; i < XFS_SCRUB_TYPE_NR; i++, d++) {
		if (strcmp(d->name, argv[optind]) == 0) {
			type = i;
			break;
		}
	}
	if (type < 0) {
		printf(_("Unknown type '%s'.\n"), argv[optind]);
		return command_usage(cmdinfo);
	}
	optind++;

	switch (d->group) {
	case XFROG_SCRUB_GROUP_INODE:
		if (optind == argc) {
			control = 0;
			control2 = 0;
		} else if (optind == argc - 2) {
			control = strtoull(argv[optind], &p, 0);
			if (*p != '\0') {
				fprintf(stderr,
					_("Bad inode number '%s'.\n"),
					argv[optind]);
				return 0;
			}
			control2 = strtoul(argv[optind + 1], &p, 0);
			if (*p != '\0') {
				fprintf(stderr,
					_("Bad generation number '%s'.\n"),
					argv[optind + 1]);
				return 0;
			}
		} else {
			fprintf(stderr,
				_("Must specify inode number and generation.\n"));
			return 0;
		}
		break;
	case XFROG_SCRUB_GROUP_AGHEADER:
	case XFROG_SCRUB_GROUP_PERAG:
		if (optind != argc - 1) {
			fprintf(stderr,
				_("Must specify one AG number.\n"));
			return 0;
		}
		control = strtoul(argv[optind], &p, 0);
		if (*p != '\0') {
			fprintf(stderr,
				_("Bad AG number '%s'.\n"), argv[optind]);
			return 0;
		}
		break;
	case XFROG_SCRUB_GROUP_RT:
	case XFROG_SCRUB_GROUP_QUOTA:
	case XFROG_SCRUB_GROUP_NONE:
	case XFROG_SCRUB_GROUP_SUMMARY:
	case XFROG_SCRUB_GROUP_ISCAN:
		if (optind != argc) {
			fprintf(stderr,
				_("No parameters allowed.\n"));
			return 0;
		}
		break;
	default:
		ASSERT(0);
		break;
	}
	fn(file->fd, type, control, control2, flags);

	return 0;
}

static int
scrub_f(
	int				argc,
	char				**argv)
{
	return parse_args(argc, argv, &scrub_cmd, scrub_ioctl);
}

void
scrub_init(void)
{
	scrub_cmd.name = "scrub";
	scrub_cmd.altname = "sc";
	scrub_cmd.cfunc = scrub_f;
	scrub_cmd.argmin = 1;
	scrub_cmd.argmax = -1;
	scrub_cmd.flags = CMD_NOMAP_OK;
	scrub_cmd.args = _("type [agno|ino gen]");
	scrub_cmd.oneline = _("scrubs filesystem metadata");
	scrub_cmd.help = scrub_help;

	add_command(&scrub_cmd);
	add_command(&scrubv_cmd);
}

static void
repair_help(void)
{
	const struct xfrog_scrub_descr	*d;
	int				i;

	printf(_(
"\n"
" Repairs a piece of XFS filesystem metadata.  The first argument is the type\n"
" of metadata to examine.  Allocation group metadata types take one AG number\n"
" as the second parameter.  Inode metadata types act on the currently open file\n"
" or (optionally) take an inode number and generation number to act upon as\n"
" the second and third parameters.\n"
"\n"
" Flags are -R to rebuild metadata.\n"
"\n"
" Example:\n"
" 'repair inobt 3' - repairs the inode btree in AG 3.\n"
" 'repair bmapbtd 128 13525' - repairs the extent map of inode 128 gen 13525.\n"
"\n"
" Known metadata repairs types are:"));
	for (i = 0, d = xfrog_scrubbers; i < XFS_SCRUB_TYPE_NR; i++, d++)
		printf(" %s", d->name);
	printf("\n");
}

static void
report_repair_outcome(
	uint32_t	flags)
{
	if (flags & XFS_SCRUB_OFLAG_CORRUPT)
		printf(_("Corruption remains.\n"));
	if (flags & XFS_SCRUB_OFLAG_PREEN)
		printf(_("Optimization possible.\n"));
	if (flags & XFS_SCRUB_OFLAG_XFAIL)
		printf(_("Cross-referencing failed.\n"));
	if (flags & XFS_SCRUB_OFLAG_XCORRUPT)
		printf(_("Corruption still detected during cross-referencing.\n"));
	if (flags & XFS_SCRUB_OFLAG_INCOMPLETE)
		printf(_("Repair was not complete.\n"));
	if (flags & XFS_SCRUB_OFLAG_NO_REPAIR_NEEDED)
		printf(_("Metadata did not need repair or optimization.\n"));
}

static void
repair_ioctl(
	int				fd,
	int				type,
	uint64_t			control,
	uint32_t			control2,
	uint32_t			flags)
{
	struct xfs_scrub_metadata	meta;
	const struct xfrog_scrub_descr	*sc;
	int				error;

	sc = &xfrog_scrubbers[type];
	memset(&meta, 0, sizeof(meta));
	meta.sm_type = type;
	switch (sc->group) {
	case XFROG_SCRUB_GROUP_AGHEADER:
	case XFROG_SCRUB_GROUP_PERAG:
		meta.sm_agno = control;
		break;
	case XFROG_SCRUB_GROUP_INODE:
		meta.sm_ino = control;
		meta.sm_gen = control2;
		break;
	case XFROG_SCRUB_GROUP_NONE:
	case XFROG_SCRUB_GROUP_RT:
	case XFROG_SCRUB_GROUP_QUOTA:
	case XFROG_SCRUB_GROUP_SUMMARY:
	case XFROG_SCRUB_GROUP_ISCAN:
		/* no control parameters */
		break;
	}
	meta.sm_flags = flags | XFS_SCRUB_IFLAG_REPAIR;

	error = ioctl(fd, XFS_IOC_SCRUB_METADATA, &meta);
	if (error)
		perror("repair");
	report_repair_outcome(meta.sm_flags);
}

static int
repair_f(
	int				argc,
	char				**argv)
{
	return parse_args(argc, argv, &repair_cmd, repair_ioctl);
}

void
repair_init(void)
{
	if (!expert)
		return;
	repair_cmd.name = "repair";
	repair_cmd.altname = "fix";
	repair_cmd.cfunc = repair_f;
	repair_cmd.argmin = 1;
	repair_cmd.argmax = -1;
	repair_cmd.flags = CMD_NOMAP_OK;
	repair_cmd.args = _("type [agno|ino gen]");
	repair_cmd.oneline = _("repairs filesystem metadata");
	repair_cmd.help = repair_help;

	add_command(&repair_cmd);
}

static void
scrubv_help(void)
{
	printf(_(
"\n"
" Scrubs pieces of XFS filesystem metadata.  The first argument is the group\n"
" of metadata to examine.  If the group is 'ag', the second parameter should\n"
" be the AG number.  If the group is 'inode', the second and third parameters\n"
" should be the inode number and generation number to act upon; if these are\n"
" omitted, the scrub is performed on the open file.  If the group is 'fs',\n"
" 'summary', or 'probe', there are no other parameters.\n"
"\n"
" Flags are -d for debug, and -r to allow repairs.\n"
" -b NN will insert a scrub barrier after every NN scrubs, and -m sets the\n"
" desired corruption mask in all barriers. -w pauses for some microseconds\n"
" after each scrub call.\n"
"\n"
" Example:\n"
" 'scrubv ag 3' - scrub all metadata in AG 3.\n"
" 'scrubv ag 3 -b 2 -m 0x4' - scrub all metadata in AG 3, and use barriers\n"
"            every third scrub to exit early if there are optimizations.\n"
" 'scrubv fs' - scrub all non-AG non-file metadata.\n"
" 'scrubv inode' - scrub all metadata for the open file.\n"
" 'scrubv inode 128 13525' - scrub all metadata for inode 128 gen 13525.\n"
" 'scrubv probe' - check for presence of online scrub.\n"
" 'scrubv summary' - scrub all summary metadata.\n"));
}

/* Fill out the scrub vectors for a group of scrubber (ag, ino, fs, summary) */
static void
scrubv_fill_group(
	struct xfs_scrub_vec_head	*vhead,
	int				barrier_interval,
	__u32				barrier_mask,
	enum xfrog_scrub_group		group)
{
	const struct xfrog_scrub_descr	*d;
	unsigned int			i;

	for (i = 0, d = xfrog_scrubbers; i < XFS_SCRUB_TYPE_NR; i++, d++) {
		if (d->group != group)
			continue;
		vhead->svh_vecs[vhead->svh_nr++].sv_type = i;

		if (barrier_interval &&
		    vhead->svh_nr % (barrier_interval + 1) == 0) {
			struct xfs_scrub_vec	*v;

			v = &vhead->svh_vecs[vhead->svh_nr++];
			v->sv_flags = barrier_mask;
			v->sv_type = XFS_SCRUB_TYPE_BARRIER;
		}
	}
}

/* Declare a structure big enough to handle all scrub types + barriers */
struct scrubv_head {
	struct xfs_scrub_vec_head	head;
	struct xfs_scrub_vec		__vecs[XFS_SCRUB_TYPE_NR * 2];
};


static int
scrubv_f(
	int				argc,
	char				**argv)
{
	struct scrubv_head		bighead = { };
	struct xfs_fd			xfd = XFS_FD_INIT(file->fd);
	struct xfs_scrub_vec_head	*vhead = &bighead.head;
	struct xfs_scrub_vec		*v;
	char				*p;
	uint32_t			flags = 0;
	__u32				barrier_mask = XFS_SCRUB_OFLAG_CORRUPT;
	enum xfrog_scrub_group		group;
	bool				debug = false;
	int				version = -1;
	int				barrier_interval = 0;
	int				rest_us = 0;
	int				c;
	int				error;

	while ((c = getopt(argc, argv, "b:dm:rv:w:")) != EOF) {
		switch (c) {
		case 'b':
			barrier_interval = atoi(optarg);
			if (barrier_interval < 0) {
				printf(
		_("Negative barrier interval makes no sense.\n"));
				return 0;
			}
			break;
		case 'd':
			debug = true;
			break;
		case 'm':
			barrier_mask = strtoul(optarg, NULL, 0);
			break;
		case 'r':
			flags |= XFS_SCRUB_IFLAG_REPAIR;
			break;
		case 'v':
			version = atoi(optarg);
			if (version != 0 && version != 1) {
				printf(_("API version must be 0 or 1.\n"));
				return 0;
			}
			break;
		case 'w':
			rest_us = atoi(optarg);
			if (rest_us < 0) {
				printf(_("Rest time must be positive.\n"));
				return 0;
			}
			break;
		default:
			scrubv_help();
			return 0;
		}
	}
	if (optind > argc - 1) {
		scrubv_help();
		return 0;
	}

	if ((flags & XFS_SCRUB_IFLAG_REPAIR) && !expert) {
		printf(_("Repair flag requires expert mode.\n"));
		return 1;
	}

	vhead->svh_rest_us = rest_us;
	for (c = 0, v = vhead->svh_vecs; c < vhead->svh_nr; c++, v++)
		v->sv_flags = flags;

	/* Extract group and domain information from cmdline. */
	if (!strcmp(argv[optind], "ag"))
		group = XFROG_SCRUB_GROUP_PERAG;
	else if (!strcmp(argv[optind], "inode"))
		group = XFROG_SCRUB_GROUP_INODE;
	else if (!strcmp(argv[optind], "rt"))
		group = XFROG_SCRUB_GROUP_RT;
	else if (!strcmp(argv[optind], "quota"))
		group = XFROG_SCRUB_GROUP_QUOTA;
	else if (!strcmp(argv[optind], "summary"))
		group = XFROG_SCRUB_GROUP_SUMMARY;
	else if (!strcmp(argv[optind], "iscan"))
		group = XFROG_SCRUB_GROUP_ISCAN;
	else if (!strcmp(argv[optind], "probe"))
		group = XFROG_SCRUB_GROUP_NONE;
	else {
		printf(_("Unknown group '%s'.\n"), argv[optind]);
		scrubv_help();
		return 0;
	}
	optind++;

	switch (group) {
	case XFROG_SCRUB_GROUP_INODE:
		if (optind == argc) {
			vhead->svh_ino = 0;
			vhead->svh_gen = 0;
		} else if (optind == argc - 2) {
			vhead->svh_ino = strtoull(argv[optind], &p, 0);
			if (*p != '\0') {
				fprintf(stderr,
					_("Bad inode number '%s'.\n"),
					argv[optind]);
				return 0;
			}
			vhead->svh_gen = strtoul(argv[optind + 1], &p, 0);
			if (*p != '\0') {
				fprintf(stderr,
					_("Bad generation number '%s'.\n"),
					argv[optind + 1]);
				return 0;
			}
		} else {
			fprintf(stderr,
				_("Must specify inode number and generation.\n"));
			return 0;
		}
		break;
	case XFROG_SCRUB_GROUP_PERAG:
		if (optind != argc - 1) {
			fprintf(stderr,
				_("Must specify one AG number.\n"));
			return 0;
		}
		vhead->svh_agno = strtoul(argv[optind], &p, 0);
		if (*p != '\0') {
			fprintf(stderr,
				_("Bad AG number '%s'.\n"), argv[optind]);
			return 0;
		}
		scrubv_fill_group(vhead, barrier_interval, barrier_mask,
				XFROG_SCRUB_GROUP_AGHEADER);
		break;
	case XFROG_SCRUB_GROUP_RT:
	case XFROG_SCRUB_GROUP_QUOTA:
	case XFROG_SCRUB_GROUP_SUMMARY:
	case XFROG_SCRUB_GROUP_ISCAN:
	case XFROG_SCRUB_GROUP_NONE:
		if (optind != argc) {
			fprintf(stderr,
				_("No parameters allowed.\n"));
			return 0;
		}
		break;
	default:
		ASSERT(0);
		break;
	}
	scrubv_fill_group(vhead, barrier_interval, barrier_mask, group);
	assert(vhead->svh_nr < ARRAY_SIZE(bighead.__vecs));

	error = -xfd_prepare_geometry(&xfd);
	if (error) {
		xfrog_perror(error, "xfd_prepare_geometry");
		exitcode = 1;
		return 0;
	}

	switch (version) {
	case 0:
		xfd.flags |= XFROG_FLAG_SCRUB_FORCE_SINGLE;
		break;
	case 1:
		xfd.flags |= XFROG_FLAG_SCRUB_FORCE_VECTOR;
		break;
	default:
		break;
	}

	error = -xfrog_scrubv_metadata(&xfd, vhead);
	if (error) {
		xfrog_perror(error, "xfrog_scrub_many");
		exitcode = 1;
		return 0;
	}

	/* Figure out what happened. */
	for (c = 0, v = vhead->svh_vecs; debug && c < vhead->svh_nr; c++, v++) {
		const char	*type;

		if (v->sv_type == XFS_SCRUB_TYPE_BARRIER)
			type = _("barrier");
		else
			type = _(xfrog_scrubbers[v->sv_type].descr);
		printf(_("[%02u] %-25s: flags 0x%x ret %d\n"), c, type,
				v->sv_flags, v->sv_ret);
	}

	/* Figure out what happened. */
	for (c = 0, v = vhead->svh_vecs; c < vhead->svh_nr; c++, v++) {
		/* Report barrier failures. */
		if (v->sv_type == XFS_SCRUB_TYPE_BARRIER) {
			if (v->sv_ret) {
				printf(_("barrier: FAILED\n"));
				break;
			}
			continue;
		}

		printf("%s: ", _(xfrog_scrubbers[v->sv_type].descr));
		switch (v->sv_ret) {
		case 0:
			break;
		default:
			printf("%s\n", strerror(-v->sv_ret));
			continue;
		}
		if (!(v->sv_flags & XFS_SCRUB_FLAGS_OUT))
			printf(_("OK.\n"));
		else if (v->sv_flags & XFS_SCRUB_IFLAG_REPAIR)
			report_repair_outcome(v->sv_flags);
		else
			report_scrub_outcome(v->sv_flags);
	}

	return 0;
}

static const struct cmdinfo scrubv_cmd = {
	.name		= "scrubv",
	.cfunc		= scrubv_f,
	.argmin		= 1,
	.argmax		= -1,
	.flags		= CMD_NOMAP_OK,
	.oneline	= N_("vectored metadata scrub"),
	.help		= scrubv_help,
};
