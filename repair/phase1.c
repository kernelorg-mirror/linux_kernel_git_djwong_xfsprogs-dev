// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2001,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "libxfs.h"
#include "globals.h"
#include "agheader.h"
#include "protos.h"
#include "err_protos.h"

static void
no_sb(void)
{
	do_warn(_("Sorry, could not find valid secondary superblock\n"));
	do_warn(_("Exiting now.\n"));
	exit(1);
}

char *
alloc_ag_buf(int size)
{
	char	*bp;

	bp = (char *)memalign(libxfs_device_alignment(), size);
	if (!bp)
		do_error(_("could not allocate ag header buffer (%d bytes)\n"),
			size);
	return(bp);
}

static void
set_needsrepair(
	struct xfs_sb	*sb)
{
	if (!xfs_sb_version_hascrc(sb)) {
		printf(
	_("needsrepair flag only supported on V5 filesystems.\n"));
		exit(0);
	}

	if (xfs_sb_version_needsrepair(sb)) {
		printf(_("Filesystem already marked as needing repair.\n"));
		return;
	}

	printf(_("Marking filesystem in need of repair.\n"));
	primary_sb_modified = 1;
	sb->sb_features_incompat |= XFS_SB_FEAT_INCOMPAT_NEEDSREPAIR;
}

static void
set_inobtcount(
	struct xfs_sb	*sb)
{
	if (!xfs_sb_version_hascrc(sb)) {
		printf(
	_("Inode btree count feature only supported on V5 filesystems.\n"));
		exit(0);
	}

	if (!xfs_sb_version_hasfinobt(sb) && !add_finobt) {
		printf(
	_("Inode btree count feature requires free inode btree.\n"));
		exit(0);
	}

	if (xfs_sb_version_hasinobtcounts(sb)) {
		printf(_("Filesystem already has inode btree counts.\n"));
		return;
	}

	printf(_("Adding inode btree counts to filesystem.\n"));
	primary_sb_modified = 1;
	sb->sb_features_ro_compat |= XFS_SB_FEAT_RO_COMPAT_INOBTCNT;
	sb->sb_features_incompat |= XFS_SB_FEAT_INCOMPAT_NEEDSREPAIR;
}

static void
set_bigtime(
	struct xfs_sb	*sb)
{
	if (!xfs_sb_version_hascrc(sb)) {
		printf(
	_("Large timestamp feature only supported on V5 filesystems.\n"));
		exit(0);
	}

	if (xfs_sb_version_hasbigtime(sb)) {
		printf(_("Filesystem already supports large timestamps.\n"));
		return;
	}

	printf(_("Adding large timestamp support to filesystem.\n"));
	primary_sb_modified = 1;
	sb->sb_features_incompat |= (XFS_SB_FEAT_INCOMPAT_NEEDSREPAIR |
				     XFS_SB_FEAT_INCOMPAT_BIGTIME);
}

/*
 * this has got to be big enough to hold 4 sectors
 */
#define MAX_SECTSIZE		(512 * 1024)

