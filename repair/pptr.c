// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2023 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "libxfs.h"
#include "libxfs/xfile.h"
#include "libxfs/xfblob.h"
#include "repair/err_protos.h"
#include "repair/slab.h"
#include "repair/pptr.h"

#undef PPTR_DEBUG

#ifdef PPTR_DEBUG
# define dbg_printf(f, a...)  do {printf(f, ## a); fflush(stdout); } while (0)
#else
# define dbg_printf(f, a...)
#endif

/*
 * Parent Pointer Validation
 * =========================
 *
 * Phase 6 validates the connectivity of the directory tree after validating
 * that all the space metadata are correct, and confirming all the inodes that
 * we intend to keep.  The first part of phase 6 walks the directories of the
 * filesystem to ensure that every file that isn't the root directory has a
 * parent.  Unconnected files are attached to the orphanage.  Filesystems with
 * the directory parent pointer feature enabled must also ensure that for every
 * directory entry that points to a child file, that child has a matching
 * parent pointer.
 *
 * There are many ways that we could check the parent pointers, but the means
 * that we have chosen is to build a per-AG master index of all parent pointers
 * of all inodes stored in that AG, and use that as the basis for comparison.
 * This consumes a lot of memory, but performing both a forward scan to check
 * dirent -> parent pointer and a backwards scan of parent pointer -> dirent
 * takes longer than the simple method presented here.  Userspace adds the
 * additional twist that inodes are not cached (and there are no ILOCKs), which
 * makes that approach even less attractive.
 *
 * During the directory walk at the start of phase 6, we transform each child
 * directory entry found into its parent pointer equivalent.  In other words,
 * the forward information:
 *
 *     (dir_ino, dir_offset, name, child_ino)
 *
 * becomes this backwards information:
 *
 *     (*child_agino, *dir_ino, dir_gen, *dir_offset, name)
 *
 * Key fields are starred.
 *
 * This tuple is recorded in the per-AG master parent pointer index.  Note
 * that names are stored separately in an xfblob data structure so that the
 * rest of the information can be sorted and processed as fixed-size records.
 *
 * Once we've finished with the forward scan, we get to work on the backwards
 * scan.  Each AG is processed independently.  First, we sort the per-AG master
 * records in order of child_agino, dir_ino, and dir_offset.  Each inode in the
 * AG is then processed in numerical order.
 *
 * The first thing that happens to the file is that we read all the extended
 * attributes to look for parent pointers.  Attributes that claim to be parent
 * pointers but are obviously garbage are thrown away.  The rest of the parent
 * pointers for that file are recorded in memory like this:
 *
 *     (*dir_ino, dir_gen, *dir_offset, name)
 *
 * When we've concluded the xattr scan, these records are sorted in order of
 * dir_ino and dir_offset.  The master index cursor should point at the first
 * record for the file that we're scanning, if everything is consistent.
 *
 * If not, there are two possibilities:
 *
 * A. The master index cursor points to a higher inode number than the one we
 * are scanning.  The file has apparently lost all parents, so all parent
 * pointers (if any) must be deleted.  This should only happen to metadata
 * inodes.
 *
 * B. The cursor instead points to a lower inode number than the one we are
 * scanning.  This means that there exists a directory entry pointing at an
 * inode that is free.  We supposedly already settled which inodes are free
 * and which aren't, which means in-memory information is inconsistent.  Abort.
 *
 * Otherwise, we are ready to check the file parent pointers against the
 * master.  If the ondisk directory metadata are all consistent, this recordset
 * should correspond exactly to the subset of the master records with a
 * child_agino matching the file that we're scanning.  We should be able to
 * walk both sets in lockstep, and find one of the following outcomes:
 *
 * 1) The master index cursor is ahead of the ondisk index cursor.  This means
 * that the inode has parent pointers that were not found during the dirent
 * scan.  These should be deleted.
 *
 * 2) The ondisk index gets ahead of the master index.  This means that the
 * dirent scan found parent pointers that are not attached to the inode.
 * These should be added.
 *
 * 3) The parent_gen or (dirent) name are not consistent.  Update the parent
 * pointer to the values that we found during the dirent scan.
 *
 * 4) Everything matches.  Move on to the next parent pointer.
 *
 * The current implementation does not try to rebuild directories from parent
 * pointer information, as this requires a lengthy scan of the filesystem for
 * each broken directory.
 */

