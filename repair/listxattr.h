/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2022 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __REPAIR_LISTXATTR_H__
#define __REPAIR_LISTXATTR_H__

typedef int (*listxattr_fn)(struct xfs_inode *ip, unsigned int attr_flags,
		const void *name, unsigned int namelen, const void *value,
		unsigned int valuelen, void *priv);

int listxattr(struct xfs_inode *ip, listxattr_fn attr_fn, void *priv);

#endif /* __REPAIR_LISTXATTR_H__ */
