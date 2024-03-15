// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "libxfs.h"
#include "command.h"
#include "attrset.h"
#include "io.h"
#include "output.h"
#include "type.h"
#include "init.h"
#include "fprint.h"
#include "faddr.h"
#include "field.h"
#include "inode.h"
#include "malloc.h"
#include "write.h"
#include <sys/xattr.h>

static int		attr_set_f(int argc, char **argv);
static int		attr_remove_f(int argc, char **argv);
static int		attr_modify_f(int argc, char **argv);
static void		attrset_help(void);

static const cmdinfo_t	attr_set_cmd =
	{ "attr_set", "aset", attr_set_f, 1, -1, 0,
	  N_("[-r|-s|-u|-p] [-n] [-R|-C] [-v n] name"),
	  N_("set the named attribute on the current inode"), attrset_help };
static const cmdinfo_t	attr_remove_cmd =
	{ "attr_remove", "aremove", attr_remove_f, 1, -1, 0,
	  N_("[-r|-s|-u|-p] [-n] name"),
	  N_("remove the named attribute from the current inode"), attrset_help };
static const cmdinfo_t	attr_modify_cmd =
	{ "attr_modify", "amodify", attr_modify_f, 1, -1, 0,
	  N_("[-r|-s|-u] [-o n] [-v n] [-m n] name value"),
	  N_("modify value of the named attribute of the current inode"),
		attrset_help };

static void
attrset_help(void)
{
	dbprintf(_(
"\n"
" The 'attr_set' and 'attr_remove' commands provide interfaces for debugging\n"
" the extended attribute allocation and removal code.\n"
" Both commands together with 'attr_modify' require an attribute name to be\n"
" specified. The attr_set and attr_modify commands allow an optional value\n"
" length (-v) to be provided as well.\n"
" There are 4 namespace flags:\n"
"  -r -- 'root'\n"
"  -u -- 'user'		(default)\n"
"  -s -- 'secure'\n"
"  -p -- 'parent'\n"
"\n"
" For attr_set, these options further define the type of set operation:\n"
"  -C -- 'create'    - create attribute, fail if it already exists\n"
"  -R -- 'replace'   - replace attribute, fail if it does not exist\n"
" attr_modify command provides more of the following options:\n"
"  -m -- 'name length'   - specify length of the name (handy with binary names)\n"
"  -o -- 'value offset'   - offset new value within old attr's value\n"
" The backward compatibility mode 'noattr2' can be emulated (-n) also.\n"
"\n"));
}

void
attrset_init(void)
{
	if (!expert_mode)
		return;

	add_command(&attr_set_cmd);
	add_command(&attr_remove_cmd);
	add_command(&attr_modify_cmd);
}

static unsigned char *
get_buf_from_file(
	const char	*fname,
	size_t		bufsize,
	int		*namelen)
{
	FILE		*fp;
	unsigned char	*buf;
	size_t		sz;

	buf = malloc(bufsize + 1);
	if (!buf) {
		perror("malloc");
		return NULL;
	}

	fp = fopen(fname, "r");
	if (!fp) {
		perror(fname);
		goto out_free;
	}

	sz = fread(buf, sizeof(char), bufsize, fp);
	if (sz == 0) {
		printf("%s: Could not read anything from file\n", fname);
		goto out_fp;
	}

	fclose(fp);

	*namelen = sz;
	return buf;
out_fp:
	fclose(fp);
out_free:
	free(buf);
	return NULL;
}

#define LIBXFS_ATTR_NS		(LIBXFS_ATTR_SECURE | \
				 LIBXFS_ATTR_ROOT | \
				 LIBXFS_ATTR_PARENT)

