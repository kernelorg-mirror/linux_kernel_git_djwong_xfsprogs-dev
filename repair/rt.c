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
#include "libfrog/bitmap.h"
#include "rt.h"

/* Bitmap of rt group inodes */
static struct bitmap	*rtg_inodes[XFS_RTG_MAX];

void
rtinit(xfs_mount_t *mp)
{
	unsigned long long	wordcnt;

	if (mp->m_sb.sb_rblocks == 0)
		return;

	/*
	 * Allocate buffers for formatting the collected rt free space
	 * information.  The rtbitmap buffer must be large enough to compare
	 * against any unused bytes in the last block of the file.
	 */
	wordcnt = libxfs_rtbitmap_wordcount(mp, mp->m_sb.sb_rextents);
	btmcompute = calloc(wordcnt, sizeof(union xfs_rtword_raw));
	if (!btmcompute)
		do_error(
	_("couldn't allocate memory for incore realtime bitmap.\n"));

	wordcnt = libxfs_rtsummary_wordcount(mp, mp->m_rsumlevels,
			mp->m_sb.sb_rbmblocks);
	sumcompute = calloc(wordcnt, sizeof(union xfs_suminfo_raw));
	if (!sumcompute)
		do_error(
	_("couldn't allocate memory for incore realtime summary info.\n"));
}

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
int
generate_rtinfo(
	struct xfs_mount	*mp,
	union xfs_rtword_raw	*words,
	union xfs_suminfo_raw	*sumcompute)
{
	xfs_rtxnum_t	extno;
	xfs_rtxnum_t	start_ext;
	int		bitsperblock;
	int		bmbno;
	xfs_rtword_t	freebit;
	xfs_rtword_t	bits;
	int		start_bmbno;
	int		i;
	int		offs;
	int		log;
	int		len;
	int		in_extent;

	ASSERT(mp->m_rbmip == NULL);

	bitsperblock = mp->m_blockwsize << XFS_NBWORDLOG;
	extno = start_ext = 0;
	bmbno = in_extent = start_bmbno = 0;

	/*
	 * slower but simple, don't play around with trying to set
	 * things one word at a time, just set bit as required.
	 * Have to * track start and end (size) of each range of
	 * free extents to set the summary info properly.
	 */
	while (extno < mp->m_sb.sb_rextents)  {
		freebit = 1;
		set_rtword(mp, words, 0);
		bits = 0;
		for (i = 0; i < sizeof(xfs_rtword_t) * NBBY &&
				extno < mp->m_sb.sb_rextents; i++, extno++)  {
			if (get_rtbmap(extno) == XR_E_FREE)  {
				sb_frextents++;
				bits |= freebit;

				if (in_extent == 0) {
					start_ext = extno;
					start_bmbno = bmbno;
					in_extent = 1;
				}
			} else if (in_extent == 1) {
				len = (int) (extno - start_ext);
				log = libxfs_highbit64(len);
				offs = xfs_rtsumoffs(mp, log, start_bmbno);
				inc_sumcount(mp, sumcompute, offs);
				in_extent = 0;
			}

			freebit <<= 1;
		}
		set_rtword(mp, words, bits);
		words++;

		if (extno % bitsperblock == 0)
			bmbno++;
	}
	if (in_extent == 1) {
		len = (int) (extno - start_ext);
		log = libxfs_highbit64(len);
		offs = xfs_rtsumoffs(mp, log, start_bmbno);
		inc_sumcount(mp, sumcompute, offs);
	}

	if (mp->m_sb.sb_frextents != sb_frextents) {
		do_warn(_("sb_frextents %" PRIu64 ", counted %" PRIu64 "\n"),
				mp->m_sb.sb_frextents, sb_frextents);
	}

	return(0);
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
	const char		*filename,
	xfs_ino_t		ino,
	void			*buf,
	xfs_fileoff_t		filelen,
	const struct xfs_buf_ops *buf_ops)
{
	struct xfs_bmbt_irec	map;
	struct xfs_buf		*bp;
	struct xfs_inode	*ip;
	xfs_fileoff_t		bno = 0;
	int			error;

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
		xfs_daddr_t	daddr;
		int		nmap = 1;

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

		daddr = XFS_FSB_TO_DADDR(mp, map.br_startblock);
		error = -libxfs_buf_read_uncached(mp->m_dev, daddr,
				XFS_FSB_TO_BB(mp, map.br_blockcount),
				0, &bp, buf_ops);
		if (error) {
			do_warn(_("unable to read %s at dblock 0x%llx, err %d\n"),
					filename, (unsigned long long)bno, error);
			break;
		}

		if (buf_ops == &xfs_rtbitmap_buf_ops) {
			struct xfs_rtalloc_args		args = {
				.mp			= mp,
			};
			struct xfs_rtbuf_blkinfo	*hdr = bp->b_addr;
			union xfs_rtword_raw		*incore = buf;
			union xfs_rtword_raw		*ondisk;

			if (hdr->rt_owner != cpu_to_be64(ino)) {
				do_warn(
 _("corrupt owner in %s at dblock 0x%llx\n"),
					filename, (unsigned long long)bno);
			}

			args.rbmbp = bp;
			ondisk = xfs_rbmblock_wordptr(&args, 0);
			check_rtwords(mp, filename, bno, ondisk, incore);
			buf += mp->m_blockwsize << XFS_WORDLOG;
		} else if (buf_ops == &xfs_rtsummary_buf_ops) {
			struct xfs_rtalloc_args		args = {
				.mp			= mp,
			};
			struct xfs_rtbuf_blkinfo	*hdr = bp->b_addr;
			union xfs_suminfo_raw		*incore = buf;
			union xfs_suminfo_raw		*ondisk;

			if (hdr->rt_owner != cpu_to_be64(ino)) {
				do_warn(
 _("corrupt owner in %s at dblock 0x%llx\n"),
					filename, (unsigned long long)bno);
			}

			args.sumbp = bp;
			ondisk = xfs_rsumblock_infoptr(&args, 0);
			check_rtwords(mp, filename, bno, ondisk, incore);
			buf += mp->m_blockwsize << XFS_WORDLOG;
		} else {
			check_rtwords(mp, filename, bno, bp->b_addr, buf);
			buf += XFS_FSB_TO_B(mp, map.br_blockcount);
		}

		bno += map.br_blockcount;
		libxfs_buf_relse(bp);
	}

	libxfs_irele(ip);
}

