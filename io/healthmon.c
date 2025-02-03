// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2024-2026 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "libxfs.h"
#include "libfrog/fsgeom.h"
#include "libfrog/paths.h"
#include "libfrog/healthevent.h"
#include "command.h"
#include "init.h"
#include "io.h"

static void
healthmon_help(void)
{
	printf(_(
"Monitor filesystem health events"
"\n"
"-c             Replace the open file with the monitor file.\n"
"-d delay_ms    Sleep this many milliseconds between reads.\n"
"-p             Only probe for the existence of the ioctl.\n"
"-v             Request all events.\n"
"\n"));
}

static inline int
monitor_sleep(
	int			delay_ms)
{
	struct timespec		ts;

	if (!delay_ms)
		return 0;

	ts.tv_sec = delay_ms / 1000;
	ts.tv_nsec = (delay_ms % 1000) * 1000000;

	return nanosleep(&ts, NULL);
}

static int
monitor(
	size_t			bufsize,
	bool			consume,
	int			delay_ms,
	bool			verbose,
	bool			only_probe)
{
	struct xfs_health_monitor	hmo = {
		.format		= XFS_HEALTH_MONITOR_FMT_V0,
	};
	struct hme_prefix	pfx;
	void			*buf;
	ssize_t			bytes_read;
	int			mon_fd;
	int			ret = 1;

	hme_prefix_init(&pfx, file->name);

	if (verbose)
		hmo.flags |= XFS_HEALTH_MONITOR_ALL;

	mon_fd = ioctl(file->fd, XFS_IOC_HEALTH_MONITOR, &hmo);
	if (mon_fd < 0) {
		perror("XFS_IOC_HEALTH_MONITOR");
		return 1;
	}

	if (only_probe) {
		ret = 0;
		goto out_mon;
	}

	buf = malloc(bufsize);
	if (!buf) {
		perror("malloc");
		goto out_mon;
	}

	if (consume) {
		close(file->fd);
		file->fd = mon_fd;
	}

	monitor_sleep(delay_ms);
	while ((bytes_read = read(mon_fd, buf, bufsize)) > 0) {
		struct xfs_health_monitor_event *hme = buf;

		while (bytes_read >= sizeof(*hme)) {
			hme_report_event(&pfx, hme);
			hme++;
			bytes_read -= sizeof(*hme);
		}
		if (bytes_read > 0) {
			printf("healthmon: %zu bytes remain?\n", bytes_read);
			fflush(stdout);
		}

		monitor_sleep(delay_ms);
	}
	if (bytes_read < 0) {
		perror("healthmon");
		goto out_buf;
	}

	ret = 0;

out_buf:
	free(buf);
out_mon:
	close(mon_fd);
	return ret;
}

static int
healthmon_f(
	int			argc,
	char			**argv)
{
	size_t			bufsize = 4096;
	bool			consume = false;
	bool			verbose = false;
	bool			only_probe = false;
	int			delay_ms = 0;
	int			c;

	while ((c = getopt(argc, argv, "b:cd:pv")) != EOF) {
		switch (c) {
		case 'b':
			errno = 0;
			c = atoi(optarg);
			if (c < 0 || errno) {
				printf("%s: bufsize must be positive\n",
						optarg);
				exitcode = 1;
				return 0;
			}
			bufsize = c;
			break;
		case 'c':
			consume = true;
			break;
		case 'd':
			errno = 0;
			delay_ms = atoi(optarg);
			if (delay_ms < 0 || errno) {
				printf("%s: delay must be positive msecs\n",
						optarg);
				exitcode = 1;
				return 0;
			}
			break;
		case 'p':
			only_probe = true;
			break;
		case 'v':
			verbose = true;
			break;
		default:
			exitcode = 1;
			healthmon_help();
			return 0;
		}
	}

	return monitor(bufsize, consume, delay_ms, verbose, only_probe);
}

static struct cmdinfo healthmon_cmd = {
	.name		= "healthmon",
	.cfunc		= healthmon_f,
	.argmin		= 0,
	.argmax		= -1,
	.flags		= CMD_FLAG_ONESHOT | CMD_NOMAP_OK,
	.args		= "[-c] [-d delay_ms] [-v]",
	.help		= healthmon_help,
};

void
healthmon_init(void)
{
	healthmon_cmd.oneline = _("monitor filesystem health events");

	add_command(&healthmon_cmd);
}
