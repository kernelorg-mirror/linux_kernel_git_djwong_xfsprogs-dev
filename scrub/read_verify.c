// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2018-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include <stdint.h>
#include <stdlib.h>
#include <sys/statvfs.h>
#include "libfrog/workqueue.h"
#include "libfrog/paths.h"
#include "xfs_scrub.h"
#include "common.h"
#include "counter.h"
#include "disk.h"
#include "read_verify.h"
#include "progress.h"

/*
 * Read Verify Pool
 *
 * Manages the data block read verification phase.  The caller schedules
 * verification requests, which are then scheduled to be run by a thread
 * pool worker.  Adjacent (or nearly adjacent) requests can be combined
 * to reduce overhead when free space fragmentation is high.  The thread
 * pool takes care of issuing multiple IOs to the device, if possible.
 */

/*
 * Perform all IO in 32M chunks.  This cannot exceed 65536 sectors
 * because that's the biggest SCSI VERIFY(16) we dare to send.
 */
#define RVP_IO_MAX_SIZE		(33554432)

/*
 * If we're running in the background then we perform IO in 128k chunks
 * to reduce the load on the IO subsystem.
 */
#define RVP_BACKGROUND_IO_MAX_SIZE	(131072)

/* What's the real maximum IO size? */
static inline unsigned int
rvp_io_max_size(void)
{
	return bg_mode > 0 ? RVP_BACKGROUND_IO_MAX_SIZE : RVP_IO_MAX_SIZE;
}

/* Tolerate 64k holes in adjacent read verify requests. */
#define RVP_IO_BATCH_LOCALITY	(65536)

struct read_verify {
	uint64_t		io_start;	/* bytes */
	uint64_t		io_length;	/* bytes */
};

struct read_verify_pool {
	struct workqueue	wq;		/* thread pool */
	struct scrub_ctx	*ctx;		/* scrub context */
	void			*readbuf;	/* read buffer */
	struct ptcounter	*verified_bytes;
	void			*ioerr_arg;
	read_verify_ioerr_fn_t	ioerr_fn;	/* io error callback */
	size_t			miniosz;	/* minimum io size, bytes */
	enum xfs_device		dev;		/* which device? */

	/*
	 * Store a runtime error code here so that we can stop the pool and
	 * return it to the caller.
	 */
	int			runtime_error;
};

/*
 * Create a thread pool to run read verifiers.
 *
 * @ioerr_fn will be called when IO errors occur.
 */
int
read_verify_pool_alloc(
	struct scrub_ctx		*ctx,
	enum xfs_device			dev,
	read_verify_ioerr_fn_t		ioerr_fn,
	void				*ioerr_arg,
	struct read_verify_pool		**prvp)
{
	struct read_verify_pool		*rvp;
	unsigned int			verifier_threads =
		disk_heads(ctx->verify_disks[XFS_DEV_DATA]);
	int				ret;

	if (rvp_io_max_size() % ctx->mnt.fsgeom.blocksize)
		return EINVAL;

	rvp = calloc(1, sizeof(struct read_verify_pool));
	if (!rvp)
		return errno;

	ret = posix_memalign((void **)&rvp->readbuf, page_size,
			rvp_io_max_size());
	if (ret)
		goto out_free;
	ret = ptcounter_alloc(verifier_threads, &rvp->verified_bytes);
	if (ret)
		goto out_buf;
	rvp->miniosz = ctx->mnt.fsgeom.blocksize;
	rvp->ctx = ctx;
	rvp->dev = dev;
	rvp->ioerr_fn = ioerr_fn;
	rvp->ioerr_arg = ioerr_arg;
	ret = -workqueue_create(&rvp->wq, (struct xfs_mount *)rvp,
			verifier_threads == 1 ? 0 : verifier_threads);
	if (ret)
		goto out_counter;
	*prvp = rvp;
	return 0;

out_counter:
	ptcounter_free(rvp->verified_bytes);
out_buf:
	free(rvp->readbuf);
out_free:
	free(rvp);
	return ret;
}

/* Abort all verification work. */
void
read_verify_pool_abort(
	struct read_verify_pool		*rvp)
{
	if (!rvp->runtime_error)
		rvp->runtime_error = ECANCELED;
	workqueue_terminate(&rvp->wq);
}

/* Finish up any read verification work. */
int
read_verify_pool_flush(
	struct read_verify_pool		*rvp)
{
	return -workqueue_terminate(&rvp->wq);
}

/* Finish up any read verification work and tear it down. */
void
read_verify_pool_destroy(
	struct read_verify_pool		*rvp)
{
	workqueue_destroy(&rvp->wq);
	ptcounter_free(rvp->verified_bytes);
	free(rvp->readbuf);
	free(rvp);
}

/*
 * Issue a read-verify IO in big batches.
 */
