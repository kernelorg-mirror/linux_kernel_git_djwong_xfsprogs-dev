// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "platform_defs.h"
#include "command.h"
#include "init.h"
#include "libfrog/paths.h"
#include "input.h"
#include "libfrog/fsgeom.h"
#include "libfrog/fsrefcounts.h"
#include "libfrog/fsmap.h"
#include "libfrog/logging.h"
#include "libfrog/bulkstat.h"
#include "libfrog/bitmap.h"
#include "handle.h"
#include "space.h"

static unsigned int vflag;

#define BATCH_SIZE		1024

struct clear_tgt {
	unsigned long long	start;
	unsigned long long	length;
	unsigned long long	owners;
	unsigned long long	prio;
	unsigned long long	evacuated;
};

struct clear_req {
	/* all the blocks that we've tried to clear */
	struct bitmap		*visited;

	/* stat buffer of the open file */
	struct stat		statbuf;
	struct stat		temp_statbuf;
	struct stat		space_statbuf;

	/* handle to this filesystem */
	void			*fshandle;
	size_t			fshandle_sz;

	/* physical storage that we want to clear */
	unsigned long long	start;
	unsigned long long	length;
	dev_t			dev;

	/* convenience variable */
	bool			realtime;

	/* free space is mapped to this file */
	int			space_fd;

	/* temporary file for migrating file data */
	int			temp_fd;
};

static void
clearfree_help(void)
{
	printf(_(
"\n"
"Evacuate the contents of the given range of physical storage in the filesystem"
"\n"
" -r -- clear space on the realtime device.\n"
" -v -- print everything that happens along the way.\n"
"\n"
"The start and end arguments are required, and must be specified in units\n"
"of 512-byte blocks.\n"
"\n"));
}

/*
 * Extract an inode's generation number.  Returns 1 if it found something,
 * 0 if the inode is already gone, or -1 for error.
 */
static int
ino_gen(
	struct xfs_fd		*xfd,
	uint64_t		ino,
	uint32_t		*gen,
	uint16_t		*mode)
{
	struct xfs_bulkstat	bulkstat;
	int			ret;

	ret = xfrog_bulkstat_single(xfd, ino, 0, &bulkstat);
	if (ret) {
		if (errno == ENOENT || errno == EINVAL)
			return 0;

		fprintf(stderr, _("bulkstat inode 0x%llx: %s\n"),
				(unsigned long long)ino,
				strerror(errno));
		return -1;
	}

	assert(bulkstat.bs_ino == ino);

	*mode = bulkstat.bs_mode;
	*gen = bulkstat.bs_gen;
	return 1;
}

/*
 * Open an inode via handle.  Returns a file descriptor, -2 if the file is
 * gone, or -1 on error.
 */
static int
open_inode(
	struct xfs_fd		*xfd,
	const struct clear_req	*req,
	uint64_t		ino,
	uint32_t		gen)
{
	struct xfs_handle	handle = { };
	struct xfs_fsop_handlereq hreq = {
		.oflags		= O_RDONLY | O_NOATIME | O_NOFOLLOW |
				  O_NOCTTY | O_LARGEFILE,
		.ihandle	= &handle,
		.ihandlen	= sizeof(handle),
	};
	int			ret;

	memcpy(&handle.ha_fsid, req->fshandle, sizeof(handle.ha_fsid));
	handle.ha_fid.fid_len = sizeof(xfs_fid_t) -
			sizeof(handle.ha_fid.fid_len);
	handle.ha_fid.fid_pad = 0;
	handle.ha_fid.fid_ino = ino;
	handle.ha_fid.fid_gen = gen;

	/*
	 * Since we extracted the fshandle from the open file instead of using
	 * path_to_fshandle, the fsid cache doesn't know about the fshandle.
	 * Construct the open by handle request manually.
	 */
	ret = ioctl(xfd->fd, XFS_IOC_OPEN_BY_HANDLE, &hreq);
	if (ret < 0) {
		if (errno == ENOENT || errno == EINVAL)
			return -2;

		fprintf(stderr, _("open inode 0x%llx: %s\n"),
				(unsigned long long)ino,
				strerror(errno));
		return -1;
	}

	return ret;
}

#ifndef FALLOC_FL_MAP_FREE_SPACE
#define FALLOC_FL_MAP_FREE_SPACE	0x80
#endif

/*
 * Map all the free space in the region that we're clearing to the space
 * catcher file.
 */
static int
grab_free_space(
	const struct clear_req	*req)
{
	int			ret;

	ret = fallocate(req->space_fd, FALLOC_FL_MAP_FREE_SPACE, req->start,
			req->length);
	if (ret) {
		perror(_("map free space"));
		return -1;
	}

	return 0;
}

/* Allocate the structures needed for a fsmap query. */
static struct fsmap_head *
open_fsmap_query(
	dev_t			dev,
	unsigned long long	physical,
	unsigned long long	length)
{
	struct fsmap_head	*mhead;

	mhead = malloc(fsmap_sizeof(BATCH_SIZE));
	if (!mhead) {
		perror(_("opening fsmap query"));
		return NULL;
	}

	memset(mhead, 0, sizeof(struct fsmap_head));
	mhead->fmh_count = BATCH_SIZE;
	mhead->fmh_keys[0].fmr_device = dev;
	mhead->fmh_keys[0].fmr_physical = physical;
	mhead->fmh_keys[1].fmr_device = dev;
	mhead->fmh_keys[1].fmr_physical = physical + length;
	mhead->fmh_keys[1].fmr_owner = ULLONG_MAX;
	mhead->fmh_keys[1].fmr_flags = UINT_MAX;
	mhead->fmh_keys[1].fmr_offset = ULLONG_MAX;

	return mhead;
}

