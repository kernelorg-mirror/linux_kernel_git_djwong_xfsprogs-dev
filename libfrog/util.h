// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#ifndef __LIBFROG_UTIL_H__
#define __LIBFROG_UTIL_H__

unsigned int	log2_roundup(unsigned int i);

static inline __attribute__((const))
int is_power_of_2(unsigned long n)
{
	return (n != 0 && ((n & (n - 1)) == 0));
}

#endif /* __LIBFROG_UTIL_H__ */