static int
attr_set_f(
	int			argc,
	char			**argv)
{
	struct xfs_da_args	args = { };
	char			*sp;
	char			*name_from_file = NULL;
	char			*value_from_file = NULL;
	int			c;

	if (cur_typ == NULL) {
		dbprintf(_("no current type\n"));
		return 0;
	}
	if (cur_typ->typnm != TYP_INODE) {
		dbprintf(_("current type is not inode\n"));
		return 0;
	}

	while ((c = getopt(argc, argv, "ruspCRnN:v:V:")) != EOF) {
		switch (c) {
		/* namespaces */
		case 'r':
			args.attr_filter &= ~LIBXFS_ATTR_NS;
			args.attr_filter |= LIBXFS_ATTR_ROOT;
			break;
		case 'u':
			args.attr_filter &= ~LIBXFS_ATTR_NS;
			break;
		case 's':
			args.attr_filter &= ~LIBXFS_ATTR_NS;
			args.attr_filter |= LIBXFS_ATTR_SECURE;
			break;
		case 'p':
			args.attr_filter &= ~LIBXFS_ATTR_NS;
			args.attr_filter |= XFS_ATTR_PARENT;
			break;

		/* modifiers */
		case 'C':
			args.xattr_flags |= XATTR_CREATE;
			args.xattr_flags &= ~XATTR_REPLACE;
			break;
		case 'R':
			args.xattr_flags |= XATTR_REPLACE;
			args.xattr_flags &= ~XATTR_CREATE;
			break;

		case 'N':
			name_from_file = optarg;
			break;

		case 'n':
			/*
			 * We never touch attr2 these days; leave this here to
			 * avoid breaking scripts.
			 */
			break;

		/* value length */
		case 'v':
			if (value_from_file) {
				dbprintf(_("already set value file\n"));
				return 0;
			}

			args.valuelen = strtol(optarg, &sp, 0);
			if (*sp != '\0' ||
			    args.valuelen < 0 || args.valuelen > 64 * 1024) {
				dbprintf(_("bad attr_set valuelen %s\n"), optarg);
				return 0;
			}
			break;

		case 'V':
			if (args.valuelen != 0) {
				dbprintf(_("already set valuelen\n"));
				return 0;
			}

			value_from_file = optarg;
			break;

		default:
			dbprintf(_("bad option for attr_set command\n"));
			return 0;
		}
	}

	if (name_from_file) {
		int namelen;

		if (optind != argc) {
			dbprintf(_("too many options for attr_set (no name needed)\n"));
			return 0;
		}

		args.name = get_buf_from_file(name_from_file, MAXNAMELEN,
				&namelen);
		if (!args.name)
			return 0;

		args.namelen = namelen;
	} else {
		if (optind != argc - 1) {
			dbprintf(_("too few options for attr_set (no name given)\n"));
			return 0;
		}

		args.name = (const unsigned char *)argv[optind];
		if (!args.name) {
			dbprintf(_("invalid name\n"));
			return 0;
		}

		args.namelen = strlen(argv[optind]);
		if (args.namelen >= MAXNAMELEN) {
			dbprintf(_("name too long\n"));
			goto out;
		}
	}

	if (value_from_file) {
		int valuelen;

		args.value = get_buf_from_file(value_from_file,
				XFS_XATTR_SIZE_MAX, &valuelen);
		if (!args.value)
			goto out;

		args.valuelen = valuelen;
	} else if (args.valuelen) {
		args.value = memalign(getpagesize(), args.valuelen);
		if (!args.value) {
			dbprintf(_("cannot allocate buffer (%d)\n"),
				args.valuelen);
			goto out;
		}
		memset(args.value, 'v', args.valuelen);
	}

	if (libxfs_iget(mp, NULL, iocur_top->ino, 0, &args.dp)) {
		dbprintf(_("failed to iget inode %llu\n"),
			(unsigned long long)iocur_top->ino);
		goto out;
	}

	if (libxfs_attr_set(&args)) {
		dbprintf(_("failed to set attr %s on inode %llu\n"),
			args.name, (unsigned long long)iocur_top->ino);
		goto out;
	}

	/* refresh with updated inode contents */
	set_cur_inode(iocur_top->ino);

out:
	if (args.dp)
		libxfs_irele(args.dp);
	if (args.value)
		free(args.value);
	if (name_from_file)
		free((void *)args.name);
	return 0;
}