/* Set us up for the next run_fsmap_query, or return false. */
static inline bool advance_fsmap_cursor(struct fsmap_head *mhead)
{
	struct fsmap	*mrec;

	mrec = &mhead->fmh_recs[mhead->fmh_entries - 1];
	if (mrec->fmr_flags & FMR_OF_LAST)
		return false;

	fsmap_advance(mhead);
	return true;
}

/*
 * Run a GETFSMAP query.  Returns 1 if there are rows, 0 if there are no rows,
 * or -1 for error.
 */
static inline int
run_fsmap_query(
	struct xfs_fd		*xfd,
	struct fsmap_head	*mhead)
{
	int			ret;

	if (mhead->fmh_entries > 0 && !advance_fsmap_cursor(mhead))
		return 0;

	ret = ioctl(xfd->fd, FS_IOC_GETFSMAP, mhead);
	if (ret) {
		perror(_("querying fsmap data"));
		return -1;
	}

	if (!(mhead->fmh_oflags & FMH_OF_DEV_T)) {
		fprintf(stderr, _("fsmap does not return dev_t.\n"));
		return -1;
	}

	if (mhead->fmh_entries == 0)
		return 0;

	return 1;
}

#define for_each_fsmap(mhead, rec) \
	for ((rec) = (mhead)->fmh_recs; \
	     (rec) < (mhead)->fmh_recs + (mhead)->fmh_entries; \
	     (rec)++)

/* Allocate the structures needed for a fsrefcounts query. */
static struct fsrefs_head *
open_fsrefs_query(
	dev_t			dev,
	unsigned long long	physical,
	unsigned long long	length)
{
	struct fsrefs_head	*rhead;

	rhead = malloc(fsrefs_sizeof(BATCH_SIZE));
	if (!rhead) {
		perror(_("opening refcount query"));
		return NULL;
	}

	memset(rhead, 0, sizeof(struct fsrefs_head));
	rhead->fch_count = BATCH_SIZE;
	rhead->fch_keys[0].fcr_device = dev;
	rhead->fch_keys[0].fcr_physical = physical;
	rhead->fch_keys[1].fcr_device = dev;
	rhead->fch_keys[1].fcr_physical = physical + length;
	rhead->fch_keys[1].fcr_owners = ULLONG_MAX;
	rhead->fch_keys[1].fcr_flags = UINT_MAX;

	return rhead;
}

/* Set us up for the next run_fsrefs_query, or return false. */
static inline bool advance_fsrefs_query(struct fsrefs_head *rhead)
{
	struct fsrefs	*rrec;

	rrec = &rhead->fch_recs[rhead->fch_entries - 1];
	if (rrec->fcr_flags & FCR_OF_LAST)
		return false;

	fsrefs_advance(rhead);
	return true;
}

/*
 * Run a GETFSREFCOUNTS query.  Returns 1 if there are rows, 0 if there are
 * no rows, or -1 for error.
 */
static inline int
run_fsrefs_query(
	struct xfs_fd		*xfd,
	struct fsrefs_head	*rhead)
{
	int			ret;

	if (rhead->fch_entries > 0 && !advance_fsrefs_query(rhead))
		return 0;

	ret = ioctl(xfd->fd, FS_IOC_GETFSREFCOUNTS, rhead);
	if (ret) {
		perror(_("querying refcount data"));
		return -1;
	}

	if (!(rhead->fch_oflags & FCH_OF_DEV_T)) {
		fprintf(stderr, _("fsrefcounts does not return dev_t.\n"));
		return -1;
	}

	if (rhead->fch_entries == 0)
		return 0;

	return 1;
}

#define for_each_fsref(rhead, rec) \
	for ((rec) = (rhead)->fch_recs; \
	     (rec) < (rhead)->fch_recs + (rhead)->fch_entries; \
	     (rec)++)

/*
 * Rank a refcount record.  We prefer to tackle highly shared and longer
 * extents first.
 */
static inline unsigned long long
fsrefs_prio(
	const struct xfs_fsop_geom	*g,
	const struct fsrefs		*p)
{
	unsigned long long		blocks = p->fcr_length / g->blocksize;
	unsigned long long		ret = blocks * p->fcr_owners;

	if (ret < blocks || ret < p->fcr_owners)
		return UINT64_MAX;
	return ret;
}

/* Make the current refcount record the clearing target if desirable. */
static void
try_retain(
	const struct clear_req		*req,
	struct clear_tgt		*target,
	const struct fsrefs		*rec,
	unsigned long long		prio)
{
	if (prio < target->prio)
		return;
	if (prio == target->prio &&
	    rec->fcr_length <= target->length)
		return;

	/* Ignore regions that we already tried to clear. */
	if (bitmap_test(req->visited, rec->fcr_physical, rec->fcr_length))
		return;

	if (vflag > 2)
		fprintf(stderr, "retain prio boost %llu -> %llu\n",
				target->prio, prio);

	target->start = rec->fcr_physical;
	target->length = rec->fcr_length;
	target->owners = rec->fcr_owners;
	target->prio = prio;
}

/*
 * Decide if this refcount record maps to extents that are sufficiently
 * interesting to target.
 */
static int
evaluate_refcount(
	struct xfs_fd			*xfd,
	const struct clear_req		*req,
	const struct fsrefs		*rrec,
	struct clear_tgt		*target)
{
	struct fsmap_head		*mhead;
	const struct xfs_fsop_geom	*fsgeom = &xfd->fsgeom;
	unsigned long long		prio = fsrefs_prio(fsgeom, rrec);
	int				ret;

	if (vflag > 2)
		fprintf(stderr,
	"refc: start %llu length %llu owners %llu prio %llu oldprio %llu\n",
				(unsigned long long)rrec->fcr_physical,
				(unsigned long long)rrec->fcr_length,
				(unsigned long long)rrec->fcr_owners,
				prio, target->prio);

	if (prio < target->prio)
		return 0;

