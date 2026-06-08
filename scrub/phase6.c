// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2018-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include <stdint.h>
#include <dirent.h>
#include <sys/statvfs.h>
#include <linux/fsmap.h>
#include "handle.h"
#include "libfrog/paths.h"
#include "libfrog/workqueue.h"
#include "xfs_scrub.h"
#include "common.h"
#include "libfrog/bitmap.h"
#include "disk.h"
#include "filemap.h"
#include "fscounters.h"
#include "inodes.h"
#include "read_verify.h"
#include "spacemap.h"
#include "vfs.h"
#include "common.h"
#include "libfrog/bulkstat.h"
#include "libfrog/ptvar.h"

/*
 * Phase 6: Verify data file integrity.
 *
 * Identify potential data block extents with GETFSMAP, then feed those
 * extents to the read-verify pool to get the verify commands batched,
 * issued, and (if there are problems) reported back to us.  If there
 * are errors, we'll record the bad regions and (if available) use rmap
 * to tell us if metadata are now corrupt.  Otherwise, we'll scan the
 * whole directory tree looking for files that overlap the bad regions
 * and report the paths of the now corrupt files.
 */

/* Verify disk blocks with GETFSMAP */

struct media_verify_state {
	struct ptvar		*verify_schedules;

	struct read_verify_pool	*rvp[XFS_DEV_RT + 1];
};

struct disk_ioerr_report {
	struct scrub_ctx	*ctx;
	enum xfs_device		dev;
};

struct owner_decode {
	uint64_t		owner;
	const char		*descr;
};

static const struct owner_decode special_owners[] = {
	{XFS_FMR_OWN_FREE,	N_("free space")},
	{XFS_FMR_OWN_UNKNOWN,	N_("unknown owner")},
	{XFS_FMR_OWN_FS,	N_("static FS metadata")},
	{XFS_FMR_OWN_LOG,	N_("journalling log")},
	{XFS_FMR_OWN_AG,	N_("per-AG metadata")},
	{XFS_FMR_OWN_INOBT,	N_("inode btree blocks")},
	{XFS_FMR_OWN_INODES,	N_("inodes")},
	{XFS_FMR_OWN_REFC,	N_("refcount btree")},
	{XFS_FMR_OWN_COW,	N_("CoW staging")},
	{XFS_FMR_OWN_DEFECTIVE,	N_("bad blocks")},
	{0, NULL},
};

/* Decode a special owner. */
static const char *
decode_special_owner(
	uint64_t			owner)
{
	const struct owner_decode	*od = special_owners;

	while (od->descr) {
		if (od->owner == owner)
			return _(od->descr);
		od++;
	}

	return NULL;
}

/* Routines to translate bad physical extents into file paths and offsets. */

struct badfile_report {
	struct scrub_ctx		*ctx;
	const char			*descr;
	struct media_verify_state	*vs;
	struct file_bmap		*bmap;
};

/* Report on bad extents found during a media scan. */
static int
report_badfile(
	uint64_t		start,
	uint64_t		length,
	void			*arg)
{
	struct badfile_report	*br = arg;
	unsigned long long	bad_offset;
	unsigned long long	bad_length;

	/* Clamp the bad region to the file mapping. */
	if (start < br->bmap->bm_physical) {
		length -= br->bmap->bm_physical - start;
		start = br->bmap->bm_physical;
	}
	length = min(length, br->bmap->bm_length);

	/* Figure out how far into the bmap is the bad mapping and report it. */
	bad_offset = start - br->bmap->bm_physical;
	bad_length = min(start + length,
			 br->bmap->bm_physical + br->bmap->bm_length) - start;

	str_unfixable_error(br->ctx, br->descr,
_("media error at data offset %llu length %llu."),
			br->bmap->bm_offset + bad_offset, bad_length);
	return 0;
}

static inline enum xfs_device from_fsx(const struct fsxattr *fsx)
{
	if (fsx->fsx_xflags & FS_XFLAG_REALTIME)
		return XFS_DEV_RT;
	return XFS_DEV_DATA;
}

