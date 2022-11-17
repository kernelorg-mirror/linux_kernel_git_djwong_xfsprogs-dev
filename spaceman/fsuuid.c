// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022 Oracle.
 * All Rights Reserved.
 */

#include "libxfs.h"
#include "libfrog/fsgeom.h"
#include "libfrog/paths.h"
#include "command.h"
#include "init.h"
#include "space.h"
#include <sys/ioctl.h>

#ifndef FS_IOC_GETFSUUID
#define FS_IOC_GETFSUUID	_IOR('f', 44, struct fsuuid)
#define UUID_SIZE 16
struct fsuuid {
    __u32   fsu_len;
    __u32   fsu_flags;
    __u8    fsu_uuid[];
};
#endif

static cmdinfo_t fsuuid_cmd;

static int
fsuuid_f(
	int		argc,
	char		**argv)
{
	struct fsuuid	*fsuuid;
	char		buf[40];
	int		error;

	fsuuid = calloc(1, sizeof(struct fsuuid) + UUID_SIZE);
	if (!fsuuid) {
		perror("malloc");
		exitcode = 1;
		return 0;
	}

	fsuuid->fsu_len = UUID_SIZE;
	fsuuid->fsu_flags = 0;

	error = ioctl(file->xfd.fd, FS_IOC_GETFSUUID, fsuuid);

	if (error) {
		perror("fsuuid");
		exitcode = 1;
	} else {
		platform_uuid_unparse((uuid_t *)fsuuid->fsu_uuid, buf);
		printf("UUID = %s\n", buf);
	}

	free(fsuuid);
	return 0;
}

void
fsuuid_init(void)
{
	fsuuid_cmd.name = "fsuuid";
	fsuuid_cmd.cfunc = fsuuid_f;
	fsuuid_cmd.argmin = 0;
	fsuuid_cmd.argmax = 0;
	fsuuid_cmd.flags = CMD_FLAG_ONESHOT;
	fsuuid_cmd.oneline = _("get mounted filesystem UUID");

	add_command(&fsuuid_cmd);
}
