// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"

#include "platform_defs.h"
#include "libfrog/flagmap.h"

/*
 * Given a mapping of bits to strings and a bitmask, format the bitmask as a
 * list of strings and hexadecimal number representing bits not mapped to any
 * string.  The output will be truncated if buf is not large enough.
 */
void
mask_to_string(
	const struct flag_map	*map,
	unsigned long long	mask,
	const char		*delimiter,
	char			*buf,
	size_t			bufsize)
{
	const char		*tag = "";
	unsigned long long	seen = 0;
	int			w;

	for (; map->string; map++) {
		seen |= map->flag;

		if (mask & map->flag) {
			w = snprintf(buf, bufsize, "%s%s", tag, _(map->string));
			if (w > bufsize)
				return;

			buf += w;
			bufsize -= w;

			tag = delimiter;
		}
	}

	if (mask & ~seen)
		snprintf(buf, bufsize, "%s0x%llx", tag, mask & ~seen);
}

/*
 * Given a mapping of values to strings and a value, return the matching string
 * or confusion.
 */
const char *
value_to_string(
	const struct flag_map	*map,
	unsigned long long	value)
{
	for (; map->string; map++) {
		if (value == map->flag)
			return _(map->string);
	}

	return _("unknown value");
}