/* Report if this extent overlaps a bad region. */
static int
report_data_loss(
	struct scrub_ctx		*ctx,
	int				fd,
	int				whichfork,
	struct fsxattr			*fsx,
	struct file_bmap		*bmap,
	void				*arg)
{
	struct badfile_report		*br = arg;
	struct media_verify_state	*vs = br->vs;

	br->bmap = bmap;

	/* Only report errors for real extents. */
	if (bmap->bm_flags & (BMV_OF_PREALLOC | BMV_OF_DELALLOC))
		return 0;

	return read_verify_iterate_failed_range(vs->rvp[from_fsx(fsx)],
			bmap->bm_physical, bmap->bm_length, report_badfile,
			br);
}

/* Report if the extended attribute data overlaps a bad region. */
static int
report_attr_loss(
	struct scrub_ctx		*ctx,
	int				fd,
	int				whichfork,
	struct fsxattr			*fsx,
	struct file_bmap		*bmap,
	void				*arg)
{
	struct badfile_report		*br = arg;
	struct media_verify_state	*vs = br->vs;

	/* Complain about attr fork extents that don't look right. */
	if (bmap->bm_flags & (BMV_OF_PREALLOC | BMV_OF_DELALLOC)) {
		str_info(ctx, br->descr,
_("found unexpected unwritten/delalloc attr fork extent."));
		return 0;
	}

	if (fsx->fsx_xflags & FS_XFLAG_REALTIME) {
		str_info(ctx, br->descr,
_("found unexpected realtime attr fork extent."));
		return 0;
	}

	if (read_verify_has_failed(vs->rvp[XFS_DEV_DATA], bmap->bm_physical,
				bmap->bm_length))
		str_corrupt(ctx, br->descr,
_("media error in extended attribute data."));

	return 0;
}

/* Iterate the extent mappings of a file to report errors. */
static int
report_fd_loss(
	struct scrub_ctx		*ctx,
	const char			*descr,
	int				fd,
	void				*arg)
{
	struct badfile_report		br = {
		.ctx			= ctx,
		.vs			= arg,
		.descr			= descr,
	};
	struct file_bmap		key = {0};
	int				ret;

	/* data fork */
	ret = scrub_iterate_filemaps(ctx, fd, XFS_DATA_FORK, &key,
			report_data_loss, &br);
	if (ret) {
		str_liberror(ctx, ret, descr);
		return ret;
	}

	/* attr fork */
	ret = scrub_iterate_filemaps(ctx, fd, XFS_ATTR_FORK, &key,
			report_attr_loss, &br);
	if (ret) {
		str_liberror(ctx, ret, descr);
		return ret;
	}

	return 0;
}

/* Report read verify errors in unlinked (but still open) files. */
static int
report_inode_loss(
	struct scrub_ctx		*ctx,
	struct xfs_handle		*handle,
	struct xfs_bulkstat		*bstat,
	void				*arg)
{
	char				descr[DESCR_BUFSZ];
	int				fd;
	int				error, err2;

	/* Ignore linked files and things we can't open. */
	if (bstat->bs_nlink != 0)
		return 0;
	if (!S_ISREG(bstat->bs_mode) && !S_ISDIR(bstat->bs_mode))
		return 0;

	scrub_render_ino_descr(ctx, descr, DESCR_BUFSZ,
			bstat->bs_ino, bstat->bs_gen, _("(unlinked)"));

	/* Try to open the inode. */
	fd = scrub_open_handle(handle);
	if (fd < 0) {
		/* Handle is stale, try again. */
		if (errno == ESTALE)
			return ESTALE;

		str_error(ctx, descr,
 _("Could not open to report read errors: %s."),
				strerror(errno));
		return 0;
	}

	/* Go find the badness. */
	error = report_fd_loss(ctx, descr, fd, arg);

	err2 = close(fd);
	if (err2)
		str_errno(ctx, descr);

	return error;
}

/* Scan a directory for matches in the read verify error list. */
static int
report_dir_loss(
	struct scrub_ctx	*ctx,
	const char		*path,
	int			dir_fd,
	void			*arg)
{
	return report_fd_loss(ctx, path, dir_fd, arg);
}

/*
 * Scan the inode associated with a directory entry for matches with
 * the read verify error list.
 */
