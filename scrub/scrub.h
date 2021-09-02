// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef XFS_SCRUB_SCRUB_H_
#define XFS_SCRUB_SCRUB_H_

/* Online scrub and repair. */
enum check_outcome {
	CHECK_DONE,	/* no further processing needed */
	CHECK_REPAIR,	/* schedule this for repairs */
	CHECK_ABORT,	/* end program */
	CHECK_RETRY,	/* repair failed, try again later */
	CHECK_TOOSLOW,	/* skipped because freezes are not allowed */
};

struct action_item;

void scrub_report_preen_triggers(struct scrub_ctx *ctx);
int scrub_primary_super(struct scrub_ctx *ctx, struct action_list *alist);
int scrub_ag_headers(struct scrub_ctx *ctx, xfs_agnumber_t agno,
		struct action_list *alist);
int scrub_ag_metadata(struct scrub_ctx *ctx, xfs_agnumber_t agno,
		struct action_list *alist);
int scrub_fs_metadata(struct scrub_ctx *ctx, struct action_list *alist);
int scrub_summary(struct scrub_ctx *ctx, struct action_list *alist);
int scrub_fs_counters(struct scrub_ctx *ctx, struct action_list *alist);
int scrub_clean_health(struct scrub_ctx *ctx, struct action_list *alist);

bool can_scrub_fs_metadata(struct scrub_ctx *ctx);
bool can_scrub_inode(struct scrub_ctx *ctx);
bool can_scrub_bmap(struct scrub_ctx *ctx);
bool can_scrub_dir(struct scrub_ctx *ctx);
bool can_scrub_attr(struct scrub_ctx *ctx);
bool can_scrub_symlink(struct scrub_ctx *ctx);
bool can_scrub_parent(struct scrub_ctx *ctx);
bool can_repair(struct scrub_ctx *ctx);

int scrub_inode_fields(struct scrub_ctx *ctx, uint64_t ino, uint32_t gen,
		struct action_list *alist);
int scrub_data_fork(struct scrub_ctx *ctx, uint64_t ino, uint32_t gen,
		struct action_list *alist);
int scrub_attr_fork(struct scrub_ctx *ctx, uint64_t ino, uint32_t gen,
		struct action_list *alist);
int scrub_cow_fork(struct scrub_ctx *ctx, uint64_t ino, uint32_t gen,
		struct action_list *alist);
int scrub_dir(struct scrub_ctx *ctx, uint64_t ino, uint32_t gen,
		struct action_list *alist);
int scrub_attr(struct scrub_ctx *ctx, uint64_t ino, uint32_t gen,
		struct action_list *alist);
int scrub_symlink(struct scrub_ctx *ctx, uint64_t ino, uint32_t gen,
		struct action_list *alist);
int scrub_parent(struct scrub_ctx *ctx, uint64_t ino, uint32_t gen,
		struct action_list *alist);

/* Repair parameters are the scrub inputs and retry count. */
struct action_item {
	struct list_head	list;
	__u64			ino;
	__u32			type;
	__u32			flags;
	__u32			gen;
	__u32			agno;
};

#endif /* XFS_SCRUB_SCRUB_H_ */
