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
#include "libfrog/bitmap.h"
#include "libfrog/convert.h"
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

/* Perform all verification IO in 32M chunks. */
#define RVP_IO_MAX_SIZE			MEGABYTES(32)

/*
 * If we're running in the background then we perform IO in 256k chunks
 * to reduce the load on the IO subsystem.
 */
#define RVP_BG_IO_MAX_SIZE		KILOBYTES(256)

/* What's the real maximum IO size? */
static inline unsigned int
rvp_io_max_size(void)
{
	return bg_mode > 0 ? RVP_BG_IO_MAX_SIZE : RVP_IO_MAX_SIZE;
}

/* Tolerate 2M holes in adjacent read verify requests. */
#define RVP_IO_BATCH_LOCALITY		MEGABYTES(2)

/*
 * Tolerate 256k holes in adjacent read verify requests when running in the
 * background.
 */
#define RVP_BG_IO_BATCH_LOCALITY	KILOBYTES(256)

/* How many holes are we willing to verify to reduce IO count? */
static inline unsigned int
rvp_io_batch_locality(void)
{
	return bg_mode > 0 ? RVP_BG_IO_BATCH_LOCALITY : RVP_IO_BATCH_LOCALITY;
}

struct read_verify {
	uint64_t		io_start;	/* bytes */
	uint64_t		io_length;	/* bytes */
};

struct read_verify_pool {
	struct workqueue	wq;		/* thread pool */
	struct scrub_ctx	*ctx;		/* scrub context */
	void			*readbuf;	/* read buffer */
	struct ptcounter	*verified_bytes;
	size_t			miniosz;	/* minimum io size, bytes */
	enum xfs_device		dev;		/* which device? */

	/*
	 * Store a runtime error code here so that we can stop the pool and
	 * return it to the caller.
	 */
	int			runtime_error;

	/* outputs: a bad block bitmap and a truncated flag */
	struct bitmap		*failmap;
	bool			truncated;
};

unsigned int
read_verify_nproc(
	struct scrub_ctx		*ctx)
{
	if (force_nr_threads)
		return force_nr_threads;

	/*
	 * Throwing all CPUs at verifying seems like a bad idea for foreground
	 * scrub, as does abusing I/O opt/min as that says absolutely nothing
	 * about parallelism.  The authors observed diminishing returns on
	 * verification speed past 8 IO threads, so that's the default.
	 */
	return 8;
}

/*
 * Create a thread pool to run read verifiers.
 */
int
read_verify_pool_alloc(
	struct scrub_ctx		*ctx,
	enum xfs_device			dev,
	struct read_verify_pool		**prvp)
{
	struct read_verify_pool		*rvp;
	const unsigned int		verifier_threads =
		read_verify_nproc(ctx);
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
	if (!rvp->wq.terminated)
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
	bitmap_free(&rvp->failmap);
	ptcounter_free(rvp->verified_bytes);
	free(rvp->readbuf);
	free(rvp);
}

/* Simulate disk errors. */
static int
verify_simulate_read_error(
	struct read_verify_pool	*rvp,
	uint64_t		start,
	ssize_t			*length)
{
	static int64_t		interval;
	uint64_t		start_interval;

	/* Simulated disk errors are disabled. */
	if (interval < 0)
		return 0;

	/* Figure out the disk read error interval. */
	if (interval == 0) {
		char		*p;

		/* Pretend there's bad media every so often, in bytes. */
		p = getenv("XFS_SCRUB_DISK_ERROR_INTERVAL");
		if (p == NULL) {
			interval = -1;
			return 0;
		}
		interval = strtoull(p, NULL, 10);
		interval &= ~(rvp->miniosz - 1);
	}
	if (interval <= 0) {
		interval = -1;
		return 0;
	}

	/*
	 * We simulate disk errors by pretending that there are media errors at
	 * predetermined intervals across the disk.  If a read verify request
	 * crosses one of those intervals we shorten it so that the next read
	 * will start on an interval threshold.  If the read verify request
	 * starts on an interval threshold, we send back EIO as if it had
	 * failed.
	 */
	if ((start % interval) == 0) {
		dbg_printf("dev %u: simulating disk error at %"PRIu64".\n",
				rvp->dev, start);
		return EIO;
	}

	start_interval = start / interval;
	if (start_interval != (start + *length) / interval) {
		*length = ((start_interval + 1) * interval) - start;
		dbg_printf(
"dev %u: simulating short read at %"PRIu64" to length %"PRIu64".\n",
				rvp->dev, start, *length);
	}

	return 0;
}

/* Use the XFS media verification ioctl to do the media scan */
static ssize_t
ioctl_verify(
	int			verify_fd,
	enum xfs_device		dev,
	uint64_t		start,
	uint64_t		length,
	bool			single_step)
{
	const uint64_t	orig_start_daddr = BTOBBT(start);
	struct xfs_verify_media	me = {
		.me_start_daddr	= orig_start_daddr,
		.me_end_daddr	= BTOBB(start + length),
		.me_dev		= dev,
		.me_rest_us	= bg_mode > 2 ? bg_mode - 1 : 0,
	};
	int			ret;

	if (single_step)
		me.me_flags |= XFS_VERIFY_MEDIA_REPORT;

	ret = ioctl(verify_fd, XFS_IOC_VERIFY_MEDIA, &me);
	if (ret < 0)
		return ret;
	if (me.me_ioerror) {
		errno = me.me_ioerror;
		return -1;
	}

	return BBTOB(me.me_start_daddr - orig_start_daddr);
}