/* ARGSUSED */
void
phase1(xfs_mount_t *mp)
{
	xfs_sb_t		*sb;
	char			*ag_bp;
	int			rval;

	do_log(_("Phase 1 - find and verify superblock...\n"));

	primary_sb_modified = 0;
	need_root_inode = 0;
	need_root_dotdot = 0;
	need_metadir_inode = 0;
	need_metadir_dotdot = 0;
	need_rbmino = 0;
	need_rsumino = 0;
	lost_quotas = 0;

	/*
	 * get AG 0 into ag header buf
	 */
	ag_bp = alloc_ag_buf(MAX_SECTSIZE);
	sb = (xfs_sb_t *) ag_bp;

	rval = get_sb(sb, 0LL, MAX_SECTSIZE, 0);
	if (rval == XR_EOF)
		do_error(_("error reading primary superblock\n"));

	/*
	 * is this really an sb, verify internal consistency
	 */
	if (rval != XR_OK)  {
		do_warn(_("bad primary superblock - %s !!!\n"),
			err_string(rval));
		if (!find_secondary_sb(sb))
			no_sb();
		primary_sb_modified = 1;
	} else if ((rval = verify_set_primary_sb(sb, 0,
					&primary_sb_modified)) != XR_OK)  {
		do_warn(_("couldn't verify primary superblock - %s !!!\n"),
			err_string(rval));
		if (!find_secondary_sb(sb))
			no_sb();
		primary_sb_modified = 1;
	}

	/*
	 * Check bad_features2 and make sure features2 the same as
	 * bad_features (ORing the two together). Leave bad_features2
	 * set so older kernels can still use it and not mount unsupported
	 * filesystems when it reads bad_features2.
	 */
	if (sb->sb_bad_features2 != 0 &&
			sb->sb_bad_features2 != sb->sb_features2) {
		sb->sb_features2 |= sb->sb_bad_features2;
		sb->sb_bad_features2 = sb->sb_features2;
		primary_sb_modified = 1;
		do_warn(_("superblock has a features2 mismatch, correcting\n"));
	}

	/*
	 * apply any version changes or conversions after the primary
	 * superblock has been verified or repaired
	 *
	 * Send output to stdout as do_log and everything else in repair
	 * is sent to stderr and there is no "quiet" option. xfs_admin
	 * will filter stderr but not stdout. This situation must be improved.
	 */
	if (convert_lazy_count) {
		if (lazy_count && !xfs_sb_version_haslazysbcount(sb)) {
			sb->sb_versionnum |= XFS_SB_VERSION_MOREBITSBIT;
			sb->sb_features2 |= XFS_SB_VERSION2_LAZYSBCOUNTBIT;
			sb->sb_bad_features2 |= XFS_SB_VERSION2_LAZYSBCOUNTBIT;
			primary_sb_modified = 1;
			printf(_("Enabling lazy-counters\n"));
		} else if (!lazy_count && xfs_sb_version_haslazysbcount(sb)) {
			if (XFS_SB_VERSION_NUM(sb) == XFS_SB_VERSION_5) {
				printf(
_("Cannot disable lazy-counters on V5 fs\n"));
				exit(1);
			}
			sb->sb_features2 &= ~XFS_SB_VERSION2_LAZYSBCOUNTBIT;
			sb->sb_bad_features2 &= ~XFS_SB_VERSION2_LAZYSBCOUNTBIT;
			printf(_("Disabling lazy-counters\n"));
			primary_sb_modified = 1;
		} else {
			printf(_("Lazy-counters are already %s\n"),
				lazy_count ? _("enabled") : _("disabled"));
			exit(0); /* no conversion required, exit */
		}
	}

	if (add_needsrepair)
		set_needsrepair(sb);
	if (add_inobtcount)
		set_inobtcount(sb);
	if (add_bigtime)
		set_bigtime(sb);

	/* shared_vn should be zero */
	if (sb->sb_shared_vn) {
		do_warn(_("resetting shared_vn to zero\n"));
		sb->sb_shared_vn = 0;
		primary_sb_modified = 1;
	}

	if (primary_sb_modified)  {
		if (!no_modify)  {
			do_warn(_("writing modified primary superblock\n"));
			write_primary_sb(sb, sb->sb_sectsize);
		} else  {
			do_warn(_("would write modified primary superblock\n"));
		}
	}

	/*
	 * misc. global var initialization
	 */
	sb_ifree = sb_icount = sb_fdblocks = sb_frextents = 0;

	/* Simulate a crash after setting needsrepair. */
	if (primary_sb_modified && add_needsrepair &&
	    abort_after_force_needsrepair)
		exit(55);

	free(sb);
}

/* Feature upgrades that require more comprehensive checks inside the fs. */

/* Make sure we can actually upgrade this (v5) filesystem. */
static void
check_new_v5_geometry(
	struct xfs_mount	*mp,
	struct xfs_sb		*new_sb)
{
	struct xfs_sb		old_sb;
	xfs_agnumber_t		agno;
	xfs_ino_t		rootino;
	int			min_logblocks;
	int			error;

	/*
	 * Save the current superblock, then copy in the new one to do log size
	 * and root inode checks.
	 */
	memcpy(&old_sb, &mp->m_sb, sizeof(struct xfs_sb));
	memcpy(&mp->m_sb, new_sb, sizeof(struct xfs_sb));

