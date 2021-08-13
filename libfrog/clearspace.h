/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2021 Oracle, Inc.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __LIBFROG_CLEARSPACE_H__
#define __LIBFROG_CLEARSPACE_H__

struct clearspace_req;

struct clearspace_init {
	/* Open file and its pathname */
	struct xfs_fd		*xfd;
	const char		*fname;

	/* Which device do we want? */
	bool			is_realtime;
	dev_t			dev;

	/* Range of device to clear. */
	unsigned long long	start;
	unsigned long long	length;

	unsigned int		trace_mask;
};

int clearspace_init(struct clearspace_req **reqp,
		const struct clearspace_init *init);
int clearspace_free(struct clearspace_req **reqp);

int clearspace_run(struct clearspace_req *req);

int clearspace_efficacy(struct clearspace_req *req,
		unsigned long long *cleared_bytes);

/* Debugging levels */

#define CSP_TRACE_FREEZE	(0x01)
#define CSP_TRACE_GRAB		(0x02)
#define CSP_TRACE_FSMAP		(0x04)
#define CSP_TRACE_FSREFS	(0x08)
#define CSP_TRACE_BMAPX		(0x10)
#define CSP_TRACE_PREP		(0x20)
#define CSP_TRACE_TARGET	(0x40)
#define CSP_TRACE_DEDUPE	(0x80)
#define CSP_TRACE_FALLOC	(0x100)
#define CSP_TRACE_FIEXCHANGE	(0x200)
#define CSP_TRACE_XREBUILD	(0x400)
#define CSP_TRACE_EFFICACY	(0x800)
#define CSP_TRACE_SETUP		(0x1000)
#define CSP_TRACE_STATUS	(0x2000)
#define CSP_TRACE_DUMPFILE	(0x4000)
#define CSP_TRACE_BITMAP	(0x8000)

#define CSP_TRACE_ALL		(CSP_TRACE_FREEZE | \
				 CSP_TRACE_GRAB | \
				 CSP_TRACE_FSMAP | \
				 CSP_TRACE_FSREFS | \
				 CSP_TRACE_BMAPX | \
				 CSP_TRACE_PREP	 | \
				 CSP_TRACE_TARGET | \
				 CSP_TRACE_DEDUPE | \
				 CSP_TRACE_FALLOC | \
				 CSP_TRACE_FIEXCHANGE | \
				 CSP_TRACE_XREBUILD | \
				 CSP_TRACE_EFFICACY | \
				 CSP_TRACE_SETUP | \
				 CSP_TRACE_STATUS | \
				 CSP_TRACE_DUMPFILE | \
				 CSP_TRACE_BITMAP)

#endif /* __LIBFROG_CLEARSPACE_H__ */