static int
report_dirent_loss(
	struct scrub_ctx	*ctx,
	const char		*path,
	int			dir_fd,
	struct dirent		*dirent,
	struct stat		*sb,
	void			*arg)
{
	int			fd;
	int			error, err2;

	/* Ignore things we can't open. */
	if (!S_ISREG(sb->st_mode) && !S_ISDIR(sb->st_mode))
		return 0;

	/* Ignore . and .. */
	if (!strcmp(".", dirent->d_name) || !strcmp("..", dirent->d_name))
		return 0;

	/*
	 * If we were given a dirent, open the associated file under
	 * dir_fd for badblocks scanning.  If dirent is NULL, then it's
	 * the directory itself we want to scan.
	 */
	fd = openat(dir_fd, dirent->d_name,
			O_RDONLY | O_NOATIME | O_NOFOLLOW | O_NOCTTY);
	if (fd < 0) {
		char		descr[PATH_MAX + 1];

		if (errno == ENOENT)
			return 0;

		snprintf(descr, PATH_MAX, "%s/%s", path, dirent->d_name);
		descr[PATH_MAX] = 0;

		str_error(ctx, descr,
 _("Could not open to report read errors: %s."),
				strerror(errno));
		return 0;
	}

	/* Go find the badness. */
	error = report_fd_loss(ctx, path, fd, arg);

	err2 = close(fd);
	if (err2)
		str_errno(ctx, path);
	if (!error && err2)
		error = err2;

	return error;
}

struct ioerr_filerange {
	uint64_t		physical;
	uint64_t		length;
};

/*
 * If reverse mapping and parent pointers are enabled, we can map media errors
 * directly back to a filename and a file position without needing to walk the
 * directory tree.
 */
static inline bool
can_use_pptrs(
	const struct scrub_ctx	*ctx)
{
	return  (ctx->mnt.fsgeom.flags & XFS_FSOP_GEOM_FLAGS_PARENT) &&
		(ctx->mnt.fsgeom.flags & XFS_FSOP_GEOM_FLAGS_RMAPBT);
}

/* Use a fsmap to report metadata lost to a media error. */
static int
report_ioerr_fsmap(
	struct scrub_ctx	*ctx,
	struct fsmap		*map,
	void			*arg)
{
	const char		*type;
	struct xfs_bulkstat	bs = { };
	char			buf[DESCR_BUFSZ];
	struct ioerr_filerange	*fr = arg;
	uint64_t		err_off;
	uint64_t		err_len;
	int			ret;

	/* Don't care about unwritten extents. */
	if (map->fmr_flags & FMR_OF_PREALLOC)
		return 0;

	if (fr->physical > map->fmr_physical)
		err_off = fr->physical - map->fmr_physical;
	else
		err_off = 0;

	/* Report special owners */
	if (map->fmr_flags & FMR_OF_SPECIAL_OWNER) {
		snprintf(buf, DESCR_BUFSZ, _("disk offset %"PRIu64),
				(uint64_t)map->fmr_physical + err_off);
		type = decode_special_owner(map->fmr_owner);
		/*
		 * On filesystems that don't store reverse mappings, the
		 * GETFSMAP call returns OWNER_UNKNOWN for allocated space.
		 * We'll have to let the directory tree walker find the file
		 * that lost data.
		 */
		if (!(ctx->mnt.fsgeom.flags & XFS_FSOP_GEOM_FLAGS_RMAPBT) &&
		    map->fmr_owner == XFS_FMR_OWN_UNKNOWN) {
			str_info(ctx, buf, _("media error detected."));
		} else {
			str_corrupt(ctx, buf, _("media error in %s."), type);
		}

		return 0;
	}

	if (can_use_pptrs(ctx)) {
		ret = -xfrog_bulkstat_single(&ctx->mnt, map->fmr_owner, 0, &bs);
		if (ret)
			str_liberror(ctx, ret,
					_("bulkstat for media error report"));
	}

	/* Report extent maps */
	if (map->fmr_flags & FMR_OF_EXTENT_MAP) {
		bool		attr = (map->fmr_flags & FMR_OF_ATTR_FORK);

		scrub_render_ino_descr(ctx, buf, DESCR_BUFSZ,
				map->fmr_owner, bs.bs_gen, " %s",
				attr ? _("extended attribute") :
				       _("file data"));
		str_corrupt(ctx, buf, _("media error in extent map"));

		return 0;
	}

	/*
	 * If directory parent pointers are available, use that to find the
	 * pathname to a file, and report that path as having lost its
	 * extended attributes, or the precise offset of the lost file data.
	 */
	if (!can_use_pptrs(ctx))
		return 0;

	scrub_render_ino_descr(ctx, buf, DESCR_BUFSZ, map->fmr_owner,
			bs.bs_gen, NULL);

	if (map->fmr_flags & FMR_OF_ATTR_FORK) {
		str_corrupt(ctx, buf, _("media error in extended attributes"));
		return 0;
	}

	err_len = min(fr->physical + fr->length,
		      map->fmr_physical + map->fmr_length) -
		  max(fr->physical, map->fmr_physical);
	str_unfixable_error(ctx, buf,
 _("media error at data offset %llu length %llu."),
			map->fmr_offset + err_off, err_len);
	return 0;
}

