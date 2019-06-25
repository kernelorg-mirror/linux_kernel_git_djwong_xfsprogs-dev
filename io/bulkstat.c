// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2019 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "platform_defs.h"
#include "command.h"
#include "init.h"
#include "path.h"
#include "io.h"
#include "input.h"
#include "xfrog.h"

static bool debug;

static void
dump_bulkstat_time(
	const char		*tag,
	uint64_t		sec,
	uint32_t		nsec)
{
	printf("\t%s = %"PRIu64".%"PRIu32"\n", tag, sec, nsec);
}

static void
dump_bulkstat(
	struct xfs_bulkstat	*bstat)
{
	printf("bs_ino = %"PRIu64"\n", bstat->bs_ino);
	printf("\tbs_size = %"PRIu64"\n", bstat->bs_size);

	printf("\tbs_blocks = %"PRIu64"\n", bstat->bs_blocks);
	printf("\tbs_xflags = 0x%"PRIx64"\n", bstat->bs_xflags);

	dump_bulkstat_time("bs_atime", bstat->bs_atime, bstat->bs_atime_nsec);
	dump_bulkstat_time("bs_ctime", bstat->bs_ctime, bstat->bs_ctime_nsec);
	dump_bulkstat_time("bs_mtime", bstat->bs_mtime, bstat->bs_mtime_nsec);
	dump_bulkstat_time("bs_btime", bstat->bs_btime, bstat->bs_btime_nsec);

	printf("\tbs_gen = 0x%"PRIx32"\n", bstat->bs_gen);
	printf("\tbs_uid = %"PRIu32"\n", bstat->bs_uid);
	printf("\tbs_gid = %"PRIu32"\n", bstat->bs_gid);
	printf("\tbs_projectid = %"PRIu32"\n", bstat->bs_projectid);

	printf("\tbs_blksize = %"PRIu32"\n", bstat->bs_blksize);
	printf("\tbs_rdev = %"PRIu32"\n", bstat->bs_rdev);
	printf("\tbs_cowextsize_blks = %"PRIu32"\n", bstat->bs_cowextsize_blks);
	printf("\tbs_extsize_blks = %"PRIu32"\n", bstat->bs_extsize_blks);

	printf("\tbs_nlink = %"PRIu32"\n", bstat->bs_nlink);
	printf("\tbs_extents = %"PRIu32"\n", bstat->bs_extents);
	printf("\tbs_aextents = %"PRIu32"\n", bstat->bs_aextents);
	printf("\tbs_version = %"PRIu16"\n", bstat->bs_version);
	printf("\tbs_forkoff = %"PRIu16"\n", bstat->bs_forkoff);

	printf("\tbs_sick = 0x%"PRIx16"\n", bstat->bs_sick);
	printf("\tbs_checked = 0x%"PRIx16"\n", bstat->bs_checked);
	printf("\tbs_mode = 0%"PRIo16"\n", bstat->bs_mode);
};

static void
bulkstat_help(void)
{
	printf(_(
"Bulk-queries the filesystem for inode stat information and prints it.\n"
"\n"
"   -a   Only iterate this AG.\n"
"   -d   Print debugging output.\n"
"   -e   Stop after this inode.\n"
"   -n   Ask for this many results at once.\n"
"   -s   Inode to start with.\n"
"   -v   Use this version of the ioctl (1 or 5).\n"));
}

static int
bulkstat_f(
	int			argc,
	char			**argv)
{
	struct xfs_fd		xfd = XFS_FD_INIT(file->fd);
	struct xfs_bulkstat_req	*breq;
	unsigned long long	startino = 0;
	unsigned long long	endino = -1ULL;
	unsigned long		batch_size = 4096;
	unsigned long		agno = 0;
	unsigned long		ver = 0;
	bool			has_agno = false;
	unsigned int		i;
	int			c;
	int			error;

	while ((c = getopt(argc, argv, "a:cde:n:qs:v:")) != -1) {
		switch (c) {
		case 'a':
			errno = 0;
			agno = strtoul(optarg, NULL, 10);
			if (errno) {
				perror(optarg);
				return 1;
			}
			has_agno = true;
			break;
		case 'd':
			debug = true;
			break;
		case 'e':
			errno = 0;
			endino = strtoull(optarg, NULL, 10);
			if (errno) {
				perror(optarg);
				return 1;
			}
			break;
		case 'n':
			errno = 0;
			batch_size = strtoul(optarg, NULL, 10);
			if (errno) {
				perror(optarg);
				return 1;
			}
			break;
		case 's':
			errno = 0;
			startino = strtoull(optarg, NULL, 10);
			if (errno) {
				perror(optarg);
				return 1;
			}
			break;
		case 'v':
			errno = 0;
			ver = strtoull(optarg, NULL, 10);
			if (errno) {
				perror(optarg);
				return 1;
			}
			if (ver != 1 && ver != 5) {
				fprintf(stderr, "version must be 1 or 5.\n");
				return 1;
			}
			break;
		default:
			bulkstat_help();
			return 0;
		}
	}
	if (optind != argc) {
		bulkstat_help();
		return 0;
	}

	error = xfrog_prepare_geometry(&xfd);
	if (error) {
		perror("xfrog_prepare_geometry");
		exitcode = 1;
		return 0;
	}

	breq = xfrog_bulkstat_alloc_req(batch_size, startino);
	if (!breq) {
		perror("alloc bulkreq");
		exitcode = 1;
		return 0;
	}

	if (has_agno)
		xfrog_bulkstat_set_ag(breq, agno);

	switch (ver) {
	case 1:
		xfd.flags |= XFROG_FLAG_BULKSTAT_FORCE_V1;
		break;
	case 5:
		xfd.flags |= XFROG_FLAG_BULKSTAT_FORCE_V5;
		break;
	default:
		break;
	}

	while ((error = xfrog_bulkstat(&xfd, breq)) == 0) {
		if (debug)
			printf(
_("bulkstat: startino=%"PRIu64" flags=0x%"PRIx32" agno=%"PRIu32" ret=%d icount=%"PRIu32" ocount=%"PRIu32"\n"),
				breq->hdr.ino,
				breq->hdr.flags,
				breq->hdr.agno,
				error,
				breq->hdr.icount,
				breq->hdr.ocount);
		if (breq->hdr.ocount == 0)
			break;

		for (i = 0; i < breq->hdr.ocount; i++) {
			if (breq->bulkstat[i].bs_ino > endino)
				break;
			dump_bulkstat(&breq->bulkstat[i]);
		}
	}
	if (error) {
		perror("xfrog_bulkstat");
		exitcode = 1;
		return 0;
	}

	free(breq);
	return 0;
}

