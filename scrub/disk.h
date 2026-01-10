// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2018-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef XFS_SCRUB_DISK_H_
#define XFS_SCRUB_DISK_H_

#define DISK_FLAG_SCSI_VERIFY	0x1
struct disk {
	struct stat	d_sb;
	int		d_fd;
	int		d_verify_fd;
	unsigned int	d_lbalog;
	unsigned int	d_lbasize;	/* bytes */
	unsigned int	d_flags;
	unsigned int	d_blksize;	/* bytes */
	uint64_t	d_size;		/* bytes */
	uint64_t	d_start;	/* bytes */
	unsigned int	d_verify_disk;
};

unsigned int disk_heads(struct disk *disk);
struct disk *disk_open(const char *pathname);
int disk_close(struct disk *disk);
ssize_t disk_read_verify(struct disk *disk, void *buf, uint64_t startblock,
		uint64_t blockcount, bool single_step);

static inline void
disk_config_xfs_verify(struct disk *disk, int mnt_fd, unsigned int verify_disk)
{
	disk->d_verify_fd = mnt_fd;
	disk->d_verify_disk = verify_disk;
}

#endif /* XFS_SCRUB_DISK_H_ */