static int
attr_remove_f(
	int			argc,
	char			**argv)
{
	struct xfs_da_args	args = { };
	char			*name_from_file = NULL;
	int			c;

	if (cur_typ == NULL) {
		dbprintf(_("no current type\n"));
		return 0;
	}
	if (cur_typ->typnm != TYP_INODE) {
		dbprintf(_("current type is not inode\n"));
		return 0;
	}

	while ((c = getopt(argc, argv, "ruspnN:")) != EOF) {
		switch (c) {
		/* namespaces */
		case 'r':
			args.attr_filter &= ~LIBXFS_ATTR_NS;
			args.attr_filter |= LIBXFS_ATTR_ROOT;
			break;
		case 'u':
			args.attr_filter &= ~LIBXFS_ATTR_NS;
			break;
		case 's':
			args.attr_filter &= ~LIBXFS_ATTR_NS;
			args.attr_filter |= LIBXFS_ATTR_SECURE;
			break;
		case 'p':
			args.attr_filter &= ~LIBXFS_ATTR_NS;
			args.attr_filter |= XFS_ATTR_PARENT;
			break;

		case 'N':
			name_from_file = optarg;
			break;

		case 'n':
			/*
			 * We never touch attr2 these days; leave this here to
			 * avoid breaking scripts.
			 */
			break;

		default:
			dbprintf(_("bad option for attr_remove command\n"));
			return 0;
		}
	}

	if (name_from_file) {
		int namelen;

		if (optind != argc) {
			dbprintf(_("too many options for attr_set (no name needed)\n"));
			return 0;
		}

		args.name = get_buf_from_file(name_from_file, MAXNAMELEN,
				&namelen);
		if (!args.name)
			return 0;

		args.namelen = namelen;
	} else {
		if (optind != argc - 1) {
			dbprintf(_("too few options for attr_remove (no name given)\n"));
			return 0;
		}

		args.name = (const unsigned char *)argv[optind];
		if (!args.name) {
			dbprintf(_("invalid name\n"));
			return 0;
		}

		args.namelen = strlen(argv[optind]);
		if (args.namelen >= MAXNAMELEN) {
			dbprintf(_("name too long\n"));
			return 0;
		}
	}

	if (libxfs_iget(mp, NULL, iocur_top->ino, 0, &args.dp)) {
		dbprintf(_("failed to iget inode %llu\n"),
			(unsigned long long)iocur_top->ino);
		goto out;
	}

	if (libxfs_attr_set(&args)) {
		dbprintf(_("failed to remove attr %s from inode %llu\n"),
			(unsigned char *)args.name,
			(unsigned long long)iocur_top->ino);
		goto out;
	}

	/* refresh with updated inode contents */
	set_cur_inode(iocur_top->ino);

out:
	if (args.dp)
		libxfs_irele(args.dp);
	if (name_from_file)
		free((void *)args.name);
	return 0;
}