static void
bulkstat_single_help(void)
{
	printf(_(
"Queries the filesystem for a single inode's stat information and prints it.\n"
"\n"
"   -v   Use this version of the ioctl (1 or 5).\n"
"\n"
"Pass in inode numbers or a special inode name:\n"
"    root    Root directory.\n"));
}

struct single_map {
	const char		*tag;
	uint64_t		code;
};

struct single_map tags[] = {
	{"root", XFS_BULK_IREQ_SPECIAL_ROOT},
	{NULL, 0},
};

static int
bulkstat_single_f(
	int			argc,
	char			**argv)
{
	struct xfs_fd		xfd = XFS_FD_INIT(file->fd);
	struct xfs_bulkstat	bulkstat;
	unsigned long		ver = 0;
	unsigned int		i;
	int			c;
	int			error;

	while ((c = getopt(argc, argv, "v:")) != -1) {
		switch (c) {
		case 'v':
			errno = 0;
			ver = strtoull(optarg, NULL, 10);
			if (errno) {
				perror(optarg);
				return 1;
			}
			if (ver != 1 && ver != 5) {
				fprintf(stderr, "version must be 1 or 5.\n");
				return 1;
			}
			break;
		default:
			bulkstat_single_help();
			return 0;
		}
	}

	error = xfrog_prepare_geometry(&xfd);
	if (error) {
		perror("xfrog_prepare_geometry");
		exitcode = 1;
		return 0;
	}

	switch (ver) {
	case 1:
		xfd.flags |= XFROG_FLAG_BULKSTAT_FORCE_V1;
		break;
	case 5:
		xfd.flags |= XFROG_FLAG_BULKSTAT_FORCE_V5;
		break;
	default:
		break;
	}

	for (i = optind; i < argc; i++) {
		struct single_map	*sm = tags;
		uint64_t		ino;
		unsigned int		flags = 0;

		/* Try to look up our tag... */
		for (sm = tags; sm->tag; sm++) {
			if (!strcmp(argv[i], sm->tag)) {
				ino = sm->code;
				flags |= XFS_BULK_IREQ_SPECIAL;
				break;
			}
		}

		/* ...or else it's an inode number. */
		if (sm->tag == NULL) {
			errno = 0;
			ino = strtoull(argv[i], NULL, 10);
			if (errno) {
				perror(argv[i]);
				exitcode = 1;
				return 0;
			}
		}

		error = xfrog_bulkstat_single(&xfd, ino, flags, &bulkstat);
		if (error) {
			perror("xfrog_bulkstat_single");
			continue;
		}

		if (debug)
			printf(
_("bulkstat_single: startino=%"PRIu64" flags=0x%"PRIx32" ret=%d\n"),
				ino, flags, error);

		dump_bulkstat(&bulkstat);
	}

	return 0;
}

static void
dump_inumbers(
	struct xfs_inumbers	*inumbers)
{
	printf("xi_startino = %"PRIu64"\n", inumbers->xi_startino);
	printf("\txi_allocmask = 0x%"PRIx64"\n", inumbers->xi_allocmask);
	printf("\txi_alloccount = %"PRIu8"\n", inumbers->xi_alloccount);
	printf("\txi_version = %"PRIu8"\n", inumbers->xi_version);
}

static void
inumbers_help(void)
{
	printf(_(
"Queries the filesystem for inode group information and prints it.\n"
"\n"
"   -a   Only iterate this AG.\n"
"   -d   Print debugging output.\n"
"   -e   Stop after this inode.\n"
"   -n   Ask for this many results at once.\n"
"   -s   Inode to start with.\n"
"   -v   Use this version of the ioctl (1 or 5).\n"));
}

