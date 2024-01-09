/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2021-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __LIBXFS_XFILE_H__
#define __LIBXFS_XFILE_H__

struct xfile_fcb {
	struct list_head	fcb_list;
	int			fd;
	unsigned int		refcount;
};

struct xfile {
	struct xfile_fcb	*fcb;
	loff_t			partition_pos;
	uint64_t		partition_bytes;
};

int xfile_create(const char *description, unsigned long long maxpos,
		struct xfile **xfilep);
void xfile_destroy(struct xfile *xf);

ssize_t xfile_load(struct xfile *xf, void *buf, size_t count, loff_t pos);
ssize_t xfile_store(struct xfile *xf, const void *buf, size_t count, loff_t pos);

struct xfile_stat {
	loff_t			size;
	unsigned long long	bytes;
};

int xfile_stat(struct xfile *xf, struct xfile_stat *statbuf);
unsigned long long xfile_bytes(struct xfile *xf);
void xfile_discard(struct xfile *xf, loff_t pos, unsigned long long count);

#endif /* __LIBXFS_XFILE_H__ */
