// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "libfrog/systemd.h"

static void
close_fds(void)
{
	int max_fd = sysconf(_SC_OPEN_MAX);
	int fd;
#ifdef HAVE_CLOSE_RANGE
	int ret;
#endif

	if (max_fd < 1)
		max_fd = 1024;

#ifdef HAVE_CLOSE_RANGE
	ret = close_range(STDERR_FILENO + 1, max_fd, 0);
	if (!ret)
		return;
#endif

	for (fd = STDERR_FILENO + 1; fd < max_fd; fd++)
		close(fd);
}

/*
 * Compute the systemd instance unit name for a given path.
 *
 * The escaping logic is implemented directly in systemctl so there's no
 * library or dbus service that we can call.
 */
int
systemd_path_instance_unit_name(
	const char		*unit_template,
	const char		*path,
	char			*unitname,
	size_t			unitnamelen)
{
	FILE			*fp;
	char			*s;
	ssize_t			bytes;
	pid_t			child_pid;
	int			pipe_fds[2];
	int			ret;

	ret = pipe(pipe_fds);
	if (ret)
		return -1;

	child_pid = fork();
	if (child_pid < 0)
		return -1;

	if (!child_pid) {
		/* child process */
		char		*argv[] = {
			"systemd-escape",
			"--template",
			(char *)unit_template,
			"--path",
			(char *)path,
			NULL,
		};

		ret = dup2(pipe_fds[1], STDOUT_FILENO);
		if (ret < 0) {
			perror(path);
			goto fail;
		}

		close_fds();

		ret = execvp("systemd-escape", argv);
		if (ret)
			perror(path);

fail:
		exit(EXIT_FAILURE);
	}

	/* parent scrapes the output */
	fp = fdopen(pipe_fds[0], "r");
	s = fgets(unitname, unitnamelen, fp);
	fclose(fp);
	close(pipe_fds[1]);

	waitpid(child_pid, NULL, 0);

	if (!s) {
		errno = ENOENT;
		return -1;
	}

	/* trim off trailing newline */
	bytes = strlen(s);
	if (s[bytes - 1] == '\n')
		s[bytes - 1] = 0;

	return 0;
}

static const char *systemd_unit_manage_string(enum systemd_unit_manage how)
{
	switch (how) {
	case UM_STOP:
		return "stop";
	case UM_START:
		return "start";
	case UM_RESTART:
		return "restart";
	}

	/* shut up gcc */
	return NULL;
}

/*
 * Start/stop/restart a systemd unit and let it run in the background.
 *
 * systemctl start wraps a lot of logic around starting a unit, so it's less
 * work for xfsprogs to invoke systemctl instead of calling through dbus.
 */
int
systemd_manage_unit(
	enum systemd_unit_manage	how,
	const char			*unitname)
{
	pid_t				child_pid;
	int				child_status;
	int				ret;

	child_pid = fork();
	if (child_pid < 0)
		return -1;

	if (!child_pid) {
		/* child starts the process */
		char		*argv[] = {
			"systemctl",
			(char *)systemd_unit_manage_string(how),
			"--no-block",
			(char *)unitname,
			NULL,
		};

		close_fds();

		ret = execvp("systemctl", argv);
		if (ret)
			perror("systemctl");

		exit(EXIT_FAILURE);
	}

	/* parent waits for process */
	waitpid(child_pid, &child_status, 0);

	/* systemctl (stop/start/restart) --no-block should return quickly */
	if (WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0)
		return 0;

	errno = ENOMEM;
	return -1;
}