	/*
	 * XFS only supports sharing data blocks.  If there's more than one
	 * owner, we know that we can easily move the blocks.
	 */
	if (rrec->fcr_owners > 1) {
		try_retain(req, target, rrec, prio);
		return 0;
	}

	/*
	 * Otherwise, this extent has single owners.  Walk the fsmap records to
	 * figure out if they're movable or not.
	 */
	mhead = open_fsmap_query(rrec->fcr_device, rrec->fcr_physical,
			rrec->fcr_length);
	if (!mhead)
		return -1;

	while ((ret = run_fsmap_query(xfd, mhead)) > 0) {
		struct fsmap	*mrec;
		uint64_t	next_phys = 0;

		for_each_fsmap(mhead, mrec) {
			struct fsrefs	fake_rec = { };

			if (vflag > 2 && target->prio == 0)
				fprintf(stderr,
	"rmap0: dev %u:%u phys %llu owner %lld length %llu flags 0x%llx next_phys %llu prio %llu\n",
					major(mrec->fmr_device),
					minor(mrec->fmr_device),
					(unsigned long long)mrec->fmr_physical,
					(unsigned long long)mrec->fmr_owner,
					(unsigned long long)mrec->fmr_length,
					(unsigned long long)mrec->fmr_flags,
					(unsigned long long)next_phys,
					prio);

			if (mrec->fmr_device != rrec->fcr_device)
				continue;
			if (mrec->fmr_flags & FMR_OF_SPECIAL_OWNER)
				continue;
			if (mrec->fmr_owner == req->temp_statbuf.st_ino)
				continue;
			if (mrec->fmr_owner == req->space_statbuf.st_ino)
				continue;

			/*
			 * If the space has become shared since the fsrefs
			 * query, just skip this record.  We might come back to
			 * it in a later iteration.
			 */
			if (mrec->fmr_physical < next_phys)
				continue;

			/* Fake enough of a fsrefs to calculate the priority. */
			fake_rec.fcr_physical = mrec->fmr_physical;
			fake_rec.fcr_length = mrec->fmr_length;
			fake_rec.fcr_owners = 1;
			prio = fsrefs_prio(fsgeom, &fake_rec);

			/* Target unwritten extents first; they're cheap. */
			if (mrec->fmr_flags & FMR_OF_PREALLOC)
				prio |= (1ULL << 63);

			if (vflag > 2)
				fprintf(stderr,
	"rmap: start %llu length %llu prio %llu\n",
					(unsigned long long)mrec->fmr_physical,
					(unsigned long long)mrec->fmr_length,
					prio);

			try_retain(req, target, &fake_rec, prio);

			next_phys = mrec->fmr_physical + mrec->fmr_length;
		}
	}

	free(mhead);
	return ret;
}

/*
 * Given a range of storage to search, find the most appealing target for space
 * clearing.  If nothing suitable is found, the target will be zeroed.
 */
static int
find_clearing_target(
	struct xfs_fd		*xfd,
	const struct clear_req	*req,
	struct clear_tgt	*target)
{
	struct fsrefs_head	*rhead;
	int			ret;

	memset(target, 0, sizeof(struct clear_tgt));

	rhead = open_fsrefs_query(req->dev, req->start, req->length);
	if (!rhead)
		return -1;

	while ((ret = run_fsrefs_query(xfd, rhead)) > 0) {
		struct fsrefs	*rrec;

		for_each_fsref(rhead, rrec) {
			if (rrec->fcr_device != req->dev)
				continue;

			ret = evaluate_refcount(xfd, req, rrec, target);
			if (ret)
				goto out;
		}
	}

	ret = bitmap_set(req->visited, target->start, target->length);
	if (ret) {
		perror(_("marking extent visited"));
		goto out;
	}
out:
	free(rhead);
	return ret;
}

/*
 * Given a fsmapping, try to reflink the physical space into the tempfd file.
 * This freezes the physical space; concurrent file writes will CoW to
 * somewhere else in the filesystem.
 */
