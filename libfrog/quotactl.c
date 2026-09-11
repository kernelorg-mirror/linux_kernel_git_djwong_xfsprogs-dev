// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "libfrog/quotactl.h"
#include <sys/quota.h>

#ifndef PRJQUOTA
#define PRJQUOTA 2
#endif

static int
xtype_to_qtype(
	uint		type)
{
	switch (type) {
	case XFS_USER_QUOTA:
		return USRQUOTA;
	case XFS_GROUP_QUOTA:
		return GRPQUOTA;
	case XFS_PROJ_QUOTA:
		return PRJQUOTA;
	}
	return 0;
}

static int
xcommand_to_qcommand(
	enum xfs_quota_cmd	xcommand)
{
	switch (xcommand) {
	case XFS_QUOTAON:
		return Q_XQUOTAON;
	case XFS_QUOTAOFF:
		return Q_XQUOTAOFF;
	case XFS_GETQUOTA:
		return Q_XGETQUOTA;
	case XFS_GETNEXTQUOTA:
		return Q_XGETNEXTQUOTA;
	case XFS_SETQLIM:
		return Q_XSETQLIM;
	case XFS_GETQSTAT:
		return Q_XGETQSTAT;
	case XFS_GETQSTATV:
		return Q_XGETQSTATV;
	case XFS_QUOTARM:
		return Q_XQUOTARM;
	case XFS_QSYNC:
		return Q_XQUOTASYNC;
	}
	return 0;
}

int
xfsquotactl(
	enum xfs_quota_cmd	xcommand,
	const char		*device,
	uint			xtype,
	uint			id,
	void			*addr)
{
	const int		op = QCMD(xcommand_to_qcommand(xcommand),
					  xtype_to_qtype(xtype));

	return quotactl(op, device, id, addr);
}
