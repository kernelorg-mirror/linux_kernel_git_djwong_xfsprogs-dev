/*
 * Copyright (c) 2018 Luis R. Rodriguez <mcgrof@kernel.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write the Free Software Foundation,
 * Inc.,  51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>

#include "libxfs.h"
#include "config.h"

/* Map config file options to the relevant parts of dft_features. */
struct cfg_section_map cfgfile_map[] = {
	{
		.name = "data",
		.subopts = {
			[CFG_D_NOALIGN] = {
				.suboptname	= "noalign",
				.ptr		= &dft_features.nodalign,
				.type		= FV_BOOL,
			},
			{NULL}
		},
	},
	{
		.name = "inode",
		.subopts = {
			[CFG_I_ALIGN] = {
				.suboptname	= "align",
				.ptr		= &dft_features.inode_align,
				.type		= FV_BOOL,
			},
			[CFG_I_PROJID32BIT] = {
				.suboptname	= "projid32bit",
				.ptr		= &dft_features.projid32bit,
				.type		= FV_BOOL,
			},
			[CFG_I_SPINODES] = {
				.suboptname	= "sparse",
				.ptr		= &dft_features.spinodes,
				.type		= FV_BOOL,
			},
			{NULL}
		},
	},
	{
		.name = "log",
		.subopts = {
			[CFG_L_LAZYSBCNTR] = {
				.suboptname	= "lazy-count",
				.ptr		= &dft_features.lazy_sb_counters,
				.type		= FV_BOOL,
			},
			{NULL}
		},
	},
	{
		.name = "metadata",
		.subopts = {
			[CFG_M_CRC] = {
				.suboptname	= "crc",
				.ptr		= &dft_features.crcs_enabled,
				.type		= FV_BOOL,
			},
			[CFG_M_FINOBT] = {
				.suboptname	= "finobt",
				.ptr		= &dft_features.finobt,
				.type		= FV_BOOL,
			},
			[CFG_M_RMAPBT] = {
				.suboptname	= "rmapbt",
				.ptr		= &dft_features.rmapbt,
				.type		= FV_BOOL,
			},
			[CFG_M_REFLINK] = {
				.suboptname	= "reflink",
				.ptr		= &dft_features.reflink,
				.type		= FV_BOOL,
			},
			{NULL}
		},
	},
	{
		.name = "naming",
		.subopts = {
			[CFG_N_FTYPE] = {
				.suboptname	= "ftype",
				.ptr		= &dft_features.dirftype,
				.type		= FV_BOOL,
			},
			{NULL}
		},
	},
	{
		.name = "rtdev",
		.subopts = {
			[CFG_R_NOALIGN] = {
				.suboptname	= "noalign",
				.ptr		= &dft_features.nortalign,
				.type		= FV_BOOL,
			},
			{NULL}
		},
	},
	{NULL},
};

/* Trim leading and trailing whitespace and comments. */
static void
trim_config_line(
	char			*line)
{
	char			*start, *end;

	/* Roll past leading whitespace. */
	for (start = line; *start && isspace(*start); start++) { /* empty */; }

	/* Run until we hit EOL or a comment. */
	for (end = start; *end && *end != '#'; end++) { /* empty */; }

	/* Trim any trailing whitespace. */
	for (; end > start && isspace(*(end - 1)); end--) { /* empty */; }

	/* Fix up the line. */
	memmove(line, start, end - start);
	line[end - start] = 0;
}

/* What did we do with this line? */
enum line_control {
	LINE_IGNORED,
	LINE_HANDLED,
	LINE_ERROR,
};

/*
 * Look for a section tag and return it if we haven't seen this section
 * already.
 */
static enum line_control
parse_section(
	const char			*config_file,
	unsigned int			line_number,
	const char			*line,
	struct cfg_section_map		**section)
{
	char				*tag;
	char				*cp;
	char				*junk;
	struct cfg_section_map		*s;
	int				n;
	enum line_control		ret;

	/*
	 * Look for the section tag, the closing paren, and any junk after
	 * that.
	 */
	tag = cp = junk = NULL;
	n = sscanf(line, " [ %m[^] \f\n\r\t\v] %ms %m[^\n]", &tag, &cp, &junk);
	if (n != 2 || strcmp(cp, "]")) {
		ret = LINE_IGNORED;
		goto out;
	}

	/* Do we know about this section? */
	for (s = cfgfile_map; s->name; s++) {
		if (strcmp(s->name, tag))
			continue;
		if (s->seen) {
			fprintf(stderr,
_("%s:%d: section '%s' already seen.\n"),
					config_file, line_number, s->name);
			ret = LINE_ERROR;
			goto out;
		}

		ASSERT(s->subopts[0].suboptname != NULL);
		s->seen = true;
		*section = s;
		ret = LINE_HANDLED;
		goto out;
	}