static int
map_fsmap_to_tempfd(
	struct xfs_fd		*xfd,
	const struct clear_req	*req,
	const struct clear_tgt	*target,
	unsigned long long	*cursor,
	const struct fsmap	*mrec)
{
	struct fsmap		short_mrec;
	struct file_clone_range	fcr = { };
	struct getbmapx		bmap[2];
	uint32_t		gen;
	uint16_t		mode;
	int			src_fd;
	int			ret, ret2;

	if (mrec->fmr_device != req->dev) {
		fprintf(stderr, _("wrong fsmap device in results.\n"));
		return -1;
	}

	if (mrec->fmr_owner == req->temp_statbuf.st_ino)
		return 0;
	if (mrec->fmr_owner == req->space_statbuf.st_ino)
		return 0;

	/* Skip mappings before the cursor. */
	if (mrec->fmr_physical + mrec->fmr_length < *cursor)
		return 0;

	/*
	 * Open this file so that we can try to freeze its data blocks.
	 * For other types of files we just skip to the evacuation step.
	 */
	ret = ino_gen(xfd, mrec->fmr_owner, &gen, &mode);
	if (ret <= 0)
		return ret;

	if (!S_ISREG(mode) && !S_ISDIR(mode))
		return 0;

	src_fd = open_inode(xfd, req, mrec->fmr_owner, gen);
	if (src_fd == -2)
		return 0;
	if (src_fd < 0)
		return src_fd;

	/*
	 * Remapping only works for regular file data blocks.  If that isn't
	 * the case, our only recourse is online rebuild.
	 */
	if (S_ISDIR(mode) ||
	    (mrec->fmr_flags & (FMR_OF_ATTR_FORK | FMR_OF_EXTENT_MAP))) {
		ret = 0;
		goto out_fd;
	}

	if (vflag > 2)
		fprintf(stderr,
	"try remap ino 0x%llx pos %llu len %llu flags 0x%llx\n",
				(unsigned long long)mrec->fmr_owner,
				(unsigned long long)mrec->fmr_offset,
				(unsigned long long)mrec->fmr_length,
				(unsigned long long)mrec->fmr_flags);

	/*
	 * If the cursor is in the middle of this mapping, increase the start
	 * of the mapping to start at the cursor.
	 */
	if (mrec->fmr_physical < *cursor) {
		unsigned long long	delta = *cursor - mrec->fmr_physical;

		short_mrec = *mrec;
		short_mrec.fmr_physical = *cursor;
		short_mrec.fmr_offset += delta;
		short_mrec.fmr_length -= delta;

		mrec = &short_mrec;
	}

	if (mrec->fmr_length == 0) {
		if (vflag > 1)
			fprintf(stderr, "skipping zero-length remap\n");
		ret = 0;
		goto out_fd;
	}

	/* Remap the space into the tempfd. */
	fcr.src_fd = src_fd;
	fcr.src_offset = mrec->fmr_offset;
	fcr.src_length = mrec->fmr_length;
	fcr.dest_offset = mrec->fmr_physical;

	if (vflag > 1)
		fprintf(stderr,
	"remap ino 0x%llx pos %llu len %llu phys %llu to tempfd pos %llu\n",
				(unsigned long long)mrec->fmr_owner,
				(unsigned long long)mrec->fmr_offset,
				(unsigned long long)mrec->fmr_length,
				(unsigned long long)mrec->fmr_physical,
				(unsigned long long)fcr.dest_offset);

	ret = ioctl(req->temp_fd, FICLONERANGE, &fcr);
	if (ret) {
		perror(_("remapping target space to tempfd"));
		goto out_fd;
	}

	/* Did we actually remap the space we wanted? */
	memset(bmap, 0, sizeof(bmap));
	bmap[0].bmv_offset = BTOBB(fcr.dest_offset);
	bmap[0].bmv_length = BTOBB(fcr.src_length);
	bmap[0].bmv_count = 2;
	bmap[0].bmv_iflags = BMV_IF_PREALLOC | BMV_IF_DELALLOC;
	ret = ioctl(req->temp_fd, XFS_IOC_GETBMAPX, &bmap);
	if (ret < 0) {
		perror(_("checking tempfd bmap after remap"));
		goto out_fd;
	}

	if (vflag > 1)
		fprintf(stderr,
	"tempfd pos %llu len %llu maps to phys %llu len %llu, wanted phys %llu\n",
				(unsigned long long)fcr.dest_offset,
				(unsigned long long)fcr.src_length,
				(unsigned long long)BBTOB(bmap[1].bmv_block),
				(unsigned long long)BBTOB(bmap[1].bmv_length),
				(unsigned long long)mrec->fmr_physical);

	/* unwritten extents become holes */
	if (mrec->fmr_flags & FMR_OF_PREALLOC) {
		if (bmap[1].bmv_block == -1)
			goto advance;
		goto out_fd;
	}

	/* written extents do no match, so truncate the tempfd and try again. */
	if (BBTOB(bmap[1].bmv_block) != mrec->fmr_physical) {
		ret = ftruncate(req->temp_fd, fcr.dest_offset);
		if (ret)
			perror(_("truncating tempfd after failed remap"));
		goto out_fd;
	}

	/* Now remap it into the space file. */
	fcr.src_fd = req->temp_fd;
	fcr.src_offset = fcr.dest_offset;
	fcr.dest_offset = mrec->fmr_physical;

	if (vflag > 1)
		fprintf(stderr, "remap phys %llu len %llu to spacefd\n",
				(unsigned long long)mrec->fmr_physical,
				(unsigned long long)mrec->fmr_length);

	ret = ioctl(req->space_fd, FICLONERANGE, &fcr);
	if (ret) {
		perror(_("remapping target space to spacefd"));
		goto out_fd;
	}

advance:
	/* Update cursor if that succeeded. */
	*cursor = mrec->fmr_physical + mrec->fmr_length;
out_fd:
	ret2 = close(src_fd);
	if (!ret && ret2)
		ret = ret2;
	if (ret) fprintf(stderr, "%s:%d ret %d\n", __func__, __LINE__, ret);
	return ret;
}

static void
trim_fsmap(
	const struct clear_tgt	*target,
	struct fsmap		*fsmap)
{
	unsigned long long	delta, end;
	bool			need_off;

	need_off = (fsmap->fmr_flags & (FMR_OF_EXTENT_MAP |
					FMR_OF_SPECIAL_OWNER));

	if (fsmap->fmr_physical < target->start) {
		delta = target->start - fsmap->fmr_physical;
		fsmap->fmr_physical = target->start;
		fsmap->fmr_length -= delta;
		if (need_off)
			fsmap->fmr_offset += delta;
	}

	end = fsmap->fmr_physical + fsmap->fmr_length;
	if (end > target->start + target->length) {
		delta = end - (target->start + target->length);
		fsmap->fmr_length -= delta;
	}
}

/*
 * Map the data file blocks in the target physical range into the tempfd.
 */
static int
map_target_to_tempfd(
	struct xfs_fd		*xfd,
	const struct clear_req	*req,
	const struct clear_tgt	*target)
{
	struct fsmap_head	*mhead;
	unsigned long long	cursor = target->start;
	int			ret;

	ret = ftruncate(req->temp_fd, 0);
	if (ret) {
		perror(_("truncating temp file"));
		return -1;
	}

	mhead = open_fsmap_query(req->dev, target->start, target->length);
	if (!mhead)
		return -1;

	while ((ret = run_fsmap_query(xfd, mhead)) > 0) {
		struct fsmap	*mrec;

		for_each_fsmap(mhead, mrec) {
			trim_fsmap(target, mrec);
			ret = map_fsmap_to_tempfd(xfd, req, target, &cursor,
					mrec);
			if (ret)
				goto out;
		}
	}
out:
	free(mhead);
	if (ret) fprintf(stderr, "%s:%d ret %d\n", __func__, __LINE__, ret);
	return ret;
}

