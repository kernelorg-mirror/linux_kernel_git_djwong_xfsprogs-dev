// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "libxfs.h"
#include "config.h"

/* Spit out a config file template. */
int
main(
	int			argc,
	char			*argv[])
{
	const struct cfg_section_map	*opts = cfgfile_map;
	const struct cfg_subopt_map	*submap;
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

	for (opts = cfgfile_map; opts->name; opts++) {
		if (manpage)
			printf(".PP\n.B ");
		else if (opts != cfgfile_map)
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