	fprintf(stderr,
_("%s:%d: section '%s' not recognized.\n"),
			config_file, line_number, tag);
	ret = LINE_ERROR;
out:
	if (tag)
		free(tag);
	if (cp)
		free(cp);
	if (junk)
		free(junk);
	return ret;
}

/* Given a subopt, compute the appropriate offset in the mkfs params. */
static void *
section_to_dft(
	struct cfg_subopt_map		*subopt,
	struct mkfs_default_params	*dft)
{
	unsigned int			offset;

	offset = (char *)subopt->ptr - (char *)&dft_features;
	return (char *)&dft->sb_feat + offset;
}

/* Load a boolean into the parameters table. */
static bool
parse_subopt_bool(
	const char			*config_file,
	unsigned int			line_number,
	struct cfg_section_map		*section,
	struct cfg_subopt_map		*subopt,
	const char			*val,
	struct mkfs_default_params	*dft)
{
	bool				*v;
	unsigned long long		raw;
	char				*endp;

	errno = 0;
	raw = strtoull(val, &endp, 0);
	if (endp == val || *endp != 0) {
		fprintf(stderr,
_("%s:%d: could not interpret value '%s'.\n"),
			config_file, line_number, val);
		return false;
	}
	if (raw != 0 && raw != 1) {
		fprintf(stderr,
_("%s:%d: %s.%s value '%s' must be 0 or 1.\n"),
			config_file, line_number, section->name,
			subopt->suboptname, val);
		return false;
	}

	v = section_to_dft(subopt, dft);
	*v = raw == 1;
	return true;
}

/* Load a value into the defaults. */
static bool
parse_subopt_value(
	const char			*config_file,
	unsigned int			line_number,
	struct cfg_section_map		*section,
	struct cfg_subopt_map		*subopt,
	const char			*val,
	struct mkfs_default_params	*dft)
{
	switch (subopt->type) {
	case FV_BOOL:
		return parse_subopt_bool(config_file, line_number, section,
				subopt, val, dft);
	default:
		ASSERT(0);
		return false;
	}
}

/*
 * Look for a key and value and set them.
 */
static enum line_control
parse_key_value(
	const char			*config_file,
	unsigned int			line_number,
	const char			*line,
	struct cfg_section_map		*section,
	struct mkfs_default_params	*dft)
{
	char				*key;
	char				*eq;
	char				*val;
	struct cfg_subopt_map		*s;
	int				n;
	enum line_control		ret;

	/*
	 * Look for the key, the equals sign, and the value.  The value is
	 * anything that comes after the equals sign.
	 */
	key = eq = val = NULL;
	n = sscanf(line, " %m[^][ \f\n\r\t\v=] %m[=] %m[^\n]", &key, &eq, &val);
	if (n != 3 || strcmp(eq, "=")) {
		ret = LINE_IGNORED;
		goto out;
	}

	/* Must have a section. */
	if (section == NULL) {
		fprintf(stderr,
_("%s:%d: key '%s' is not in a section.\n"),
				config_file, line_number, key);
		ret = LINE_ERROR;
		goto out;
	}

	/* Do we know about this value? */
	for (s = section->subopts; s->suboptname; s++) {
		if (strcmp(s->suboptname, key))
			continue;
		if (s->seen) {
			fprintf(stderr,
_("%s:%d: section '%s' key '%s' already seen.\n"),
					config_file, line_number, section->name,
					s->suboptname);
			ret = LINE_ERROR;
			goto out;
		}
		if (parse_subopt_value(config_file, line_number, section, s,
				val, dft)) {
			s->seen = true;
			ret = LINE_HANDLED;
		} else {
			ret = LINE_ERROR;
		}
		goto out;
	}

	fprintf(stderr,
_("%s:%d: key '%s' is not a part of section '%s'.\n"),
			config_file, line_number, key, section->name);
	ret = LINE_ERROR;
out:
	if (key)
		free(key);
	if (eq)
		free(eq);
	if (val)
		free(val);
	return ret;
}

/* Deal with a single line of the config file. */
static bool
parse_config_line(
	struct mkfs_default_params	*dft,
	const char			*config_file,
	unsigned int			line_number,
	char				*line,
	struct cfg_section_map		**section)
{
	enum line_control		ret;

	/* Remove leading & trailing whitespace and comments. */
	trim_config_line(line);

	/* Ignore empty lines. */
	if (line[0] == 0)
		return true;

	/* Is this a section header? */
	ret = parse_section(config_file, line_number, line, section);
	switch (ret) {
	case LINE_HANDLED:
		return true;
	case LINE_ERROR:
		return false;
	case LINE_IGNORED:
		break;
	}

	/* Is this a value? */
	ret = parse_key_value(config_file, line_number, line, *section, dft);
	switch (ret) {
	case LINE_HANDLED:
		return true;
	case LINE_ERROR:
		return false;
	case LINE_IGNORED:
		break;
	}