/* Try to evacuate blocks by using online repair. */
static int
try_rebuild_file_metadata(
	struct xfs_fd		*xfd,
	struct clear_tgt	*target,
	const struct fsmap	*mrec,
	int			fd,
	uint32_t		gen,
	uint16_t		mode)
{
	struct xfs_scrub_metadata scrub = {
		.sm_type	= XFS_SCRUB_TYPE_PROBE,
		.sm_flags	= XFS_SCRUB_IFLAG_REPAIR |
				  XFS_SCRUB_IFLAG_FREEZE_OK |
				  XFS_SCRUB_IFLAG_FORCE_REBUILD,
	};
	int			ret;

	if (vflag > 2)
		fprintf(stderr,
	"try rebuild ino 0x%llx pos %llu len %llu flags 0x%llx\n",
				(unsigned long long)mrec->fmr_owner,
				(unsigned long long)mrec->fmr_offset,
				(unsigned long long)mrec->fmr_length,
				(unsigned long long)mrec->fmr_flags);

	if (fd == -1) {
		scrub.sm_ino = mrec->fmr_owner;
		scrub.sm_gen = gen;
		fd = xfd->fd;
	}

	if (mrec->fmr_flags & FMR_OF_ATTR_FORK) {
		if (mrec->fmr_flags & FMR_OF_EXTENT_MAP)
			scrub.sm_type = XFS_SCRUB_TYPE_BMBTA;
		else
			scrub.sm_type = XFS_SCRUB_TYPE_XATTR;
	} else if (mrec->fmr_flags & FMR_OF_EXTENT_MAP) {
		scrub.sm_type = XFS_SCRUB_TYPE_BMBTD;
	} else if (S_ISLNK(mode)) {
		scrub.sm_type = XFS_SCRUB_TYPE_SYMLINK;
	} else if (S_ISDIR(mode)) {
		scrub.sm_type = XFS_SCRUB_TYPE_DIR;
	}

	if (scrub.sm_type == XFS_SCRUB_TYPE_PROBE)
		return 0;

	if (vflag > 2)
		fprintf(stderr, "rebuild ino 0x%llx gen 0x%x type %u\n",
				(unsigned long long)mrec->fmr_owner,
				(unsigned int)gen,
				(unsigned int)scrub.sm_type);

	ret = ioctl(fd, XFS_IOC_SCRUB_METADATA, &scrub);
	if (ret) {
		fprintf(stderr, _("rebuilding inode 0x%llx type %u\n"),
				mrec->fmr_owner, scrub.sm_type);
		return -1;
	}

	target->evacuated++;
	return 0;
}

/* Evacuate one fsmapping. */
static int
evacuate_data_fsmap(
	struct xfs_fd		*xfd,
	const struct clear_req	*req,
	struct clear_tgt	*target,
	const struct fsmap	*mrec)
{
	struct file_dedupe_range *fdr;
	struct file_dedupe_range_info *info;
	unsigned long long	real_length;
	uint32_t		gen;
	uint16_t		mode;
	int			dest_fd;
	int			ret, ret2;

	if (mrec->fmr_device != req->dev) {
		fprintf(stderr, _("wrong fsmap device in results.\n"));
		return -1;
	}

	if (mrec->fmr_owner == req->temp_statbuf.st_ino)
		return 0;
	if (mrec->fmr_owner == req->space_statbuf.st_ino)
		return 0;

	/*
	 * Open this file so that we can try to freeze its data blocks.
	 * For other types of files we just skip to the evacuation step.
	 */
	ret = ino_gen(xfd, mrec->fmr_owner, &gen, &mode);
	if (ret <= 0)
		return ret;

	/*
	 * We're only allowed to open regular files and directories via handle
	 * so jump to online rebuild for these types.
	 */
	if (!S_ISREG(mode) && !S_ISDIR(mode))
		return try_rebuild_file_metadata(xfd, target, mrec, -1, gen,
				mode);

	dest_fd = open_inode(xfd, req, mrec->fmr_owner, gen);
	if (dest_fd == -2)
		return 0;
	if (dest_fd < 0)
		return dest_fd;

	/*
	 * Remapping only works for regular file data blocks.  If that isn't
	 * the case, our only recourse is online rebuild.
	 */
	if (S_ISDIR(mode) ||
	    (mrec->fmr_flags & (FMR_OF_ATTR_FORK | FMR_OF_EXTENT_MAP))) {
		ret = try_rebuild_file_metadata(xfd, target, mrec, dest_fd,
				gen, mode);
		goto out_fd;
	}

	if (vflag > 2)
		fprintf(stderr,
	"try evac ino 0x%llx pos %llu len %llu flags 0x%llx\n",
				(unsigned long long)mrec->fmr_owner,
				(unsigned long long)mrec->fmr_offset,
				(unsigned long long)mrec->fmr_length,
				(unsigned long long)mrec->fmr_flags);

	/*
	 * Use the fsmapping to remap the target file into the funshared space
	 * in the tempfd.  In other words, the tempfd has a new copy of the
	 * data and we are going to try to reconstruct the sharing relationship.
	 */
	fdr = calloc(1, sizeof(struct file_dedupe_range) +
			sizeof(struct file_dedupe_range_info));
	if (!fdr) {
		perror(_("allocating dedupe context"));
		ret = -1;
		goto out_fd;
	}

	fdr->src_offset = mrec->fmr_physical;
	fdr->src_length = mrec->fmr_length;
	fdr->dest_count = 1;
	info = &fdr->info[0];
	info->dest_fd = dest_fd;
	info->dest_offset = mrec->fmr_offset;

	/* First we try to do the entire thing all at once. */
	do {
		ret = ioctl(req->temp_fd, FIDEDUPERANGE, fdr);
		if (ret) {
			fprintf(stderr, _("evacuating inode 0x%llx: %s\n"),
					mrec->fmr_owner, strerror(errno));
			goto out_fdr;
		}

		if (info->status < 0) {
			ret = info->status;
			goto out_fdr;
		}
		if (info->status == FILE_DEDUPE_RANGE_DIFFERS)
			break;

		if (fdr->src_length < info->bytes_deduped) {
			ret = -1;
			errno = EL3HLT;
			goto out_fdr;
		}

		if (vflag)
			fprintf(stderr,
	"evacuated ino 0x%llx pos %llu len %llu flags 0x%llx\n",
					(unsigned long long)mrec->fmr_owner,
					(unsigned long long)mrec->fmr_offset,
					(unsigned long long)mrec->fmr_length,
					(unsigned long long)mrec->fmr_flags);

		fdr->src_offset += info->bytes_deduped;
		info->dest_offset += info->bytes_deduped;
		fdr->src_length -= info->bytes_deduped;
		target->evacuated++;
	} while (fdr->src_length > 0);

	/* Ok, that didn't work.  Let's walk the blocks one by one. */
	real_length = fdr->src_length;
	while (real_length > 0) {
		fdr->src_length = min(xfd->fsgeom.blocksize, real_length);

		ret = ioctl(req->temp_fd, FIDEDUPERANGE, fdr);
		if (ret) {
			fprintf(stderr, _("slow evacuating inode 0x%llx: %s\n"),
					mrec->fmr_owner, strerror(errno));
			goto out_fdr;
		}

		if (info->status < 0) {
			ret = info->status;
			goto out_fdr;
		}

		if (info->status == FILE_DEDUPE_RANGE_SAME) {
			if (fdr->src_length < info->bytes_deduped) {
				ret = -1;
				errno = EL3HLT;
				goto out_fdr;
			}

			if (vflag)
				fprintf(stderr,
	"slow evacuated ino 0x%llx pos %llu len %llu flags 0x%llx\n",
					(unsigned long long)mrec->fmr_owner,
					(unsigned long long)mrec->fmr_offset,
					(unsigned long long)mrec->fmr_length,
					(unsigned long long)mrec->fmr_flags);

			target->evacuated++;
		} else {
			info->bytes_deduped = fdr->src_length;
		}

		fdr->src_offset += info->bytes_deduped;
		info->dest_offset += info->bytes_deduped;
		real_length -= info->bytes_deduped;
	}

out_fdr:
	free(fdr);
out_fd:
	ret2 = close(dest_fd);
	if (!ret && ret2)
		ret = ret2;
	return ret;
}