/*
 * For a range of bad blocks, visit each space mapping that overlaps the bad
 * range so that we can report lost metadata.
 */
static int
report_ioerr(
	uint64_t			start,
	uint64_t			length,
	void				*arg)
{
	struct fsmap			keys[2] = { };
	struct ioerr_filerange		fr = {
		.physical		= start,
		.length			= length,
	};
	struct disk_ioerr_report	*dioerr = arg;

	/* Go figure out which blocks are bad from the fsmap. */
	keys[0].fmr_device = to_fsmap_dev(dioerr->ctx, dioerr->dev);
	keys[0].fmr_physical = start;
	keys[1].fmr_device = keys[0].fmr_device;
	keys[1].fmr_physical = start + length - 1;
	keys[1].fmr_owner = ULLONG_MAX;
	keys[1].fmr_offset = ULLONG_MAX;
	keys[1].fmr_flags = UINT_MAX;
	return -scrub_iterate_fsmap(dioerr->ctx, keys, report_ioerr_fsmap,
			&fr);
}

static inline const char *trunc_msg(enum xfs_device dev)
{
	switch (dev) {
	case XFS_DEV_DATA:
		return _("data device truncated");
	case XFS_DEV_LOG:
		return _("log device truncated");
	case XFS_DEV_RT:
		return _("rt device truncated");
	}
	abort();
}

/* Report all the media errors found on a disk. */
static int
report_disk_ioerrs(
	struct scrub_ctx		*ctx,
	struct media_verify_state	*vs,
	enum xfs_device			dev)
{
	struct disk_ioerr_report	dioerr = {
		.ctx			= ctx,
		.dev			= dev,
	};

	if (!vs->rvp[dev])
		return 0;

	if (read_verify_truncated(vs->rvp[dev]))
		str_corrupt(ctx, ctx->mntpoint, trunc_msg(dev));

	return read_verify_iterate_failed(vs->rvp[dev], report_ioerr, &dioerr);
}

/* Given bad extent lists for the data & rtdev, find bad files. */
static int
report_all_media_errors(
	struct scrub_ctx		*ctx,
	struct media_verify_state	*vs)
{
	int				ret;

	ret = report_disk_ioerrs(ctx, vs, XFS_DEV_DATA);
	if (ret) {
		str_liberror(ctx, ret, _("walking data device io errors"));
		return ret;
	}

	ret = report_disk_ioerrs(ctx, vs, XFS_DEV_LOG);
	if (ret) {
		str_liberror(ctx, ret, _("walking log device io errors"));
		return ret;
	}

	ret = report_disk_ioerrs(ctx, vs, XFS_DEV_RT);
	if (ret) {
		str_liberror(ctx, ret, _("walking rt device io errors"));
		return ret;
	}

	/*
	 * Scan the directory tree to get file paths if we didn't already use
	 * directory parent pointers to report the loss.  If parent pointers
	 * are enabled, report_ioerr_fsmap will have already reported file
	 * paths that have lost file data and xattrs.
	 */
	if (can_use_pptrs(ctx))
		return 0;

	ret = scan_fs_tree(ctx, report_dir_loss, report_dirent_loss, vs);
	if (ret)
		return ret;

	/* Scan for unlinked files. */
	return scrub_scan_user_files(ctx, report_inode_loss, vs);
}

