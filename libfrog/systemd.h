/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2026 Oracle.  All rights reserved.
 * All Rights Reserved.
 */
#ifndef __LIBFROG_SYSTEMD_H__
#define __LIBFROG_SYSTEMD_H__

int systemd_path_instance_unit_name(const char *unit_template,
		const char *path, char *unitname, size_t unitnamelen);

enum systemd_unit_manage {
	UM_STOP,
	UM_START,
	UM_RESTART,
};

int systemd_manage_unit(enum systemd_unit_manage how, const char *unitname);

#endif /* __LIBFROG_SYSTEMD_H__ */