/* Use deduplication to remap data extents away from where we're clearing. */
static int
evacuate_files(
	struct xfs_fd		*xfd,
	const struct clear_req	*req,
	struct clear_tgt	*target)
{
	struct fsmap_head	*mhead;
	int			ret;

	mhead = open_fsmap_query(req->dev, target->start, target->length);
	if (!mhead)
		return -1;

	while ((ret = run_fsmap_query(xfd, mhead)) > 0) {
		struct fsmap	*mrec;

		for_each_fsmap(mhead, mrec) {
			trim_fsmap(target, mrec);
			ret = evacuate_data_fsmap(xfd, req, target, mrec);
			if (ret)
				goto out;
		}
	}
out:
	free(mhead);
	return ret;
}

/* Try to evacuate all data blocks in the target region using reflink. */
static int
clear_target_dedupe(
	struct xfs_fd		*xfd,
	const struct clear_req	*req,
	struct clear_tgt	*target)
{
	int			ret;

	if (vflag)
		fprintf(stderr,
	"Target: phys %llu len %llu owners %llu prio %llu\n",
				target->start, target->length,
				target->owners, target->prio);

	ret = map_target_to_tempfd(xfd, req, target);
	if (ret)
		return ret;

	ret = fallocate(req->temp_fd, FALLOC_FL_UNSHARE_RANGE, target->start,
			target->length);
	if (ret) {
		perror(_("unsharing tempfd data"));
		return ret;
	}

	ret = evacuate_files(xfd, req, target);
	if (ret)
		return ret;

	return 0;
}

/* Try to evacuate blocks by using online repair to rebuild AG metadata. */
static int
try_rebuild_ag_metadata(
	struct xfs_fd		*xfd,
	const struct clear_req	*req,
	struct clear_tgt	*target,
	uint32_t		agno,
	uint32_t		mask)
{
	struct xfs_scrub_metadata scrub = {
		.sm_flags	= XFS_SCRUB_IFLAG_REPAIR |
				  XFS_SCRUB_IFLAG_FREEZE_OK |
				  XFS_SCRUB_IFLAG_FORCE_REBUILD,
	};
	unsigned int		i;
	int			ret;

	if (vflag > 2)
		fprintf(stderr, "try rebuild ag %u mask 0x%x\n",
				(unsigned int)agno,
				(unsigned int)mask);

	for (i = XFS_SCRUB_TYPE_AGFL; i < XFS_SCRUB_TYPE_REFCNTBT; i++) {

		if (!(mask & (1U << i)))
			continue;

		scrub.sm_type = i;

		if (vflag > 2)
			fprintf(stderr, "try rebuild ag %u type %u\n",
					(unsigned int)agno,
					(unsigned int)scrub.sm_type);

		ret = ioctl(xfd->fd, XFS_IOC_SCRUB_METADATA, &scrub);
		if (ret) {
			fprintf(stderr, _("rebuilding ag %u type %u\n"),
					(unsigned int)agno, scrub.sm_type);
			return -1;
		}

		target->evacuated++;

		ret = grab_free_space(req);
		if (ret)
			return ret;
	}

	return 0;
}

