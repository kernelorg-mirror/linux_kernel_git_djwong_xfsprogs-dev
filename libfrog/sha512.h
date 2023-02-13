/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2023 Oracle, Inc.
 * All Rights Reserved.
 */
#ifndef __LIBFROG_SHA512_H__
#define __LIBFROG_SHA512_H__

struct sha512_state {
	__u64		length;
	__u64		state[8];
	unsigned long	curlen;
	unsigned char	buf[128];
};

#define SHA512_DESC_ON_STACK(mp, name) \
	struct sha512_state name

#define SHA512_DIGEST_SIZE	64

void sha512(const unsigned char *in, unsigned long in_size, unsigned char *out);

int sha512_init(struct sha512_state *md);
int sha512_done(struct sha512_state *md, unsigned char *out);
int sha512_process(struct sha512_state *md, const unsigned char *in,
		unsigned long inlen);

static inline void sha512_erase(struct sha512_state *md)
{
	memset(md, 0, sizeof(*md));
}

#endif	/* __LIBFROG_SHA512_H__ */