static int
attr_modify_f(
	int			argc,
	char			**argv)
{
	struct xfs_da_args	args = {
		.xattr_flags	= XATTR_REPLACE,
	};
	int			c;
	int			offset = 0;
	char			*sp;
	char			*converted;
	uint8_t			*name;
	int			namelen = 0;
	uint8_t			*value;
	int			valuelen = 0;

	if (cur_typ == NULL) {
		dbprintf(_("no current type\n"));
		return 0;
	}

	if (cur_typ->typnm != TYP_INODE) {
		dbprintf(_("current type is not inode\n"));
		return 0;
	}

	while ((c = getopt(argc, argv, "ruspnv:o:m:")) != EOF) {
		switch (c) {
		/* namespaces */
		case 'r':
			args.attr_filter &= ~LIBXFS_ATTR_NS;
			args.attr_filter |= LIBXFS_ATTR_ROOT;
			break;
		case 'u':
			args.attr_filter &= ~LIBXFS_ATTR_NS;
			break;
		case 's':
			args.attr_filter &= ~LIBXFS_ATTR_NS;
			args.attr_filter |= LIBXFS_ATTR_SECURE;
			break;
		case 'p':
			args.attr_filter &= ~LIBXFS_ATTR_NS;
			args.attr_filter |= XFS_ATTR_PARENT;
			break;

		case 'n':
			/*
			 * We never touch attr2 these days; leave this here to
			 * avoid breaking scripts.
			 */
			break;

		case 'o':
			offset = strtol(optarg, &sp, 0);
			if (*sp != '\0' || offset < 0 || offset > 64 * 1024) {
				dbprintf(_("bad attr_modify offset %s\n"),
						optarg);
				return 0;
			}
			break;

		case 'v':
			valuelen = strtol(optarg, &sp, 0);
			if (*sp != '\0' || offset < 0 || valuelen > 64 * 1024) {
				dbprintf(_("bad attr_modify value len %s\n"),
						optarg);
				return 0;
			}
			break;

		case 'm':
			namelen = strtol(optarg, &sp, 0);
			if (*sp != '\0' || offset < 0 || namelen > MAXNAMELEN) {
				dbprintf(_("bad attr_modify name len %s\n"),
						optarg);
				return 0;
			}
			break;

		default:
			dbprintf(_("bad option for attr_modify command\n"));
			return 0;
		}
	}

	if (optind != argc - 2) {
		dbprintf(_("too few options for attr_modify\n"));
		return 0;
	}

	if (namelen >= MAXNAMELEN) {
		dbprintf(_("name too long\n"));
		return 0;
	}

	if (!namelen) {
		if (argv[optind][0] == '#')
			namelen = strlen(argv[optind])/2;
		if (argv[optind][0] == '"')
			namelen = strlen(argv[optind]) - 2;
	}

	name = xcalloc(namelen, sizeof(uint8_t));
	converted = convert_arg(argv[optind], (int)(namelen*8));
	if (!converted) {
		dbprintf(_("invalid name\n"));
		goto out_free_name;
	}

	memcpy(name, converted, namelen);
	args.name = (const uint8_t *)name;
	args.namelen = namelen;

	optind++;

	if (valuelen > 64 * 1024) {
		dbprintf(_("value too long\n"));
		goto out_free_name;
	}

	if (!valuelen) {
		if (argv[optind][0] == '#')
			valuelen = strlen(argv[optind])/2;
		if (argv[optind][0] == '"')
			valuelen = strlen(argv[optind]) - 2;
	}

	if ((valuelen + offset) > 64 * 1024) {
		dbprintf(_("offsetted value too long\n"));
		goto out_free_name;
	}

	value = xcalloc(valuelen, sizeof(uint8_t));
	converted = convert_arg(argv[optind], (int)(valuelen*8));
	if (!converted) {
		dbprintf(_("invalid value\n"));
		goto out_free_value;
	}
	memcpy(value, converted, valuelen);

	if (libxfs_iget(mp, NULL, iocur_top->ino, 0, &args.dp)) {
		dbprintf(_("failed to iget inode %llu\n"),
			(unsigned long long)iocur_top->ino);
		goto out;
	}

	if (libxfs_attr_get(&args)) {
		dbprintf(_("failed to get attr '%s' from inode %llu\n"),
			args.name, (unsigned long long)iocur_top->ino);
		goto out;
	}

	if (valuelen + offset > args.valuelen) {
		dbprintf(_("new value too long\n"));
		goto out;
	}

	/* As args.valuelen is now set let's get args.value */
	if (libxfs_attr_get(&args)) {
		dbprintf(_("failed to get attr '%s' from inode %llu\n"),
			args.name, (unsigned long long)iocur_top->ino);
		goto out;
	}

	/* modify value */
	memcpy((uint8_t *)args.value + offset, value, valuelen);

	if (libxfs_attr_set(&args)) {
		dbprintf(_("failed to set attr '%s' from inode %llu\n"),
			(unsigned char *)args.name,
			(unsigned long long)iocur_top->ino);
		goto out;
	}

	/* refresh with updated inode contents */
	set_cur_inode(iocur_top->ino);

out:
	if (args.dp)
		libxfs_irele(args.dp);
	xfree(args.value);
out_free_value:
	xfree(value);
out_free_name:
	xfree(name);
	return 0;
}