/* Compute a scrub mask for a fsmap special owner. */
static uint32_t fsmap_owner_to_scrub_mask(__u64 owner)
{
	switch (owner) {
	case XFS_FMR_OWN_FREE:
	case XFS_FMR_OWN_UNKNOWN:
	case XFS_FMR_OWN_FS:
	case XFS_FMR_OWN_LOG:
		/* can't move these */
		return 0;
	case XFS_FMR_OWN_AG:
		return (1U << XFS_SCRUB_TYPE_BNOBT) |
		       (1U << XFS_SCRUB_TYPE_CNTBT) |
		       (1U << XFS_SCRUB_TYPE_AGFL) |
		       (1U << XFS_SCRUB_TYPE_RMAPBT);
	case XFS_FMR_OWN_INOBT:
		return (1U << XFS_SCRUB_TYPE_INOBT) |
		       (1U << XFS_SCRUB_TYPE_FINOBT);
	case XFS_FMR_OWN_REFC:
		return (1U << XFS_SCRUB_TYPE_REFCNTBT);
	case XFS_FMR_OWN_INODES:
	case XFS_FMR_OWN_COW:
		/* don't know how to get rid of these */
		return 0;
	case XFS_FMR_OWN_DEFECTIVE:
		/* good, get rid of it */
		return 0;
	default:
		return 0;
	}
}

/* Try to clear all per-AG metadata from the requested range. */
static int
clear_metadata(
	struct xfs_fd		*xfd,
	const struct clear_req	*req,
	struct clear_tgt	*target,
	bool			*cleared_anything)
{
	struct fsmap_head	*mhead;
	uint32_t		curr_agno = -1U;
	uint32_t		curr_mask = 0;
	int			ret;

	if (req->realtime)
		return 0;

	mhead = open_fsmap_query(req->dev, target->start, target->length);
	if (!mhead)
		return -1;

	while ((ret = run_fsmap_query(xfd, mhead)) > 0) {
		struct fsmap	*mrec;

		for_each_fsmap(mhead, mrec) {
			uint64_t	daddr;
			uint32_t	agno;
			uint32_t	mask;

			if (mrec->fmr_device != req->dev)
				continue;
			if (!(mrec->fmr_flags & FMR_OF_SPECIAL_OWNER))
				continue;

			/* Ignore regions that we already tried to clear. */
			if (bitmap_test(req->visited, mrec->fmr_physical,
						mrec->fmr_length))
				goto out;

			mask = fsmap_owner_to_scrub_mask(mrec->fmr_owner);
			if (!mask)
				continue;

			daddr = BTOBB(mrec->fmr_physical);
			agno = cvt_daddr_to_agno(xfd, daddr);

			if (vflag > 2)
				fprintf(stderr,
	"agno %u:%u mask 0x%x owner %lld\n",
						curr_agno, agno, curr_mask,
						(unsigned long long)mrec->fmr_owner);

			if (curr_agno == -1U) {
				curr_agno = agno;
			} else if (curr_agno != agno) {
				/* XXX why do we go around in a loop sometimes? */
				ret = try_rebuild_ag_metadata(xfd, req, target,
						curr_agno, curr_mask);
				if (ret)
					goto out;

				*cleared_anything = true;
				curr_agno = agno;
				curr_mask = 0;
			}

			/* Put this on the list and try to clear it once. */
			curr_mask |= mask;
			ret = bitmap_set(req->visited, mrec->fmr_physical,
					mrec->fmr_length);
			if (ret) {
				perror(_("marking metadata extent visited"));
				goto out;
			}
		}
	}

	if (curr_agno != -1U && curr_mask != 0) {
		ret = try_rebuild_ag_metadata(xfd, req, target, curr_agno,
				curr_mask);
		if (ret)
			goto out;
		*cleared_anything = true;
	}

out:
	free(mhead);
	return ret;
}

/* Dump all speculative preallocations and COW staging blocks. */
static int
dump_speculative_preallocations(
	struct xfs_fd		*xfd)
{
	struct xfs_fs_eofblocks	eofb = {
		.eof_version	= XFS_EOFBLOCKS_VERSION,
		.eof_flags	= XFS_EOF_FLAGS_SYNC,
	};
	int			ret;

	if (vflag > 2)
		fprintf(stderr, "dumping speculative preallocations\n");

	ret = ioctl(file->xfd.fd, XFS_IOC_FREE_EOFBLOCKS, &eofb);
	if (ret) {
		perror(_("dumping speculative preallocations and CoW\n"));
		return -1;
	}

	return 0;
}

static inline void
target_metadata(
	const struct clear_req	*req,
	struct clear_tgt	*target)
{
	target->start = req->start;
	target->length = req->length;
	target->prio = 0;
	target->evacuated = 0;
	target->owners = 0;
}

/*
 * Loop through the space to find the most appealing part of the device to
 * clear, then try to evacuate everything within.
 */
static int
clear_space(
	struct xfs_fd		*xfd,
	const struct clear_req	*req)
{
	struct clear_tgt	target;
	bool			cleared_anything;
	int			ret;

	if (vflag)
		fprintf(stderr,
	"Range to clear: dev %u:%u physical %llu len %llu\n",
				major(req->dev), minor(req->dev),
				req->start, req->length);

	/* Grab all the free space and dump all speculative preallocations. */
	ret = grab_free_space(req);
	if (ret)
		return ret;

	/* Empty out CoW forks before relocating data extents. */
	ret = syncfs(xfd->fd);
	if (ret)
		return ret;

	ret = dump_speculative_preallocations(xfd);
	if (ret)
		return ret;

	/* Evacuate as many file blocks as we can. */
	do {
		ret = grab_free_space(req);
		if (ret)
			return ret;

		ret = find_clearing_target(xfd, req, &target);
		if (ret)
			return ret;

		if (target.length == 0)
			break;

		ret = clear_target_dedupe(xfd, req, &target);
		if (ret)
			return ret;

		if (vflag)
			fprintf(stderr, _("Evacuated %llu file items.\n"),
					target.evacuated);
	} while (target.evacuated > 0);

	/* Evacuate as many AG metadata blocks as we can. */
	do {
		target_metadata(req, &target);

		ret = clear_metadata(xfd, req, &target, &cleared_anything);
		if (ret)
			return ret;

		if (vflag)
			fprintf(stderr, _("Evacuated %llu metadata items.\n"),
					target.evacuated);
	} while (target.evacuated > 0 && cleared_anything);

	return 0;
}