	/* Do we have a big enough log? */
	min_logblocks = libxfs_log_calc_minimum_size(mp);
	if (old_sb.sb_logblocks < min_logblocks) {
		printf(
	_("Filesystem log too small to upgrade filesystem; need %u blocks, have %u.\n"),
				min_logblocks, old_sb.sb_logblocks);
		exit(0);
	}

	rootino = libxfs_ialloc_calc_rootino(mp, new_sb->sb_unit);
	if (old_sb.sb_rootino != rootino) {
		printf(
	_("Cannot upgrade filesystem, root inode (%llu) cannot be moved to %llu.\n"),
				(unsigned long long)old_sb.sb_rootino,
				(unsigned long long)rootino);
		exit(0);
	}

	/* Put back the old super so that we can read AG headers. */
	memcpy(&mp->m_sb, &old_sb, sizeof(struct xfs_sb));

	/* Make sure we have enough space for per-AG reservations. */
	for (agno = 0; agno < mp->m_sb.sb_agcount; agno++) {
		struct xfs_trans	*tp;
		struct xfs_agf		*agf;
		struct xfs_buf		*agi_bp, *agf_bp;
		struct xfs_perag	*pag;
		unsigned int		avail, agblocks;

		pag = libxfs_perag_get(mp, agno);

		/*
		 * Create a dummy transaction so that we can load the AGI and
		 * AGF buffers in memory with the old fs geometry and pin them
		 * there while we try to make a per-AG reservation with the new
		 * geometry.
		 */
		error = -libxfs_trans_alloc_empty(mp, &tp);
		if (error)
			do_error(
	_("Cannot reserve resources for upgrade check, err=%d.\n"),
					error);

		error = -libxfs_ialloc_read_agi(mp, tp, agno, &agi_bp);
		if (error)
			do_error(
	_("Cannot read AGI %u for upgrade check, err=%d.\n"),
					agno, error);

		error = -libxfs_alloc_read_agf(mp, tp, agno, 0, &agf_bp);
		if (error)
			do_error(
	_("Cannot read AGF %u for upgrade check, err=%d.\n"),
					agno, error);
		agf = agf_bp->b_addr;
		agblocks = be32_to_cpu(agf->agf_length);

		/*
		 * Install the new superblock and try to make a per-AG space
		 * reservation with the new geometry.  We pinned the AG header
		 * buffers to the transaction, so we shouldn't hit any
		 * corruption errors on account of the new geometry.
		 */
		memcpy(&mp->m_sb, new_sb, sizeof(struct xfs_sb));
		error = -libxfs_ag_resv_init(pag, tp);
		if (error == ENOSPC) {
			printf(
	_("Not enough free space would remain in AG %u for metadata.\n"),
					agno);
			exit(0);
		}
		if (error)
			do_error(
	_("Error %d while checking AG %u space reservation.\n"),
					error, agno);

		/*
		 * Would we have at least 10% free space in this AG after
		 * making per-AG reservations?
		 */
		avail = pag->pagf_freeblks + pag->pagf_flcount;
		avail -= pag->pag_meta_resv.ar_reserved;
		avail -= pag->pag_rmapbt_resv.ar_asked;
		if (avail < agblocks / 10)
			printf(
	_("Not enough free space would remain in AG %u after upgrade.\n"),
					agno);

		error = -libxfs_ag_resv_free(pag);
		if (error)
			do_error(
	_("Error %d while cleaning up after AG %u space reservation.\n"),
					error, agno);

		/*
		 * Put back the old superblock, mark the per-AG structure as
		 * uninitialized so that we don't trip over stale cached
		 * counters after the upgrade, and release all the resources.
		 */
		memcpy(&mp->m_sb, &old_sb, sizeof(struct xfs_sb));
		libxfs_trans_cancel(tp);
		pag->pagf_init = 0;
		pag->pagi_init = 0;
		libxfs_perag_put(pag);
	}
}