void
check_rtbitmap(
	struct xfs_mount	*mp)
{
	const struct xfs_buf_ops *buf_ops = NULL;

	if (need_rbmino)
		return;
	if (xfs_has_rtgroups(mp))
		buf_ops = &xfs_rtbitmap_buf_ops;

	check_rtfile_contents(mp, "rtbitmap", mp->m_sb.sb_rbmino, btmcompute,
			mp->m_sb.sb_rbmblocks, buf_ops);
}

void
check_rtsummary(
	struct xfs_mount	*mp)
{
	const struct xfs_buf_ops *buf_ops = NULL;

	if (need_rsumino)
		return;
	if (xfs_has_rtgroups(mp))
		buf_ops = &xfs_rtsummary_buf_ops;

	check_rtfile_contents(mp, "rtsummary", mp->m_sb.sb_rsumino, sumcompute,
			XFS_B_TO_FSB(mp, mp->m_rsumsize), buf_ops);
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

bool
is_rtgroup_inode(
	xfs_ino_t		ino,
	enum xfs_rtg_inodes	type)
{
	if (!rtg_inodes[type])
		return false;
	return bitmap_test(rtg_inodes[type], ino, 1);
}

xfs_rgnumber_t
rtgroup_for_rtginode(
	struct xfs_mount	*mp,
	xfs_ino_t		ino,
	enum xfs_rtg_inodes	type)
{
	struct xfs_rtgroup	*rtg;
	xfs_rgnumber_t		rgno;

	if (!rtg_inodes[type])
		return NULLRGNUMBER;

	for_each_rtgroup(mp, rgno, rtg) {
		if (rtg->rtg_inodes[type] &&
		    rtg->rtg_inodes[type]->i_ino == ino)
			return rgno;
	}

	return NULLRGNUMBER;
}

void
rtginode_avoid_check(
	struct xfs_mount	*mp,
	enum xfs_rtg_inodes	type)
{
	struct xfs_rtgroup	*rtg;
	xfs_rgnumber_t		rgno;

	for_each_rtgroup(mp, rgno, rtg)
		libxfs_rtginode_irele(&rtg->rtg_inodes[type]);

	bitmap_clear(rtg_inodes[type], 0, XFS_MAXINUMBER);
}

static inline int
set_rtginode(
	struct xfs_trans	*tp,
	struct xfs_rtgroup	*rtg,
	enum xfs_rtg_inodes	type)
{
	struct xfs_inode	*ip;
	int			error;

	error = -libxfs_rtginode_load(rtg, type, tp);
	if (error)
		return error;

	ip = rtg->rtg_inodes[type];
	if (!ip) /* inode type not enabled */
		return 0;

	if (bitmap_test(rtg_inodes[type], ip->i_ino, 1))
		return EFSCORRUPTED;
	return bitmap_set(rtg_inodes[type], ip->i_ino, 1);
}

void
discover_rtgroup_inodes(
	struct xfs_mount	*mp)
{
	struct xfs_rtgroup	*rtg;
	struct xfs_trans	*tp;
	xfs_rgnumber_t		rgno;
	int			error, err2;
	int			i;

	if (!xfs_has_rtgroups(mp))
		return;

	for (i = 0; i < XFS_RTG_MAX; i++) {
		error = bitmap_alloc(&rtg_inodes[i]);
		if (error)
			goto out;
	}

	error = -libxfs_trans_alloc_empty(mp, &tp);
	if (error)
		goto out;
	error = -libxfs_rtginode_load_parent(tp);
	if (error)
		goto out_cancel;

	for_each_rtgroup(mp, rgno, rtg) {
		for (i = 0; i < XFS_RTG_MAX; i++) {
			err2 = set_rtginode(tp, rtg, i);
			if (err2 && !error)
				error = err2;
		}
	}

out_cancel:
	libxfs_trans_cancel(tp);
out:
	if (error == EFSCORRUPTED)
		do_warn(
 _("corruption in metadata directory tree while discovering rt group inodes\n"));
	if (error)
		do_warn(
 _("couldn't discover rt group inodes, err %d\n"),
				error);
}

void
free_rtgroup_inodes(
	struct xfs_mount	*mp)
{
	int			i;

	if (!xfs_has_rtgroups(mp))
		return;

	for (i = 0; i < XFS_RTG_MAX; i++)
		bitmap_free(&rtg_inodes[i]);
}
