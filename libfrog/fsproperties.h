/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __LIBFROG_FSPROPERTIES_H__
#define __LIBFROG_FSPROPERTIES_H__

/* Name space for filesystem properties. */
#define XFS_FSPROPS_NS		"trusted"

/*
 * All filesystem property xattr names must have this string after the
 * namespace.  For example, the VFS xattr calls should use the name
 * "trusted.xfs:fubar".  The xfs xattr ioctls would set ATTR_ROOT and use the
 * name "xfs:fubar".
 */
#define XFS_FSPROPS_PREFIX	"xfs:"

static inline int
fsprop_name_to_attr_name(
	const char		*prop_name,
	char			**attr_name)
{
	return asprintf(attr_name, XFS_FSPROPS_PREFIX "%s", prop_name);
}

static inline const char *
attr_name_to_fsprop_name(
	const char		*attr_name)
{
	const size_t		bytes = sizeof(XFS_FSPROPS_PREFIX) - 1;
	unsigned int		i;

	for (i = 0; i < bytes; i++) {
		if (attr_name[i] == 0)
			return NULL;
	}

	if (memcmp(attr_name, XFS_FSPROPS_PREFIX, bytes) != 0)
		return NULL;

	return attr_name + bytes;
}

bool fsprop_validate(const char *name, const char *value);

/* Specific Filesystem Properties */

#define FSPROP_SELF_HEALING_NAME	"self_healing"

enum fsprop_self_healing {
	FSPROP_SELFHEAL_UNSET = 0,	/* do not set property */
	FSPROP_SELFHEAL_NONE,		/* no background scrubs */
	FSPROP_SELFHEAL_CHECK,		/* allow only background checking */
	FSPROP_SELFHEAL_OPTIMIZE,	/* allow background optimization */
	FSPROP_SELFHEAL_REPAIR,		/* allow background repair & optimization */
};

const char *fsprop_write_self_healing(enum fsprop_self_healing x);
enum fsprop_self_healing fsprop_read_self_healing(const char *value);

#endif /* __LIBFROG_FSPROPERTIES_H__ */
