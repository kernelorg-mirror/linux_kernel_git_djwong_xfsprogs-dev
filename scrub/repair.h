/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef XFS_SCRUB_REPAIR_H_
#define XFS_SCRUB_REPAIR_H_

struct action_list {
	struct list_head	list;
};

struct action_item;

int action_lists_alloc(size_t nr, struct action_list **listsp);
void action_lists_free(struct action_list **listsp);

void action_list_init(struct action_list *alist);
size_t action_list_length(struct action_list *alist);
void action_list_add(struct action_list *dest, struct action_item *item);
void action_list_discard(struct action_list *alist);

void repair_item_mustfix(struct scrub_item *sri, struct scrub_item *fix_now,
		unsigned long long *broken_primaries,
		unsigned long long *broken_secondaries);

int action_list_process(struct scrub_ctx *ctx, int fd,
		struct action_list *alist, unsigned int repair_flags);
int repair_item_corruption(struct scrub_ctx *ctx, struct scrub_item *sri);
int repair_item(struct scrub_ctx *ctx, struct scrub_item *sri,
		unsigned int repair_flags);
int repair_item_defer(struct scrub_ctx *ctx, const struct scrub_item *sri);
static inline int
repair_item_completely(
	struct scrub_ctx	*ctx,
	struct scrub_item	*sri)
{
	return repair_item(ctx, sri, XRM_COMPLAIN_IF_UNFIXED | XRM_NOPROGRESS);
}

#endif /* XFS_SCRUB_REPAIR_H_ */
