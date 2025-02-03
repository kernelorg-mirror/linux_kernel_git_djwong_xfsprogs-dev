// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025-2026 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef LIBFROG_HEALTHEVENT_H_
#define LIBFROG_HEALTHEVENT_H_

struct hme_prefix {
	/*
	 * Format a complete file path into this buffer to prevent the logging
	 * code from printing the mountpoint and a file handle.  Only works for
	 * file-related events.
	 */
	char		path[MAXPATHLEN];

	/* Set this to the mountpoint */
	const char	*mountpoint;
};

static inline bool hme_prefix_has_path(const struct hme_prefix *pfx)
{
	return pfx->path[0] != 0;
}

static inline void hme_prefix_clear_path(struct hme_prefix *pfx)
{
	pfx->path[0] = 0;
}

static inline void
hme_prefix_init(
	struct hme_prefix	*pfx,
	const char		*mountpoint)
{
	pfx->mountpoint = mountpoint;
	hme_prefix_clear_path(pfx);
}

void hme_report_event(const struct hme_prefix *pfx,
		const struct xfs_health_monitor_event *hme);

enum repair_outcome {
	REPAIR_SUCCESS,
	REPAIR_FAILED,
	REPAIR_PROBABLY_OK,
	REPAIR_UNNECESSARY,
};

void report_health_repair(const struct hme_prefix *pfx,
		const struct xfs_health_monitor_event *hme,
		uint32_t event_mask,
		enum repair_outcome outcome);

#endif /* LIBFROG_HEALTHEVENT_H_ */