/* Schedule a read-verify of a (data block) extent. */
static int
check_rmap(
	struct scrub_ctx		*ctx,
	struct fsmap			*map,
	void				*arg)
{
	struct media_verify_state	*vs = arg;
	struct read_verify_pool		*rvp =
		vs->rvp[from_fsmap_dev(ctx, map->fmr_device)];
	struct read_verify_schedule	*rs;
	bool				scheduled;
	int				ret;

	dbg_printf("rmap dev %d:%d phys %"PRIu64" owner %"PRId64
			" offset %"PRIu64" len %"PRIu64" flags 0x%x\n",
			major(map->fmr_device), minor(map->fmr_device),
			(uint64_t)map->fmr_physical, (int64_t)map->fmr_owner,
			(uint64_t)map->fmr_offset, (uint64_t)map->fmr_length,
			map->fmr_flags);

	/* "Unknown" extents should be verified; they could be data. */
	if ((map->fmr_flags & FMR_OF_SPECIAL_OWNER)) {
		switch (map->fmr_owner) {
		case XFS_FMR_OWN_UNKNOWN:
		case XFS_FMR_OWN_LOG:
			map->fmr_flags &= ~FMR_OF_SPECIAL_OWNER;
			break;
		}
	}

	/*
	 * We only care about read-verifying data extents that have been
	 * written to disk.  This means we can skip "special" owners
	 * (metadata), xattr blocks, unwritten extents, and extent maps.
	 * These should all get checked elsewhere in the scrubber.
	 */
	if (map->fmr_flags & (FMR_OF_PREALLOC | FMR_OF_ATTR_FORK |
			      FMR_OF_EXTENT_MAP | FMR_OF_SPECIAL_OWNER))
		return 0;

	/* XXX: Filter out directory data blocks. */

	rs = ptvar_get(vs->verify_schedules, &ret);
	if (ret) {
		str_liberror(ctx, -ret, _("grabbing media verify schedule"));
		return -ret;
	}

	/* Schedule the read verify command for (eventual) running. */
	scheduled = try_read_verify_schedule_io(rs, rvp, map->fmr_physical,
			map->fmr_length);
	if (scheduled)
		return 0;

	ret = read_verify_schedule_now(rs);
	if (ret) {
		str_liberror(ctx, ret, _("scheduling media verify command"));
		return ret;
	}

	scheduled = try_read_verify_schedule_io(rs, rvp, map->fmr_physical,
			map->fmr_length);
	assert(scheduled);
	return 0;
}

/* Initiate any scheduled verifications now. */
static int
force_one_verify(
	struct ptvar			*ptv,
	void				*data,
	void				*foreach_arg)
{
	struct read_verify_schedule	*rs = data;

	return -read_verify_schedule_now(rs);
}

/* Wait for read/verify actions to finish, then return # bytes checked. */
static int
clean_pool(
	struct media_verify_state	*vs,
	enum xfs_device			dev,
	unsigned long long		*bytes_checked,
	bool				*ok)
{
	struct read_verify_pool		*rvp = vs->rvp[dev];
	int				ret;

	if (!rvp)
		return 0;

	ret = read_verify_pool_flush(rvp);
	if (ret)
		return ret;

	*bytes_checked += read_verify_progress(rvp);
	if (!read_verify_ok(rvp))
		*ok = false;
	return 0;
}

static inline int
alloc_pool(
	struct scrub_ctx		*ctx,
	struct media_verify_state	*vs,
	enum xfs_device			dev)
{
	return read_verify_pool_alloc(ctx, dev, &vs->rvp[dev]);
}

static inline void
free_pool(
	struct media_verify_state	*vs,
	enum xfs_device			dev)
{
	if (vs->rvp[dev]) {
		read_verify_pool_abort(vs->rvp[dev]);
		read_verify_pool_destroy(vs->rvp[dev]);
	}
}

/*
 * Read verify all the file data blocks in a filesystem.  Since XFS doesn't
 * do data checksums, we trust that the underlying storage will pass back
 * an IO error if it can't retrieve whatever we previously stored there.
 * If we hit an IO error, we'll record the bad blocks in a bitmap and then
 * scan the extent maps of the entire fs tree to figure (and the unlinked
 * inodes) out which files are now broken.
 */
int
phase6_func(
	struct scrub_ctx		*ctx)
{
	struct media_verify_state	vs = { NULL };
	bool				ok = true;
	int				ret, ret2, ret3;

