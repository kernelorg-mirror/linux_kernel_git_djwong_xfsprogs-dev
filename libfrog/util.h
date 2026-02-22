// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#ifndef __LIBFROG_UTIL_H__
#define __LIBFROG_UTIL_H__

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>

unsigned int	log2_roundup(unsigned int i);
unsigned int	log2_rounddown(unsigned long long i);

#define min_t(type,x,y) \
	({ type __x = (x); type __y = (y); __x < __y ? __x: __y; })
#define max_t(type,x,y) \
	({ type __x = (x); type __y = (y); __x > __y ? __x: __y; })

void *memchr_inv(const void *start, int c, size_t bytes);

struct timespec64;

bool current_fixed_time(struct timespec64 *tv);
bool get_deterministic_seed(uint32_t *result);

#ifdef HAVE_GETRANDOM_NONBLOCK
uint32_t get_random_u32(void);
#else
#define get_random_u32()	(0)
#endif

extern void cmn_err(int, char *, ...);
enum ce { CE_DEBUG, CE_CONT, CE_NOTE, CE_WARN, CE_ALERT, CE_PANIC };

typedef unsigned char u8;
unsigned int hweight8(unsigned int w);
unsigned int hweight32(unsigned int w);
unsigned int hweight64(uint64_t w);

#endif /* __LIBFROG_UTIL_H__ */