static int
inumbers_f(
	int			argc,
	char			**argv)
{
	struct xfs_fd		xfd = XFS_FD_INIT(file->fd);
	struct xfs_inumbers_req	*ireq;
	unsigned long long	startino = 0;
	unsigned long long	endino = -1ULL;
	unsigned long		batch_size = 4096;
	unsigned long		agno = 0;
	unsigned long		ver = 0;
	bool			has_agno = false;
	unsigned int		i;
	int			c;
	int			error;

	while ((c = getopt(argc, argv, "a:cde:n:qs:v:")) != -1) {
		switch (c) {
		case 'a':
			errno = 0;
			agno = strtoul(optarg, NULL, 10);
			if (errno) {
				perror(optarg);
				return 1;
			}
			has_agno = true;
			break;
		case 'd':
			debug = true;
			break;
		case 'e':
			errno = 0;
			endino = strtoull(optarg, NULL, 10);
			if (errno) {
				perror(optarg);
				return 1;
			}
			break;
		case 'n':
			errno = 0;
			batch_size = strtoul(optarg, NULL, 10);
			if (errno) {
				perror(optarg);
				return 1;
			}
			break;
		case 's':
			errno = 0;
			startino = strtoull(optarg, NULL, 10);
			if (errno) {
				perror(optarg);
				return 1;
			}
			break;
		case 'v':
			errno = 0;
			ver = strtoull(optarg, NULL, 10);
			if (errno) {
				perror(optarg);
				return 1;
			}
			if (ver != 1 && ver != 5) {
				fprintf(stderr, "version must be 1 or 5.\n");
				return 1;
			}
			break;
		default:
			bulkstat_help();
			return 0;
		}
	}
	if (optind != argc) {
		bulkstat_help();
		return 0;
	}

	error = xfrog_prepare_geometry(&xfd);
	if (error) {
		perror("xfrog_prepare_geometry");
		exitcode = 1;
		return 0;
	}

	ireq = xfrog_inumbers_alloc_req(batch_size, startino);
	if (!ireq) {
		perror("alloc inumbersreq");
		exitcode = 1;
		return 0;
	}

	if (has_agno)
		xfrog_inumbers_set_ag(ireq, agno);

	switch (ver) {
	case 1:
		xfd.flags |= XFROG_FLAG_BULKSTAT_FORCE_V1;
		break;
	case 5:
		xfd.flags |= XFROG_FLAG_BULKSTAT_FORCE_V5;
		break;
	default:
		break;
	}

	while ((error = xfrog_inumbers(&xfd, ireq)) == 0) {
		if (debug)
			printf(
_("bulkstat: startino=%"PRIu64" flags=0x%"PRIx32" agno=%"PRIu32" ret=%d icount=%"PRIu32" ocount=%"PRIu32"\n"),
				ireq->hdr.ino,
				ireq->hdr.flags,
				ireq->hdr.agno,
				error,
				ireq->hdr.icount,
				ireq->hdr.ocount);
		if (ireq->hdr.ocount == 0)
			break;

		for (i = 0; i < ireq->hdr.ocount; i++) {
			 if (ireq->inumbers[i].xi_startino > endino)
				 break;
			 dump_inumbers(&ireq->inumbers[i]);
		}
	}
	if (error) {
		perror("xfrog_inumbers");
		exitcode = 1;
		return 0;
	}

	free(ireq);
	return 0;
}

static cmdinfo_t	bulkstat_cmd = {
	.name = "bulkstat",
	.cfunc = bulkstat_f,
	.argmin = 0,
	.argmax = -1,
	.flags = CMD_NOMAP_OK,
	.help = bulkstat_help,
};

static cmdinfo_t	bulkstat_single_cmd = {
	.name = "bulkstat_single",
	.cfunc = bulkstat_single_f,
	.argmin = 0,
	.argmax = -1,
	.flags = CMD_NOMAP_OK,
	.help = bulkstat_single_help,
};

static cmdinfo_t	inumbers_cmd = {
	.name = "inumbers",
	.cfunc = inumbers_f,
	.argmin = 0,
	.argmax = -1,
	.flags = CMD_NOMAP_OK,
	.help = inumbers_help,
};

void
bulkstat_init(void)
{
	bulkstat_cmd.args = _("[-a agno] [-d] [-e endino] [-n batchsize] [-s startino]");
	bulkstat_cmd.oneline = _("Bulk stat of inodes in a filesystem");

	bulkstat_single_cmd.args = _("inum...");
	bulkstat_single_cmd.oneline = _("Stat one inode in a filesystem");

	inumbers_cmd.args = _("[-a agno] [-d] [-e endino] [-n batchsize] [-s startino]");
	inumbers_cmd.oneline = _("Query inode groups in a filesystem");

	add_command(&bulkstat_cmd);
	add_command(&bulkstat_single_cmd);
	add_command(&inumbers_cmd);
}
