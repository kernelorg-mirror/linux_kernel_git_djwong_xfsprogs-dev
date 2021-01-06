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

/*
 * Only ask the kernel to repair this object if the kernel directly told us it
 * was corrupt.  Objects that are only flagged as having cross-referencing
 * errors or flagged as eligible for optimization are left for later.
 */
#define XRM_REPAIR_ONLY		(1U << 0)

/* Complain if still broken even after fix. */
#define XRM_COMPLAIN_IF_UNFIXED	(1U << 1)

/* Don't call progress_add after repairing an item. */
#define XRM_NOPROGRESS		(1U << 2)

int action_list_process(struct scrub_ctx *ctx, struct action_list *alist,
		unsigned int repair_flags);
int repair_item_corruption(struct scrub_ctx *ctx, struct scrub_item *sri);
int repair_item(struct scrub_ctx *ctx, struct scrub_item *sri,
		unsigned int repair_flags);
int repair_item_defer(struct scrub_ctx *ctx, const struct scrub_item *sri);

static inline unsigned int
repair_item_count_needsrepair(
	const struct scrub_item	*sri)
{
	unsigned int		scrub_type;
	unsigned int		nr = 0;

	foreach_scrub_type(scrub_type)
		if (sri->sri_state[scrub_type] & SCRUB_ITEM_REPAIR_ANY)
			nr++;
	return nr;
}

static inline int
repair_item_completely(
	struct scrub_ctx	*ctx,
	struct scrub_item	*sri)
{
	return repair_item(ctx, sri, XRM_COMPLAIN_IF_UNFIXED | XRM_NOPROGRESS);
}

#endif /* XFS_SCRUB_REPAIR_H_ */
