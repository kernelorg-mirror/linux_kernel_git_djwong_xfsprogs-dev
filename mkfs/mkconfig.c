// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "libxfs.h"
#include "config.h"

enum cfgfile_var_type {
	FV_BOOL = 0
};

struct subopt_map {
	const char		*suboptname;
	const void		*ptr;
	enum cfgfile_var_type	type;
};

/* Map all the config file options to their dft_features equivalents. */
struct confopts {
	const char		*name;
	struct subopt_map	subopts[CFG_MAX_SUBOPTS];
} confopts_tab[] = {
	{
		.name = "data",
		.subopts = {
			[CFG_D_NOALIGN] = {
				.suboptname	= "noalign",
				.ptr		= &dft_features.nodalign,
				.type		= FV_BOOL,
			},
			{NULL}
		},
	},
	{
		.name = "inode",
		.subopts = {
			[CFG_I_ALIGN] = {
				.suboptname	= "align",
				.ptr		= &dft_features.inode_align,
				.type		= FV_BOOL,
			},
			[CFG_I_PROJID32BIT] = {
				.suboptname	= "projid32bit",
				.ptr		= &dft_features.projid32bit,
				.type		= FV_BOOL,
			},
			[CFG_I_SPINODES] = {
				.suboptname	= "sparse",
				.ptr		= &dft_features.spinodes,
				.type		= FV_BOOL,
			},
			{NULL}
		},
	},
	{
		.name = "log",
		.subopts = {
			[CFG_L_LAZYSBCNTR] = {
				.suboptname	= "lazy-count",
				.ptr		= &dft_features.lazy_sb_counters,
				.type		= FV_BOOL,
			},
			{NULL}
		},
	},
	{
		.name = "metadata",
		.subopts = {
			[CFG_M_CRC] = {
				.suboptname	= "crc",
				.ptr		= &dft_features.crcs_enabled,
				.type		= FV_BOOL,
			},
			[CFG_M_FINOBT] = {
				.suboptname	= "finobt",
				.ptr		= &dft_features.finobt,
				.type		= FV_BOOL,
			},
			[CFG_M_RMAPBT] = {
				.suboptname	= "rmapbt",
				.ptr		= &dft_features.rmapbt,
				.type		= FV_BOOL,
			},
			[CFG_M_REFLINK] = {
				.suboptname	= "reflink",
				.ptr		= &dft_features.reflink,
				.type		= FV_BOOL,
			},
			{NULL}
		},
	},
	{
		.name = "naming",
		.subopts = {
			[CFG_N_FTYPE] = {
				.suboptname	= "ftype",
				.ptr		= &dft_features.dirftype,
				.type		= FV_BOOL,
			},
			{NULL}
		},
	},
	{
		.name = "rtdev",
		.subopts = {
			[CFG_R_NOALIGN] = {
				.suboptname	= "noalign",
				.ptr		= &dft_features.nortalign,
				.type		= FV_BOOL,
			},
			{NULL}
		},
	},
	{NULL},
};

/* Spit out a config file template. */
int
main(
	int			argc,
	char			*argv[])
{
	struct confopts		*opts = confopts_tab;
	struct subopt_map	*submap;
	int			c;
	bool			manpage = false;

	while ((c = getopt(argc, argv, "m")) != EOF) {
		switch (c) {
		case 'm':
			manpage = true;
			break;
		case '?':
			fprintf(stderr, "Unknown option %c.\n", optopt);
			return 1;
		}
	}
	if (argc - optind != 0) {
		fprintf(stderr, "extra arguments\n");
		return 1;
	}

	if (!manpage) {
		printf("# mkfs.xfs configuration file to collect settings.\n");
		printf("# See the mkfs.xfs(8) manpage for details.\n");
		printf("# Copy this file to %s/%s to override the built-in defaults.\n",
				MKFS_XFS_CONF_DIR, MKFS_XFS_DEFAULT_CONFIG);
	}

	for (opts = confopts_tab; opts->name; opts++) {
		if (manpage)
			printf(".PP\n.B ");
		else if (opts != confopts_tab)
			printf("\n");
		printf("[%s]\n", opts->name);
		for (submap = opts->subopts; submap->suboptname; submap++) {
			if (manpage)
				printf(".br\n.B ");
			printf("%s = ", submap->suboptname);
			switch (submap->type) {
			case FV_BOOL:
				printf("%d\n", *((bool *)submap->ptr));
				break;
			}
		}
	}

	return 0;
}
