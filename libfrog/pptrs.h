// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2023 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __LIBFROG_PPTRS_H_
#define	__LIBFROG_PPTRS_H_

struct path_list;

typedef int (*walk_pptr_fn)(struct xfs_pptr_info *pi, struct xfs_parent_ptr *pptr,
		void *arg);
typedef int (*walk_ppath_fn)(const char *mntpt, struct path_list *path,
		void *arg);

#define WALK_PPTRS_ABORT	1
int fd_walk_pptrs(int fd, walk_pptr_fn fn, void *arg);
int handle_walk_pptrs(void *hanp, size_t hanlen, walk_pptr_fn fn, void *arg);

#define WALK_PPATHS_ABORT	1
int fd_walk_ppaths(int fd, walk_ppath_fn fn, void *arg);
int handle_walk_ppaths(void *hanp, size_t hanlen, walk_ppath_fn fn, void *arg);

int fd_to_path(int fd, char *path, size_t pathlen);
int handle_to_path(void *hanp, size_t hlen, char *path, size_t pathlen);

#endif /* __LIBFROG_PPTRS_H_ */
