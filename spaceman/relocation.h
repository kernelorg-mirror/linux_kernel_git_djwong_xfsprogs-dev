// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 Red Hat, Inc.
 * All Rights Reserved.
 */
#ifndef XFS_SPACEMAN_RELOCATION_H_
#define XFS_SPACEMAN_RELOCATION_H_

struct radix_tree_root;

/*
 * The relocation_data tree contains all the information that is used by inode
 * and path discovery for relocation operations.
 */
extern struct radix_tree_root	relocation_data;

/*
 * Tags for the relocation_data tree that indicate what it contains and the
 * discovery information that needed to be stored.
 */
#define MOVE_INODE	0
#define MOVE_BLOCKS	1
#define INODE_PATH	2

/*
 * When the entry in the relocation_data tree is tagged with INODE_PATH, the
 * entry contains a structure that tracks the discovered paths to the inode. If
 * the inode has multiple hard links, then we chain each individual path found
 * via the path_list and record the number of paths in the link_count entry.
 */
struct inode_path {
	uint64_t		ino;
	struct list_head	path_list;
	uint32_t		link_count;
	char			path[1];
};

int find_relocation_targets(xfs_agnumber_t agno);
int relocate_file_to_ag(const char *mnt, const char *path, struct xfs_fd *xfd,
			xfs_agnumber_t agno);
int resolve_target_paths(const char *mntpt);

#endif /* XFS_SPACEMAN_RELOCATION_H_ */
