// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025-2026 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef LIBFROG_FLAGMAP_H_
#define LIBFROG_FLAGMAP_H_

struct flag_map {
	unsigned long long	flag;
	const char		*string;
};

void mask_to_string(const struct flag_map *map, unsigned long long mask,
		const char *delimiter, char *buf, size_t bufsize);

const char *lowest_set_mask_string(const struct flag_map *map,
		unsigned long long mask);

const char *value_to_string(const struct flag_map *map,
		unsigned long long value);

#endif /* LIBFROG_FLAGMAP_H_ */
