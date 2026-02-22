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

static inline bool systemd_is_service(void)
{
	return getenv("SERVICE_MODE") != NULL;
}

/* Special processing for a service/daemon program that is exiting. */
static inline int
systemd_service_exit_now(int ret)
{
	/*
	 * If we're being run as a service, the return code must fit the LSB
	 * init script action error guidelines, which is to say that we
	 * compress all errors to 1 ("generic or unspecified error", LSB 5.0
	 * section 22.2) and hope the admin will scan the log for what actually
	 * happened.
	 */
	return ret != 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

/* Special processing for a service/daemon program that is exiting. */
static inline int
systemd_service_exit(int ret)
{
	/*
	 * We have to sleep 2 seconds here because journald uses the pid to
	 * connect our log messages to the systemd service.  This is critical
	 * for capturing all the log messages if the service fails, because
	 * failure analysis tools use the service name to gather log messages
	 * for reporting.
	 */
	sleep(2);

	return systemd_service_exit_now(ret);
}

#endif /* __LIBFROG_SYSTEMD_H__ */