/*
 * Create a temporary file on the same volume (data/rt) that we're trying to
 * clear free space on.
 */
static int
open_temp_file(
	struct xfs_fd		*xfd,
	const struct clear_req	*req,
	struct stat		*statbuf)
{
	struct fsxattr		fsx;
	int			fd, ret;

	fd = openat(file->xfd.fd, ".", O_TMPFILE | O_RDWR | O_EXCL, 0600);
	if (fd < 0) {
		perror(_("opening temp file"));
		return -1;
	}

	/* Make sure we got the same filesystem as the open file. */
	ret = fstat(fd, statbuf);
	if (ret) {
		perror(_("stat temp file"));
		goto fail;
	}
	if (statbuf->st_dev != req->statbuf.st_dev) {
		fprintf(stderr,
	_("Cannot create temp file on same fs as open file.\n"));
		goto fail;
	}

	/* Ensure this file targets the correct data/rt device. */
	ret = ioctl(fd, FS_IOC_FSGETXATTR, &fsx);
	if (ret) {
		perror(_("FSGETXATTR"));
		goto fail;
	}

	if (!!(fsx.fsx_xflags & FS_XFLAG_REALTIME) != req->realtime) {
		if (req->realtime)
			fsx.fsx_xflags |= FS_XFLAG_REALTIME;
		else
			fsx.fsx_xflags &= ~FS_XFLAG_REALTIME;

		ret = ioctl(fd, FS_IOC_FSSETXATTR, &fsx);
		if (ret) {
			perror(_("FSSETXATTR"));
			goto fail;
		}
	}

	return fd;
fail:
	close(fd);
	return -1;
}

/* Extract fshandle from the open file. */
static int
extract_fshandle(
	struct clear_req	*req)
{
	void			*handle;
	size_t			handle_sz;
	int			ret;

	ret = fd_to_handle(file->xfd.fd, &handle, &handle_sz);
	if (ret)
		return ret;

	ret = handle_to_fshandle(handle, handle_sz, &req->fshandle,
			&req->fshandle_sz);
	if (ret)
		return ret;

	free_handle(handle, handle_sz);
	return 0;
}

static int
clearfree_f(
	int			argc,
	char			**argv)
{
	struct clear_req	req = { };
	long long		lnum;
	int			c, ret;

	while ((c = getopt(argc, argv, "drv")) != EOF) {
		switch (c) {
		case 'r':	/* rt device */
			req.realtime = true;
			break;
		case 'v':	/* Verbose output */
			vflag++;
			break;
		default:
			exitcode = 1;
			clearfree_help();
			return 0;
		}
	}

	if (argc != optind + 2) {
		clearfree_help();
		goto fail;
	}

	if (req.realtime) {
		if (file->xfd.fsgeom.rtblocks == 0) {
			fprintf(stderr, _("No realtime volume present.\n"));
			goto fail;
		}
		req.dev = file->fs_path.fs_rtdev;
	} else {
		req.dev = file->fs_path.fs_datadev;
	}

	lnum = cvtnum(file->xfd.fsgeom.blocksize, file->xfd.fsgeom.sectsize,
			argv[optind]);
	if (lnum < 0) {
		fprintf(stderr, _("Bad clearfree start sector %s.\n"),
				argv[optind]);
		goto fail;
	}
	req.start = lnum;

	lnum = cvtnum(file->xfd.fsgeom.blocksize, file->xfd.fsgeom.sectsize,
			argv[optind + 1]);
	if (lnum < 0) {
		fprintf(stderr, _("Bad clearfree length %s.\n"),
				argv[optind + 1]);
		goto fail;
	}
	req.length = lnum;

	ret = fstat(file->xfd.fd, &req.statbuf);
	if (ret) {
		perror(file->name);
		goto fail;
	}

	if (!S_ISDIR(req.statbuf.st_mode)) {
		errno = -ENOTDIR;
		perror(file->name);
		goto fail;
	}

	ret = extract_fshandle(&req);
	if (ret) {
		perror(file->name);
		goto fail;
	}

	req.temp_fd = open_temp_file(&file->xfd, &req, &req.temp_statbuf);
	if (req.temp_fd < 0)
		goto fail;

	req.space_fd = open_temp_file(&file->xfd, &req, &req.space_statbuf);
	if (req.space_fd < 0)
		goto fail;

	ret = bitmap_alloc(&req.visited);
	if (ret) {
		perror(_("allocating work bitmap"));
		goto fail;
	}

	ret = clear_space(&file->xfd, &req);
	if (ret)
		goto fail;

	bitmap_free(&req.visited);

	ret = close(req.space_fd);
	if (ret) {
		perror(_("closing space catcher file\n"));
		goto fail;
	}

	ret = close(req.temp_fd);
	if (ret) {
		perror(_("closing temp file\n"));
		goto fail;
	}

	free_handle(req.fshandle, req.fshandle_sz);
	fshandle_destroy();
	return 0;
fail:
	exitcode = 1;
	return 1;
}

static struct cmdinfo clearfree_cmd = {
	.name		= "clearfree",
	.cfunc		= clearfree_f,
	.argmin		= 0,
	.argmax		= -1,
	.flags		= CMD_FLAG_ONESHOT,
	.args		= "[-rv] start end",
	.help		= clearfree_help,
};

void
clearfree_init(void)
{
	clearfree_cmd.oneline = _("clear free space in the filesystem");

	add_command(&clearfree_cmd);
}
