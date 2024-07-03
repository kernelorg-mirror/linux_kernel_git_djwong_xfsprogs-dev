// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2001,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "libxfs.h"
#include "avl.h"
#include "globals.h"
#include "agheader.h"
#include "incore.h"
#include "dinode.h"
#include "protos.h"
#include "err_protos.h"
#include "rt.h"

/* Computed rt bitmap/summary data */
static union xfs_rtword_raw	*btmcompute;
static union xfs_suminfo_raw	*sumcompute;

static inline void
set_rtword(
	struct xfs_mount	*mp,
	union xfs_rtword_raw	*word,
	xfs_rtword_t		value)
{
	if (xfs_has_rtgroups(mp))
		word->rtg = cpu_to_be32(value);
	else
		word->old = value;
}

static inline void
inc_sumcount(
	struct xfs_mount	*mp,
	union xfs_suminfo_raw	*info,
	xfs_rtsumoff_t		index)
{
	union xfs_suminfo_raw	*p = info + index;

	if (xfs_has_rtgroups(mp))
		be32_add_cpu(&p->rtg, 1);
	else
		p->old++;
}

/*
 * generate the real-time bitmap and summary info based on the
 * incore realtime extent map.
 */
void
generate_rtinfo(
	struct xfs_mount	*mp)
{
	unsigned int		bitsperblock =
		mp->m_blockwsize << XFS_NBWORDLOG;
	xfs_rtxnum_t		extno = 0;
	xfs_rtxnum_t		start_ext = 0;
	int			bmbno = 0;
	int			start_bmbno = 0;
	bool			in_extent = false;
	union xfs_rtword_raw	*words;

	btmcompute = calloc(libxfs_rtbitmap_wordcount(mp, mp->m_sb.sb_rextents),
			sizeof(union xfs_rtword_raw));
	if (!btmcompute)
		do_error(
_("couldn't allocate memory for incore realtime bitmap.\n"));
	words = btmcompute;

	sumcompute = calloc(libxfs_rtsummary_wordcount(mp, mp->m_rsumlevels,
			mp->m_sb.sb_rbmblocks), sizeof(union xfs_suminfo_raw));
	if (!sumcompute)
		do_error(
_("couldn't allocate memory for incore realtime summary info.\n"));

	ASSERT(mp->m_rbmip == NULL);

	/*
	 * Slower but simple, don't play around with trying to set things one
	 * word at a time, just set bit as required.  Have to track start and
	 * end (size) of each range of free extents to set the summary info
	 * properly.
	 */
	while (extno < mp->m_sb.sb_rextents)  {
		xfs_rtword_t		freebit = 1;
		xfs_rtword_t		bits = 0;
		int			i;

		set_rtword(mp, words, 0);
		for (i = 0; i < sizeof(xfs_rtword_t) * NBBY &&
				extno < mp->m_sb.sb_rextents; i++, extno++)  {
			if (get_rtbmap(extno) == XR_E_FREE)  {
				sb_frextents++;
				bits |= freebit;

				if (!in_extent) {
					start_ext = extno;
					start_bmbno = bmbno;
					in_extent = true;
				}
			} else if (in_extent) {
				uint64_t	len = extno - start_ext;
				xfs_rtsumoff_t	offs;

				offs = xfs_rtsumoffs(mp, libxfs_highbit64(len),
						start_bmbno);
				inc_sumcount(mp, sumcompute, offs);
				in_extent = false;
			}

			freebit <<= 1;
		}
		set_rtword(mp, words, bits);
		words++;

		if (extno % bitsperblock == 0)
			bmbno++;
	}

	if (in_extent) {
		uint64_t	len = extno - start_ext;
		xfs_rtsumoff_t	offs;

		offs = xfs_rtsumoffs(mp, libxfs_highbit64(len), start_bmbno);
		inc_sumcount(mp, sumcompute, offs);
	}

	if (mp->m_sb.sb_frextents != sb_frextents) {
		do_warn(_("sb_frextents %" PRIu64 ", counted %" PRIu64 "\n"),
				mp->m_sb.sb_frextents, sb_frextents);
	}
}

