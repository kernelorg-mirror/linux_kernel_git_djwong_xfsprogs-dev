// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"

#include <libfrog/statmount.h>

int
libfrog_listmount(
	uint64_t		mnt_id,
	int			mnt_ns_fd,
	uint64_t		*cursor,
	uint64_t		*mnt_ids,
	size_t			nr_mnt_ids)
{
	struct mnt_id_req	req = {
		.size		= sizeof(req),
		.mnt_id		= mnt_id,
#ifdef HAVE_LISTMOUNT_NS_FD
		.mnt_ns_fd	= mnt_ns_fd,
#else
		.spare		= mnt_ns_fd,
#endif
		.param		= *cursor,
	};
	int ret = syscall(SYS_listmount, &req, mnt_ids, nr_mnt_ids, 0);

	if (ret > 0)
		*cursor = mnt_ids[ret - 1];

	return ret;
}

int
libfrog_statmount(
	uint64_t		mnt_id,
	int			mnt_ns_fd,
	uint64_t		statmount_flags,
	struct statmount	*smbuf,
	size_t			smbuf_size)
{
	struct mnt_id_req	req = {
		.size		= sizeof(req),
		.mnt_id		= mnt_id,
#ifdef HAVE_LISTMOUNT_NS_FD
		.mnt_ns_fd	= mnt_ns_fd,
#else
		.spare		= mnt_ns_fd,
#endif
		.param		= statmount_flags,
	};

	return syscall(SYS_statmount, &req, smbuf, smbuf_size, 0);
}

int
libfrog_fstatmount(
	int			fd,
	uint64_t		statmount_flags,
	struct statmount	*smbuf,
	size_t			smbuf_size)
{
	struct mnt_id_req	req = {
		.size		= sizeof(req),
#ifdef HAVE_LISTMOUNT_NS_FD
		.mnt_ns_fd	= fd,
#else
		.spare		= fd,
#endif
		.param		= statmount_flags,
	};

	return syscall(SYS_statmount, &req, smbuf, smbuf_size, STATMOUNT_BY_FD);
}