struct ag_pptr {
	/* parent directory handle */
	xfs_ino_t		parent_ino;
	unsigned int		parent_gen;

	/* dirent offset */
	xfs_dir2_dataptr_t	diroffset;

	/* dirent name length */
	unsigned int		namelen;

	/* cookie for the actual dirent name */
	xfblob_cookie		name_cookie;

	/* agino of the child file */
	xfs_agino_t		child_agino;
};

struct ag_pptrs {
	/* Lock to protect pptr_recs during the dirent scan. */
	pthread_mutex_t		lock;

	/* Parent pointer records for files in this AG. */
	struct xfs_slab		*pptr_recs;
};

/* Global names storage file. */
static struct xfblob	*names;
static pthread_mutex_t	names_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct ag_pptrs	*fs_pptrs;

void
parent_ptr_free(
	struct xfs_mount	*mp)
{
	xfs_agnumber_t		agno;

	if (!xfs_has_parent(mp))
		return;

	for (agno = 0; agno < mp->m_sb.sb_agcount; agno++) {
		free_slab(&fs_pptrs[agno].pptr_recs);
		pthread_mutex_destroy(&fs_pptrs[agno].lock);
	}
	free(fs_pptrs);
	fs_pptrs = NULL;

	xfblob_destroy(names);
}

void
parent_ptr_init(
	struct xfs_mount	*mp)
{
	xfs_agnumber_t		agno;
	int			error;

	if (!xfs_has_parent(mp))
		return;

	error = -xfblob_create(mp, "parent pointer names", &names);
	if (error)
		do_error(_("init parent pointer names failed: %s\n"),
				strerror(error));

	fs_pptrs = calloc(mp->m_sb.sb_agcount, sizeof(struct ag_pptrs));
	if (!fs_pptrs)
		do_error(
 _("init parent pointer per-AG record array failed: %s\n"),
				strerror(errno));

	for (agno = 0; agno < mp->m_sb.sb_agcount; agno++) {
		error = pthread_mutex_init(&fs_pptrs[agno].lock, NULL);
		if (error)
			do_error(
 _("init agno %u parent pointer lock failed: %s\n"),
					agno, strerror(error));

		error = -init_slab(&fs_pptrs[agno].pptr_recs,
				sizeof(struct ag_pptr));
		if (error)
			do_error(
 _("init agno %u parent pointer recs failed: %s\n"),
					agno, strerror(error));
	}
}

/* Remember that @dp has a dirent (@fname, @ino) at @diroffset. */
void
add_parent_ptr(
	xfs_ino_t		ino,
	const unsigned char	*fname,
	xfs_dir2_dataptr_t	diroffset,
	struct xfs_inode	*dp)
{
	struct xfs_mount	*mp = dp->i_mount;
	struct ag_pptr		ag_pptr = {
		.child_agino	= XFS_INO_TO_AGINO(mp, ino),
		.parent_ino	= dp->i_ino,
		.parent_gen	= VFS_I(dp)->i_generation,
		.diroffset	= diroffset,
		.namelen	= strlen(fname),
	};
	struct ag_pptrs		*ag_pptrs;
	xfs_agnumber_t		agno = XFS_INO_TO_AGNO(mp, ino);
	int			error;

	if (!xfs_has_parent(mp))
		return;

	pthread_mutex_lock(&names_mutex);
	error = -xfblob_store(names, &ag_pptr.name_cookie, fname,
			ag_pptr.namelen);
	pthread_mutex_unlock(&names_mutex);
	if (error)
		do_error(_("storing name '%s' failed: %s\n"),
				fname, strerror(error));

	ag_pptrs = &fs_pptrs[agno];
	pthread_mutex_lock(&ag_pptrs->lock);
	error = -slab_add(ag_pptrs->pptr_recs, &ag_pptr);
	pthread_mutex_unlock(&ag_pptrs->lock);
	if (error)
		do_error(_("storing name '%s' key failed: %s\n"),
				fname, strerror(error));

	dbg_printf(
 _("%s: dp %llu fname '%s' diroffset %u ino %llu cookie 0x%llx\n"),
			__func__, (unsigned long long)dp->i_ino, fname,
			diroffset, (unsigned long long)ino,
			(unsigned long long)ag_pptr.name_cookie);
}
