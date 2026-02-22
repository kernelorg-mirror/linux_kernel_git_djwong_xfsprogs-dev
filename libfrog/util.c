// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#include "platform_defs.h"
#include "util.h"

#ifdef HAVE_GETRANDOM_NONBLOCK
#include <sys/random.h>
#endif

extern char *progname;

/*
 * libfrog is a collection of miscellaneous userspace utilities.
 * It's a library of Funny Random Oddball Gunk <cough>.
 */

unsigned int
log2_roundup(unsigned int i)
{
	unsigned int	rval;

	for (rval = 0; rval < NBBY * sizeof(i); rval++) {
		if ((1 << rval) >= i)
			break;
	}
	return rval;
}

void *
memchr_inv(const void *start, int c, size_t bytes)
{
	const unsigned char	*p = start;

	while (bytes > 0) {
		if (*p != (unsigned char)c)
			return (void *)p;
		bytes--;
	}

	return NULL;
}

unsigned int
log2_rounddown(unsigned long long i)
{
	int	rval;

	for (rval = NBBY * sizeof(i) - 1; rval >= 0; rval--) {
		if ((1ULL << rval) < i)
			break;
	}
	return rval;
}

/*
 * current_fixed_time() tries to detect if SOURCE_DATE_EPOCH is in our
 * environment, and set input timespec's timestamp to that value.
 *
 * Returns true on success, fail otherwise.
 */
bool
current_fixed_time(
	struct timespec64	*tv)
{
	/*
	 * To avoid many getenv() we'll use an initialization static flag, so
	 * we only read once.
	 */
	static bool		enabled = false;
	static bool		read_env = false;
	static time64_t		epoch;
	char			*endp;
	char			*source_date_epoch;

	if (!read_env) {
		read_env = true;
		source_date_epoch = getenv("SOURCE_DATE_EPOCH");
		if (source_date_epoch && source_date_epoch[0] != '\0') {
			errno = 0;
			epoch = strtoll(source_date_epoch, &endp, 10);
			if (errno != 0 || *endp != '\0') {
				fprintf(stderr,
 _("%s: SOURCE_DATE_EPOCH '%s' invalid timestamp, ignoring.\n"),
						progname, source_date_epoch);

				return false;
			}

			enabled = true;
		}
	}

	/*
	 * This will happen only if we successfully read a valid
	 * SOURCE_DATE_EPOCH and properly initiated the epoch value.
	 */
	if (read_env && enabled) {
		tv->tv_sec = epoch;
		tv->tv_nsec = 0;
		return true;
	}

	/*
	 * We initialized but had no valid SOURCE_DATE_EPOCH so we fall back
	 * to regular behaviour.
	 */
	return false;
}

void
cmn_err(int level, char *fmt, ...)
{
	va_list	ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	fputs("\n", stderr);
	va_end(ap);
}

unsigned int
hweight8(unsigned int w)
{
	unsigned int res = w - ((w >> 1) & 0x55);
	res = (res & 0x33) + ((res >> 2) & 0x33);
	return (res + (res >> 4)) & 0x0F;
}

unsigned int
hweight32(unsigned int w)
{
	unsigned int res = w - ((w >> 1) & 0x55555555);
	res = (res & 0x33333333) + ((res >> 2) & 0x33333333);
	res = (res + (res >> 4)) & 0x0F0F0F0F;
	res = res + (res >> 8);
	return (res + (res >> 16)) & 0x000000FF;
}

unsigned int
hweight64(uint64_t w)
{
	return hweight32((unsigned int)w) +
	       hweight32((unsigned int)(w >> 32));
}

/*
 * get_deterministic_seed() tries to detect if DETERMINISTIC_SEED=1 is in our
 * environment, and set our result to 0x53454544 (SEED) instead of
 * extracting from getrandom().
 *
 * Returns true on success, fail otherwise.
 */
bool
get_deterministic_seed(
	uint32_t	*result)
{
	/*
	 * To avoid many getenv() we'll use an initialization static flag, so
	 * we only read once.
	 */
	static bool	enabled = false;
	static bool	read_env = false;
	static uint32_t	deterministic_seed = 0x53454544; /* SEED */
	char		*seed_env;

	if (!read_env) {
		read_env = true;
		seed_env = getenv("DETERMINISTIC_SEED");
		if (seed_env && strcmp(seed_env, "1") == 0)
			enabled = true;
	}

	/*
	 * This will happen only if we successfully read DETERMINISTIC_SEED=1.
	 */
	if (read_env && enabled) {
		*result = deterministic_seed;

		return true;
	}

	/*
	 * We initialized but had no DETERMINISTIC_SEED=1 in env so we fall
	 * back to regular behaviour.
	 */
	return false;
}

#ifdef HAVE_GETRANDOM_NONBLOCK
uint32_t
get_random_u32(void)
{
	uint32_t	ret;
	ssize_t		sz;

	/*
	 * Check for DETERMINISTIC_SEED in environment, it means we're
	 * creating a reproducible filesystem.
	 * If it fails, fall back to returning getrandom() like we used to do.
	 */
	if (get_deterministic_seed(&ret))
		return ret;
	/*
	 * Try to extract a u32 of randomness from /dev/urandom.  If that
	 * fails, fall back to returning zero like we used to do.
	 */
	sz = getrandom(&ret, sizeof(ret), GRND_NONBLOCK);
	if (sz != sizeof(ret))
		return 0;

	return ret;
}
#endif