static bool
set_finobt(
	struct xfs_mount	*mp,
	struct xfs_sb		*new_sb)
{
	if (!xfs_sb_version_hascrc(&mp->m_sb)) {
		printf(
	_("Free inode btree feature only supported on V5 filesystems.\n"));
		exit(0);
	}

	if (xfs_sb_version_hasfinobt(&mp->m_sb)) {
		printf(_("Filesystem already supports free inode btrees.\n"));
		return false;
	}

	printf(_("Adding free inode btrees to filesystem.\n"));
	new_sb->sb_features_ro_compat |= XFS_SB_FEAT_RO_COMPAT_FINOBT;
	new_sb->sb_features_incompat |= XFS_SB_FEAT_INCOMPAT_NEEDSREPAIR;
	return true;
}

static bool
set_reflink(
	struct xfs_mount	*mp,
	struct xfs_sb		*new_sb)
{
	if (!xfs_sb_version_hascrc(&mp->m_sb)) {
		printf(
	_("Reflink feature only supported on V5 filesystems.\n"));
		exit(0);
	}

	if (xfs_sb_version_hasreflink(&mp->m_sb)) {
		printf(_("Filesystem already supports reflink.\n"));
		return false;
	}

	printf(_("Adding reflink support to filesystem.\n"));
	new_sb->sb_features_ro_compat |= XFS_SB_FEAT_RO_COMPAT_REFLINK;
	new_sb->sb_features_incompat |= XFS_SB_FEAT_INCOMPAT_NEEDSREPAIR;
	return true;
}

static bool
set_rmapbt(
	struct xfs_mount	*mp,
	struct xfs_sb		*new_sb)
{
	if (!xfs_sb_version_hascrc(&mp->m_sb)) {
		printf(
	_("Reverse mapping btree feature only supported on V5 filesystems.\n"));
		exit(0);
	}

	if (xfs_sb_version_hasreflink(&mp->m_sb)) {
		printf(
	_("Reverse mapping btrees cannot be added when reflink is enabled.\n"));
		exit(0);
	}

	if (xfs_sb_version_hasfinobt(&mp->m_sb)) {
		printf(_("Filesystem already supports reverse mapping btrees.\n"));
		return false;
	}

	printf(_("Adding reverse mapping btrees to filesystem.\n"));
	new_sb->sb_features_ro_compat |= XFS_SB_FEAT_RO_COMPAT_RMAPBT;
	new_sb->sb_features_incompat |= XFS_SB_FEAT_INCOMPAT_NEEDSREPAIR;
	return true;
}

/*
 * Now that we have the xfs mount fully set up, we can perform upgrades that
 * require us to check the state of other parts of the filesystem before
 * proceeding.
 */
void
phase1a(
	struct xfs_mount	*mp)
{
	struct xfs_sb		new_sb;
	struct xfs_buf		*bp;
	bool			additions = false;
	bool			upgrade = false;
	int			error;

	memcpy(&new_sb, &mp->m_sb, sizeof(struct xfs_sb));

	if (add_finobt) {
		additions = true;
		upgrade |= set_finobt(mp, &new_sb);
	}
	if (add_reflink) {
		additions = true;
		upgrade |= set_reflink(mp, &new_sb);
	}
	if (add_rmapbt) {
		additions = true;
		upgrade |= set_rmapbt(mp, &new_sb);
	}

	if (!additions)
		return;
	if (!upgrade)
		exit(0);

	check_new_v5_geometry(mp, &new_sb);
	memcpy(&mp->m_sb, &new_sb, sizeof(struct xfs_sb));
	if (no_modify)
		return;

	bp = libxfs_getsb(mp);
	if (!bp || bp->b_error) {
		do_error(_("couldn't get superblock for v5 upgrade, err=%d\n"),
				bp ? bp->b_error : ENOMEM);
	} else {
		libxfs_sb_to_disk(bp->b_addr, &mp->m_sb);

		/*
		 * Write the new super to disk along with the needsrepair flag,
		 * if the upgrade warrants it.
		 */
		error = -libxfs_bwrite(bp);
		if (error)
			do_error(_("v5 feature upgrade failed, err=%d\n"),
					error);
	}
	if (bp)
		libxfs_buf_relse(bp);
}