static void
check_rtwords(
	struct xfs_mount	*mp,
	const char		*filename,
	unsigned long long	bno,
	void			*ondisk,
	void			*incore)
{
	unsigned int		wordcnt = mp->m_blockwsize;
	union xfs_rtword_raw	*o = ondisk, *i = incore;
	int			badstart = -1;
	unsigned int		j;

	if (memcmp(ondisk, incore, wordcnt << XFS_WORDLOG) == 0)
		return;

	for (j = 0; j < wordcnt; j++, o++, i++) {
		if (o->old == i->old) {
			/* Report a range of inconsistency that just ended. */
			if (badstart >= 0)
				do_warn(
 _("discrepancy in %s at dblock 0x%llx words 0x%x-0x%x/0x%x\n"),
					filename, bno, badstart, j - 1, wordcnt);
			badstart = -1;
			continue;
		}

		if (badstart == -1)
			badstart = j;
	}

	if (badstart >= 0)
		do_warn(
 _("discrepancy in %s at dblock 0x%llx words 0x%x-0x%x/0x%x\n"),
					filename, bno, badstart, wordcnt,
					wordcnt);
}

static void
check_rtfile_contents(
	struct xfs_mount	*mp,
	bool			is_summary)
{
	struct xfs_inode	*ip;
	const struct xfs_buf_ops *buf_ops = xfs_rtblock_ops(mp, is_summary);
	const char		*filename;
	xfs_ino_t		ino;
	void			*buf;
	xfs_filblks_t		filelen;
	xfs_fileoff_t		bno = 0;
	int			error;

	if (is_summary) {
		filename = _("rtsummary");
		ino = mp->m_sb.sb_rsumino;
		buf = sumcompute;
		filelen = XFS_B_TO_FSB(mp, mp->m_rsumsize);
	} else {
		filename = _("rtbitmap");
		ino = mp->m_sb.sb_rbmino;
		buf = btmcompute;
		filelen = mp->m_sb.sb_rbmblocks;
	}

	error = -libxfs_iget(mp, NULL, ino, 0, &ip);
	if (error) {
		do_warn(_("unable to open %s file, err %d\n"), filename, error);
		return;
	}

	if (ip->i_disk_size != XFS_FSB_TO_B(mp, filelen)) {
		do_warn(_("expected %s file size %llu, found %llu\n"),
				filename,
				(unsigned long long)XFS_FSB_TO_B(mp, filelen),
				(unsigned long long)ip->i_disk_size);
	}

	while (bno < filelen)  {
		struct xfs_bmbt_irec	map;
		struct xfs_buf		*bp;
		unsigned int		offset = 0;
		int			nmap = 1;

		error = -libxfs_bmapi_read(ip, bno, 1, &map, &nmap, 0);
		if (error) {
			do_warn(_("unable to read %s mapping, err %d\n"),
					filename, error);
			break;
		}

		if (map.br_startblock == HOLESTARTBLOCK) {
			do_warn(_("hole in %s file at dblock 0x%llx\n"),
					filename, (unsigned long long)bno);
			break;
		}

		error = -libxfs_buf_read_uncached(mp->m_dev,
				XFS_FSB_TO_DADDR(mp, map.br_startblock),
				XFS_FSB_TO_BB(mp, 1),
				0, &bp, buf_ops);
		if (error) {
			do_warn(_("unable to read %s at dblock 0x%llx, err %d\n"),
					filename, (unsigned long long)bno, error);
			break;
		}

		if (xfs_has_rtgroups(mp)) {
			struct xfs_rtbuf_blkinfo	*hdr = bp->b_addr;

			if (hdr->rt_owner != cpu_to_be64(ino)) {
				do_warn(
 _("corrupt owner in %s at dblock 0x%llx\n"),
					filename, (unsigned long long)bno);
			}

			offset = sizeof(*hdr);
		}

		check_rtwords(mp, filename, bno, bp->b_addr + offset, buf);
		buf += mp->m_blockwsize << XFS_WORDLOG;
		bno++;
		libxfs_buf_relse(bp);
	}

	libxfs_irele(ip);
}