	ret = alloc_pool(ctx, &vs, XFS_DEV_DATA);
	if (ret) {
		str_liberror(ctx, ret, _("creating data device media verifier"));
		return ret;
	}
	if (ctx->fsinfo.fs_log) {
		ret = alloc_pool(ctx, &vs, XFS_DEV_LOG);
		if (ret) {
			str_liberror(ctx, ret,
					_("creating log device media verifier"));
			goto out_datapool;
		}
	}
	if (ctx->fsinfo.fs_rt || ctx->mnt.fsgeom.rtstart) {
		ret = alloc_pool(ctx, &vs, XFS_DEV_RT);
		if (ret) {
			str_liberror(ctx, ret,
					_("creating rt device media verifier"));
			goto out_logpool;
		}
	}

	ret = -ptvar_alloc(scrub_scan_spacemaps_nproc(ctx),
			sizeof(struct read_verify_schedule), NULL,
			&vs.verify_schedules);
	if (ret)
		goto out_rtpool;

	ret = scrub_scan_all_spacemaps(ctx, check_rmap, &vs);
	if (ret)
		goto out_schedules;

	ret = -ptvar_foreach(vs.verify_schedules, force_one_verify, NULL);
	if (ret) {
		str_liberror(ctx, ret, _("flushing read verify commands"));
		goto out_schedules;
	}
	ptvar_free(vs.verify_schedules);
	vs.verify_schedules = NULL;

	ret = clean_pool(&vs, XFS_DEV_DATA, &ctx->bytes_checked, &ok);
	if (ret)
		str_liberror(ctx, ret, _("flushing data device verify pool"));

	ret2 = clean_pool(&vs, XFS_DEV_LOG, &ctx->bytes_checked, &ok);
	if (ret2)
		str_liberror(ctx, ret2, _("flushing log device verify pool"));

	ret3 = clean_pool(&vs, XFS_DEV_RT, &ctx->bytes_checked, &ok);
	if (ret3)
		str_liberror(ctx, ret3, _("flushing rt device verify pool"));

	/*
	 * If the verify flush didn't work or we found no bad blocks, we're
	 * done!  No errors detected.
	 */
	if (ret || ret2 || ret3 || ok) {
		ret |= ret2 | ret3; /* caller only cares about non-zero/zero */
		goto out_rtpool;
	}

	/* Scan the whole dir tree to see what matches the bad extents. */
	ret = report_all_media_errors(ctx, &vs);
	goto out_rtpool;

out_schedules:
	ptvar_free(vs.verify_schedules);
out_rtpool:
	free_pool(&vs, XFS_DEV_RT);
out_logpool:
	free_pool(&vs, XFS_DEV_LOG);
out_datapool:
	free_pool(&vs, XFS_DEV_DATA);
	return ret;
}

/* Estimate how much work we're going to do. */
int
phase6_estimate(
	struct scrub_ctx	*ctx,
	uint64_t		*items,
	unsigned int		*nr_threads,
	int			*rshift)
{
	unsigned long long	d_blocks;
	unsigned long long	d_bfree;
	unsigned long long	r_blocks;
	unsigned long long	r_bfree;
	unsigned long long	dontcare;
	unsigned long long	l_blocks;
	int			ret;

	ret = scrub_scan_estimate_blocks(ctx, &d_blocks, &d_bfree, &r_blocks,
			&r_bfree, &dontcare, &l_blocks);
	if (ret) {
		str_liberror(ctx, ret, _("estimating verify work"));
		return ret;
	}

	*items = cvt_off_fsb_to_b(&ctx->mnt,
			(d_blocks - d_bfree) + (r_blocks - r_bfree));

	/* count external logs now that we scan them */
	if (ctx->fsinfo.fs_log)
		*items += cvt_off_fsb_to_b(&ctx->mnt, l_blocks);

	/*
	 * Each read-verify pool starts a thread pool, and each worker thread
	 * can contribute to the progress counter.  Hence we need to set
	 * nr_threads appropriately to handle that many threads.
	 */
	*nr_threads = read_verify_nproc(ctx);
	if (ctx->fsinfo.fs_rt || ctx->mnt.fsgeom.rtstart)
		*nr_threads += read_verify_nproc(ctx);
	if (ctx->fsinfo.fs_log)
		*nr_threads += read_verify_nproc(ctx);
	*rshift = 20;
	return 0;
}