	fprintf(stderr,
_("%s:%d: line not recognized as a section header or a key/value pair.\n"),
			config_file, line_number);
	return false;
}

/* Interpret every line of a config file. */
#define LINE_LEN	1025
static int
parse_config_stream(
	struct mkfs_default_params	*dft,
	const char			*config_file,
	FILE				*fp)
{
	char				line[LINE_LEN];
	struct cfg_section_map		*section = NULL;
	unsigned int			line_number = 1;
	bool				ret;

	while (fgets(line, LINE_LEN, fp)) {
		/* Lines should never hit the max length. */
		if (strlen(line) >= LINE_LEN - 1) {
			fprintf(stderr,
_("%s:%d: line too long.\n"),
					config_file, line_number);
			return -1;
		}
		ret = parse_config_line(dft, config_file, line_number, line,
				&section);
		if (!ret)
			return -1;
		line_number++;
	}
	return 0;
}

static int
config_stat_check(
	struct stat	*sp)
{
	if (!S_ISREG(sp->st_mode)) {
		errno = EINVAL;
		return -1;
	}

	/* Anything beyond 1 MiB is kind of silly right now */
	if (sp->st_size > 1 * 1024 * 1024) {
		errno = E2BIG;
		return -1;
	}

	return 0;
}

#ifndef O_PATH
#if defined __alpha__
#define O_PATH		040000000
#elif defined(__hppa__)
#define O_PATH		020000000
#elif defined(__sparc__)
#define O_PATH		0x1000000
#else
#define O_PATH		010000000
#endif
#endif /* O_PATH */

/*
 * Try to open a config file, either cli-specified, or default.
 *
 * If specified on commandline, search relative to pwd or absolute path.
 * If not specified or if above fails, try either cli-spec'd file or "default"
 * in MKFS_XFS_CONF_DIR.
 *
 * If any config file is successfully opened, dft->type is set to reflect the
 * source, an fd is returned, and the absolute path is returned in **fpath,
 * which must be free()'d by the caller.
 *
 * If a cli-specified file is not found -1 is returned and errno set. Otherwise
 * the file descriptor is returned.
 */
int
open_config_file(
	const char			*config_file,
	struct mkfs_default_params	*dft,
	char				**fpath)
{
	int				dirfd = -1, fd = -1, len, ret = 0;
	struct stat			st;
	bool				cli_specified = false;

	*fpath = malloc(PATH_MAX);
	if (!*fpath)
		return -1;

	memset(*fpath, 0, PATH_MAX);

	/* first try relative to pwd or absolute path to cli configfile */
	if (config_file) {
		cli_specified = true;
		if (strlen(config_file) > PATH_MAX) {
			errno = ENAMETOOLONG;
			goto out;
		}
		/* Get absolute path to this file */
		realpath(config_file, *fpath);
		fd = openat(AT_FDCWD, config_file, O_NOFOLLOW, O_RDONLY);
	}

	/* on failure search for cli config or default file in sysconfdir */
	if (fd < 0) {
		if (!cli_specified)
			config_file = MKFS_XFS_DEFAULT_CONFIG;
		len = snprintf(*fpath, PATH_MAX, "%s/%s", MKFS_XFS_CONF_DIR,
				config_file);
		/* Indicates truncation */
		if (len >= PATH_MAX) {
			errno = ENAMETOOLONG;
			goto out;
		}
		dirfd = open(MKFS_XFS_CONF_DIR, O_PATH|O_NOFOLLOW|O_DIRECTORY);
		if (dirfd < 0)
			goto out;
		fd = openat(dirfd, config_file, O_NOFOLLOW, O_RDONLY);
		if (fd < 0)
			goto out;
	}

	ret = fstat(fd, &st);
	if (ret != 0)
		goto out;
	ret = config_stat_check(&st);
	if (ret != 0)
		goto out;
	
out:
	/* stat check is always fatal; missing is fatal only if cli-specified */
	if (ret ||
	    (fd < 0 && cli_specified)) {
		fprintf(stderr, _("Unable to open config file: %s : %s\n"),
			*fpath, strerror(errno));
		free(*fpath);
		exit(1);
	}

	if (ret && fd >= 0)
		close(fd);
	if (dirfd >= 0)
		close(dirfd);
	return ret ? ret : fd;
}

/*
 * This is only called *iff* there is a configuration file which we know we
 * *must* parse.
 */
int
parse_defaults_file(
	int				fd,
	struct mkfs_default_params	*dft,
	const char			*config_file)
{
	FILE				*fp;
	int				ret;

	fp = fdopen(fd, "r");
	if (!fp)
		return -1;

	ret = parse_config_stream(dft, config_file, fp);
	if (ret) {
		errno = EINVAL;
		fclose(fp);
		return -1;
	}

	return 0;
}