/* Read-verify an extent of a disk device. */
static ssize_t
read_verify_one(
	struct read_verify_pool	*rvp,
	struct read_verify	*rv,
	ssize_t			len,
	bool			single_step)
{
	if (debug) {
		int		ret;

		ret = verify_simulate_read_error(rvp, rv->io_start, &len);
		if (ret) {
			errno = ret;
			return -1;
		}

		/* Don't actually issue the IO */
		if (getenv("XFS_SCRUB_DISK_VERIFY_SKIP"))
			return len;
	}

	if (rvp->ctx->no_verify_ioctl)
		return disk_read_verify(rvp->ctx->verify_disks[rvp->dev],
				rvp->readbuf, rv->io_start, len);
	return ioctl_verify(rvp->ctx->mnt.fd, rvp->dev, rv->io_start, len,
			single_step);
}

/* Remember a media error for later. */
static int
read_verify_error(
	struct read_verify_pool		*rvp,
	uint64_t			start,
	uint64_t			length,
	int				error)
{
	static pthread_mutex_t		lock = PTHREAD_MUTEX_INITIALIZER;
	int				ret;

	if (!length) {
		rvp->truncated = true;
		return 0;
	}

	if (!rvp->failmap) {
		struct bitmap *failmap;

		ret = -bitmap_alloc(&failmap);
		if (ret) {
			str_liberror(rvp->ctx, ret,
 _("allocating bad block bitmap"));
			return ret;
		}

		pthread_mutex_lock(&lock);
		if (!rvp->failmap)
			rvp->failmap = failmap;
		else
			bitmap_free(&failmap);
		pthread_mutex_unlock(&lock);
	}

	ret = -bitmap_set(rvp->failmap, start, length);
	if (ret) {
		str_liberror(rvp->ctx, ret, _("setting bad block bitmap"));
		return ret;
	}

	return 0;
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
	int				ret = 0, ret2;

	rvp = (struct read_verify_pool *)wq->wq_ctx;
	if (rvp->runtime_error)
		return;

	io_max_size = rvp_io_max_size();

	while (rv->io_length > 0) {
		read_error = 0;
		len = min(rv->io_length, io_max_size);
		dbg_printf("diskverify %u %"PRIu64" %zu\n", rvp->dev,
				rv->io_start, len);
		sz = read_verify_one(rvp, rv, len, io_max_size <= rvp->miniosz);
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
			switch (read_error) {
			case EIO:
			case EILSEQ:
			case EREMOTEIO:
			case ENODATA:
				break;
			default:
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
			ret = read_verify_error(rvp, rv->io_start, sz,
					read_error);
			if (ret)
				goto out_err;
		} else if (sz == 0) {
			/* No bytes at all?  Did we hit the end of the disk? */
			dbg_printf("EOF %u @ %"PRIu64" %zu err %d\n",
					rvp->dev, rv->io_start, sz, read_error);
			ret = read_verify_error(rvp, rv->io_start, sz,
					read_error);
			if (ret)
				goto out_err;
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

out_err:
	free(rv);
	ret2 = ptcounter_add(rvp->verified_bytes, verified);
	if (!ret && ret2)
		ret = ret2;
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
	int				ret;

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
	const unsigned int		locality = rvp_io_batch_locality();

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
	    ((start >= rs->io_start && start <= rv_end + locality) ||
	     (rs->io_start >= start &&
	      rs->io_start <= req_end + locality))) {
		rs->io_start = min(rs->io_start, start);
		rs->io_length = max(req_end, rv_end) - rs->io_start;

		return true;
	}

	return false;
}

/* Did read verification succeed? */
bool
read_verify_ok(
	const struct read_verify_pool	*rvp)
{
	return rvp->failmap == NULL && !rvp->truncated && !rvp->runtime_error;
}

/* Did the verification unexpectedly stop early due to short reads? */
bool
read_verify_truncated(
	const struct read_verify_pool	*rvp)
{
	return rvp->truncated;
}

/* How many bytes has this pool verified? */
uint64_t
read_verify_progress(
	const struct read_verify_pool	*rvp)
{
	uint64_t			ret = 0;

	ptcounter_value(rvp->verified_bytes, &ret);
	return ret;
}

/* Call @fn for every media failure this pool observed. */
int
read_verify_iterate_failed(
	struct read_verify_pool		*rvp,
	int				(*fn)(uint64_t, uint64_t, void *),
	void				*arg)
{
	if (!rvp->failmap)
		return 0;

	return -bitmap_iterate(rvp->failmap, fn, arg);
}

/* Call @fn for every media failure this pool observed in the given range. */
int
read_verify_iterate_failed_range(
	struct read_verify_pool		*rvp,
	uint64_t			start,
	uint64_t			length,
	int				(*fn)(uint64_t, uint64_t, void *),
	void				*arg)
{
	if (!rvp->failmap)
		return 0;

	return -bitmap_iterate_range(rvp->failmap, start, length, fn, arg);
}

/* Were there any media failures within the given range? */
bool
read_verify_has_failed(
	struct read_verify_pool		*rvp,
	uint64_t			start,
	uint64_t			length)
{
	if (rvp->failmap)
		return bitmap_test(rvp->failmap, start, length);
	return false;
}