static void
read_verify(
	struct workqueue		*wq,
	xfs_agnumber_t			agno,
	void				*arg)
{
	struct read_verify		*rv = arg;
	struct read_verify_pool		*rvp;
	unsigned long long		verified = 0;
	ssize_t				io_max_size;
	ssize_t				sz;
	ssize_t				len;
	int				read_error;
	int				ret;

	rvp = (struct read_verify_pool *)wq->wq_ctx;
	if (rvp->runtime_error)
		return;

	io_max_size = rvp_io_max_size();

	while (rv->io_length > 0) {
		read_error = 0;
		len = min(rv->io_length, io_max_size);
		dbg_printf("diskverify %u %"PRIu64" %zu\n", rvp->dev,
				rv->io_start, len);
		sz = disk_read_verify(rvp->ctx->verify_disks[rvp->dev],
				rvp->readbuf, rv->io_start, len);
		if (sz == len && io_max_size < rvp->miniosz) {
			/*
			 * If the verify request was 100% successful and less
			 * than a single block in length, we were trying to
			 * read to the end of a block after a short read.  That
			 * suggests there's something funny with this device,
			 * so single-step our way through the rest of the @rv
			 * range.
			 */
			io_max_size = rvp->miniosz;
		} else if (sz < 0) {
			read_error = errno;

			/* Runtime error, bail out... */
			if (read_error != EIO && read_error != EILSEQ) {
				rvp->runtime_error = read_error;
				return;
			}

			/*
			 * A direct read encountered an error while performing
			 * a multi-block read.  Reduce the transfer size to a
			 * single block so that we can identify the exact range
			 * of bad blocks and good blocks.  We single-step all
			 * the way to the end of the @rv range, (re)starting
			 * with the block that just failed.
			 */
			if (io_max_size > rvp->miniosz) {
				io_max_size = rvp->miniosz;
				continue;
			}

			/*
			 * A direct read hit an error while we were stepping
			 * through single blocks.  Mark everything bad from
			 * io_start to the next miniosz block.
			 */
			sz = rvp->miniosz - (rv->io_start % rvp->miniosz);
			dbg_printf("IOERR %u @ %"PRIu64" %zu err %d\n",
					rvp->dev, rv->io_start, sz, read_error);
			rvp->ioerr_fn(rvp->ctx, rvp->dev, rv->io_start, sz,
					read_error, rvp->ioerr_arg);
		} else if (sz == 0) {
			/* No bytes at all?  Did we hit the end of the disk? */
			dbg_printf("EOF %u @ %"PRIu64" %zu err %d\n",
					rvp->dev, rv->io_start, sz, read_error);
			rvp->ioerr_fn(rvp->ctx, rvp->dev, rv->io_start, sz,
					read_error, rvp->ioerr_arg);
			break;
		} else if (sz < len) {
			/*
			 * A short direct read suggests that we might have hit
			 * an IO error midway through the read but still had to
			 * return the number of bytes that were actually read.
			 *
			 * We need to force an EIO, so try reading the rest of
			 * the block (if it was a partial block read) or the
			 * next full block.
			 */
			io_max_size = rvp->miniosz - (sz % rvp->miniosz);
			dbg_printf("SHORT %u READ @ %"PRIu64" %zu try for %zd\n",
					rvp->dev, rv->io_start, sz,
					io_max_size);
		} else {
			/* We should never get back more bytes than we asked. */
			assert(sz == len);
		}

		progress_add(sz);
		if (read_error == 0)
			verified += sz;
		rv->io_start += sz;
		rv->io_length -= sz;
		background_sleep();
	}

	free(rv);
	ret = ptcounter_add(rvp->verified_bytes, verified);
	if (ret)
		rvp->runtime_error = ret;
}

/* Queue a read verify request immediately. */
int
read_verify_schedule_now(
	struct read_verify_schedule	*rs)
{
	struct read_verify_pool		*rvp = rs->rvp;
	struct read_verify		*tmp;
	bool				ret;

	if (!rvp)
		return 0;

	dbg_printf("verify dev %u start %"PRIu64" len %"PRIu64"\n",
			rvp->dev, rs->io_start, rs->io_length);

	/* Worker thread saw a runtime error, don't queue more. */
	if (rvp->runtime_error)
		return rvp->runtime_error;

	/* Otherwise clone the request and queue the copy. */
	tmp = malloc(sizeof(struct read_verify));
	if (!tmp) {
		rvp->runtime_error = errno;
		return errno;
	}

	tmp->io_start = rs->io_start;
	tmp->io_length = rs->io_length;

	ret = -workqueue_add(&rvp->wq, read_verify, 0, tmp);
	if (ret) {
		free(tmp);
		rvp->runtime_error = ret;
		return ret;
	}

	/* Reset the schedule */
	rs->rvp = NULL;
	rs->io_length = 0;
	return 0;
}

/*
 * Schedule a read verification request.  We'll batch subsequent requests if
 * they're within 64k of each other.  Returns true if the schedule was updated,
 * or false if the caller should call read_verify_schedule_now().
 */
bool
try_read_verify_schedule_io(
	struct read_verify_schedule	*rs,
	struct read_verify_pool		*rvp,
	uint64_t			start,
	uint64_t			length)
{
	uint64_t			req_end;
	uint64_t			rv_end;

	assert(rvp->readbuf);

	/* Round up and down to the start of a miniosz chunk. */
	start &= ~(rvp->miniosz - 1);
	length = roundup(length, rvp->miniosz);

	req_end = start + length;
	rv_end = rs->io_start + rs->io_length;

	/* If the schedule is empty, stash the new IO. */
	if (!rs->rvp) {
		rs->rvp = rvp;
		rs->io_start = start;
		rs->io_length = length;

		return true;
	}

	/*
	 * If we have a stashed IO, we haven't changed pools, the error
	 * reporting is the same, and the two extents are close,
	 * we can combine them.
	 */
	if (rs->rvp == rvp && rs->io_length > 0 &&
	    ((start >= rs->io_start && start <= rv_end + RVP_IO_BATCH_LOCALITY) ||
	     (rs->io_start >= start &&
	      rs->io_start <= req_end + RVP_IO_BATCH_LOCALITY))) {
		rs->io_start = min(rs->io_start, start);
		rs->io_length = max(req_end, rv_end) - rs->io_start;

		return true;
	}

	return false;
}

/* How many bytes has this process verified? */
int
read_verify_bytes(
	struct read_verify_pool		*rvp,
	uint64_t			*bytes_checked)
{
	return ptcounter_value(rvp->verified_bytes, bytes_checked);
}
