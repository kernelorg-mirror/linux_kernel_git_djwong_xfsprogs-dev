// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include <string.h>
#include "xfs.h"
#include "libfrog/fsgeom.h"
#include "libfrog/fsproperties.h"
#include "list.h"

/* Return the offset of a string in a string table, or -1 if not found. */
static inline int
__fsprops_lookup(
	const char	*values[],
	unsigned int	nr_values,
	const char	*value)
{
	unsigned int	i;

	for (i = 0; i < nr_values; i++) {
		if (values[i] && !strcmp(value, values[i]))
			return i;
	}

	return -1;
}

#define fsprops_lookup(values, value) \
	__fsprops_lookup((values), ARRAY_SIZE(values), (value))

/* Self-healing fs property */

static const char *fsprop_self_healing_values[] = {
	[FSPROP_SELFHEAL_UNSET]		= NULL,
	[FSPROP_SELFHEAL_NONE]		= "none",
	[FSPROP_SELFHEAL_CHECK]		= "check",
	[FSPROP_SELFHEAL_OPTIMIZE]	= "optimize",
	[FSPROP_SELFHEAL_REPAIR]	= "repair",
};

/* Convert the self_healing property enum to a string. */
const char *
fsprop_write_self_healing(
	enum fsprop_self_healing	x)
{
	if (x <= FSPROP_SELFHEAL_UNSET ||
	    x >= ARRAY_SIZE(fsprop_self_healing_values))
		return NULL;
	return fsprop_self_healing_values[x];
}

/*
 * Turn a self_healing value string into an enumerated value, or _UNSET if it's
 * not recognized.
 */
enum fsprop_self_healing
fsprop_read_self_healing(
	const char	*value)
{
	int ret = fsprops_lookup(fsprop_self_healing_values, value);
	if (ret < 0)
		return FSPROP_SELFHEAL_UNSET;
	return ret;
}

/* Return true if a fs property name=value tuple is allowed. */
bool
fsprop_validate(
	const char	*name,
	const char	*value)
{
	if (!strcmp(name, FSPROP_SELF_HEALING_NAME))
		return fsprops_lookup(fsprop_self_healing_values, value) >= 0;

	return true;
}
