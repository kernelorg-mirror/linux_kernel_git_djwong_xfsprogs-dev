// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2018-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef XFS_SCRUB_READ_VERIFY_H_
#define XFS_SCRUB_READ_VERIFY_H_

struct scrub_ctx;
struct read_verify_pool;
struct disk;

struct read_verify_schedule {
	struct read_verify_pool	*rvp;
	void			*io_end_arg;
	uint64_t		io_start;	/* bytes */
	uint64_t		io_length;	/* bytes */
};

/* Function called when an IO error happens. */
typedef void (*read_verify_ioerr_fn_t)(struct scrub_ctx *ctx,
		struct disk *disk, uint64_t start, uint64_t length,
		int error, void *arg);

int read_verify_pool_alloc(struct scrub_ctx *ctx, struct disk *disk,
		size_t miniosz, read_verify_ioerr_fn_t ioerr_fn,
		struct read_verify_pool **prvp);
void read_verify_pool_abort(struct read_verify_pool *rvp);
int read_verify_pool_flush(struct read_verify_pool *rvp);
void read_verify_pool_destroy(struct read_verify_pool *rvp);

int read_verify_schedule_now(struct read_verify_schedule *rs);
bool try_read_verify_schedule_io(struct read_verify_schedule *rs,
		struct read_verify_pool *rvp, uint64_t start, uint64_t length,
		void *end_arg);

int read_verify_bytes(struct read_verify_pool *rvp, uint64_t *bytes);

#endif /* XFS_SCRUB_READ_VERIFY_H_ */