void
check_rtbitmap(
	struct xfs_mount	*mp)
{
	if (need_rbmino)
		return;

	check_rtfile_contents(mp, false);
}

void
check_rtsummary(
	struct xfs_mount	*mp)
{
	if (need_rsumino)
		return;

	check_rtfile_contents(mp, true);
}

void
fill_rtbitmap(
	struct xfs_mount	*mp)
{
	struct xfs_trans	*tp;
	struct xfs_inode	*ip;
	int			error;

	error = -libxfs_trans_alloc_empty(mp, &tp);
	if (error)
		do_error(
_("couldn't allocate empty transaction, error %d\n"), error);
	error = -libxfs_iget(mp, tp, mp->m_sb.sb_rbmino, 0, &ip);
	if (error)
		do_error(
_("couldn't iget realtime bitmap inode, error %d\n"), error);
	libxfs_trans_cancel(tp);

	error = -libxfs_rtfile_initialize_blocks(ip, 0, mp->m_sb.sb_rbmblocks,
			btmcompute);
	if (error)
		do_error(
_("couldn't re-initialize realtime bitmap inode, error %d\n"), error);
	libxfs_irele(ip);
}

void
fill_rtsummary(
	struct xfs_mount	*mp)
{
	struct xfs_trans	*tp;
	struct xfs_inode	*ip;
	int			error;

	error = -libxfs_trans_alloc_empty(mp, &tp);
	if (error)
		do_error(
_("couldn't allocate empty transaction, error %d\n"), error);
	error = -libxfs_iget(mp, tp, mp->m_sb.sb_rsumino, 0, &ip);
	if (error) {
		do_error(
_("couldn't iget realtime summary inode, error - %d\n"), error);
	}
	libxfs_trans_cancel(tp);

	mp->m_rsumip = ip;
	error = -libxfs_rtfile_initialize_blocks(ip, 0,
			mp->m_rsumsize >> mp->m_sb.sb_blocklog,
			sumcompute);
	mp->m_rsumip = NULL;
	if (error)
		do_error(
_("couldn't re-initialize realtime summary inode, error %d\n"), error);

	libxfs_irele(ip);
}

void
check_rtsb(
	struct xfs_mount	*mp)
{
	struct xfs_buf		*bp;
	int			error;

	error = -libxfs_buf_read_uncached(mp->m_rtdev_targp, XFS_RTSB_DADDR,
			XFS_FSB_TO_BB(mp, 1), 0, &bp, &xfs_rtsb_buf_ops);
	if (!error) {
		libxfs_buf_relse(bp);
		return;
	}

	if (no_modify) {
		do_warn(_("would rewrite realtime superblock\n"));
		return;
	}

	/*
	 * Rewrite the rt superblock so that an update to the primary fs
	 * superblock will not get confused by the non-matching rtsb.
	 */
	do_warn(_("will rewrite realtime superblock\n"));
	rewrite_rtsb(mp);
}

void
rewrite_rtsb(
	struct xfs_mount	*mp)
{
	struct xfs_buf		*rtsb_bp;
	struct xfs_buf		*sb_bp = libxfs_getsb(mp);
	int			error;

	if (!sb_bp)
		do_error(
 _("couldn't grab primary sb to update realtime sb\n"));

	error = -libxfs_buf_get_uncached(mp->m_rtdev_targp,
			XFS_FSB_TO_BB(mp, 1), XFS_RTSB_DADDR, &rtsb_bp);
	if (error)
		do_error(
 _("couldn't grab realtime superblock\n"));

	rtsb_bp->b_maps[0].bm_bn = XFS_RTSB_DADDR;
	rtsb_bp->b_ops = &xfs_rtsb_buf_ops;

	libxfs_rtgroup_update_super(rtsb_bp, sb_bp);
	libxfs_buf_mark_dirty(rtsb_bp);
	libxfs_buf_relse(rtsb_bp);
	libxfs_buf_relse(sb_bp);
}
