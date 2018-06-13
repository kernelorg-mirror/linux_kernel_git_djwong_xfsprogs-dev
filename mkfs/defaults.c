// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "libxfs.h"
#include "config.h"

/* build time feature defaults */
const struct sb_feat_args dft_features = {
	.log_version = 2,
	.attr_version = 2,
	.dir_version = 2,
	.inode_align = true,
	.nci = false,
	.lazy_sb_counters = true,
	.projid32bit = true,
	.crcs_enabled = true,
	.dirftype = true,
	.finobt = true,
	.spinodes = true,
	.rmapbt = false,
	.reflink = false,
	.parent_pointers = false,
	.nodalign = false,
	.nortalign = false,
};
