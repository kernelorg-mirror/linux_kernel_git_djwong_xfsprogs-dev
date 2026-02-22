// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"

#include "libfrog/flagmap.h"
#include "libfrog/statmount.h"
#include "command.h"
#include "input.h"
#include "init.h"
#include "io.h"

static const struct flag_map statmount_funcs[] = {
	{ STATMOUNT_SB_BASIC,		N_("sb_basic") },
	{ STATMOUNT_MNT_BASIC,		N_("mnt_basic") },
	{ STATMOUNT_PROPAGATE_FROM,	N_("propagate_from") },
	{ STATMOUNT_MNT_ROOT,		N_("mnt_root") },
	{ STATMOUNT_MNT_POINT,		N_("mnt_point") },
	{ STATMOUNT_FS_TYPE,		N_("fs_type") },
	{ STATMOUNT_MNT_NS_ID,		N_("mnt_ns_id") },
	{ STATMOUNT_MNT_OPTS,		N_("mnt_opts") },
	{ STATMOUNT_FS_SUBTYPE,		N_("fs_subtype") },
	{ STATMOUNT_SB_SOURCE,		N_("sb_source") },
	{ STATMOUNT_OPT_ARRAY,		N_("opt_array") },
	{ STATMOUNT_OPT_SEC_ARRAY,	N_("opt_sec_array") },
	{ STATMOUNT_SUPPORTED_MASK,	N_("supported_mask") },
	{0, NULL},
};

static const struct flag_map mount_attrs[] = {
	{ MOUNT_ATTR_RDONLY,		N_("rdonly") },
	{ MOUNT_ATTR_NOSUID,		N_("nosuid") },
	{ MOUNT_ATTR_NODEV,		N_("nodev") },
	{ MOUNT_ATTR_NOEXEC,		N_("noexec") },
	{ MOUNT_ATTR__ATIME,		N_("atime") },
	{ MOUNT_ATTR_RELATIME,		N_("relatime") },
	{ MOUNT_ATTR_NOATIME,		N_("noatime") },
	{ MOUNT_ATTR_STRICTATIME,	N_("strictatime") },
	{ MOUNT_ATTR_NODIRATIME,	N_("nodiratime") },
	{ MOUNT_ATTR_IDMAP,		N_("idmap") },
	{ MOUNT_ATTR_NOSYMFOLLOW,	N_("nosymfollow") },
	{0, NULL},
};

static const struct flag_map mount_prop_flags[] = {
	{ MS_SHARED,			N_("shared") },
	{ MS_SLAVE,			N_("nopeer") },
	{ MS_PRIVATE,			N_("private") },
	{ MS_UNBINDABLE,		N_("unbindable") },
	{0, NULL},
};

static void
listmount_help(void)
{
	printf(_(
"\n"
" List all mounted filesystems.\n"
"\n"
" -f   -- statmount mask flags to set.  Defaults to all possible flags.\n"
" -i   -- mount id to use.  Defaults to the root of the mount namespace.\n"
" -n   -- path to a procfs mount namespace file.\n"
" -t   -- only display mount info for this fs type.\n"
));
}

static void
dump_mountinfo(
	int			mnt_ns_fd,
	uint64_t		statmount_flags,
	bool			rawflag,
	uint64_t		row_id,
	const char		*fstype,
	uint64_t		mnt_id)
{
	char			buf[4096];
	size_t			smbuf_size = libfrog_statmount_sizeof(4096);
	struct statmount	*smbuf = malloc(smbuf_size);
	int			ret;

	if (!smbuf) {
		perror("malloc");
		return;
	}

	if (fstype)
		statmount_flags |= STATMOUNT_FS_TYPE | STATMOUNT_FS_SUBTYPE;

	ret = libfrog_statmount(mnt_id, mnt_ns_fd, statmount_flags, smbuf,
			smbuf_size);
	if (ret) {
		perror("statmount");
		goto out_smbuf;
	}

	if (fstype) {
		char	real_fstype[256];

		if (!(smbuf->mask & STATMOUNT_FS_TYPE))
			return;

		if (smbuf->mask & STATMOUNT_FS_SUBTYPE)
			snprintf(real_fstype, sizeof(fstype), "%s.%s",
					smbuf->str + smbuf->fs_type,
					smbuf->str + smbuf->fs_subtype);
		else
			snprintf(real_fstype, sizeof(fstype), "%s",
					smbuf->str + smbuf->fs_type);
		if (strcmp(fstype, real_fstype))
			return;
	}

	printf("mnt_id[%llu]: 0x%llx\n", (unsigned long long)row_id,
			(unsigned long long)mnt_id);

	if (rawflag) {
		printf("\tmask: 0x%llx\n", (unsigned long long)smbuf->mask);
	} else {
		mask_to_string(statmount_funcs, smbuf->mask, ",", buf,
				sizeof(buf));
		printf("\tmask: {%s}\n", buf);
	}

	if (smbuf->mask & STATMOUNT_SB_BASIC) {
		printf("\tsb_dev_major: %u\n", smbuf->sb_dev_major);
		printf("\tsb_dev_minor: %u\n", smbuf->sb_dev_minor);
		printf("\tsb_magic: 0x%llx\n",
				(unsigned long long)smbuf->sb_magic);
		printf("\tsb_flags: 0x%x\n", smbuf->sb_flags);
	}

	if (smbuf->mask & STATMOUNT_MNT_BASIC) {
		printf("\tmnt_id: 0x%llx\n",
				(unsigned long long)smbuf->mnt_id);
		printf("\tmnt_parent_id: 0x%llx\n",
				(unsigned long long)smbuf->mnt_parent_id);
		printf("\tmnt_id_old: %u\n", smbuf->mnt_id_old);
		printf("\tmnt_parent_id_old: %u\n", smbuf->mnt_parent_id_old);
		if (rawflag) {
			printf("\tmnt_attr: 0x%llx\n",
					(unsigned long long)smbuf->mnt_attr);
			printf("\tmnt_propagation: 0x%llx\n",
					(unsigned long long)smbuf->mnt_propagation);
		} else {
			mask_to_string(mount_attrs, smbuf->mnt_attr, ",", buf,
					sizeof(buf));
			printf("\tmnt_attr: {%s}\n", buf);
			mask_to_string(mount_prop_flags, smbuf->mnt_propagation,
					",", buf, sizeof(buf));
			printf("\tmnt_propagation: {%s}\n", buf);
		}
		printf("\tmnt_peer_group: 0x%llx\n",
				(unsigned long long)smbuf->mnt_peer_group);
		printf("\tmnt_master: 0x%llx\n",
				(unsigned long long)smbuf->mnt_master);
	}

	if (smbuf->mask & STATMOUNT_PROPAGATE_FROM)
		printf("\tpropagate_from: 0x%llx\n",
				(unsigned long long)smbuf->propagate_from);

	if (smbuf->mask & STATMOUNT_MNT_ROOT)
		printf("\tmnt_root: %s\n", smbuf->str + smbuf->mnt_root);
	if (smbuf->mask & STATMOUNT_MNT_POINT)
		printf("\tmnt_point: %s\n", smbuf->str + smbuf->mnt_point);
	if (smbuf->mask & STATMOUNT_FS_TYPE)
		printf("\tfs_type: %s\n", smbuf->str + smbuf->fs_type);
	if (smbuf->mask & STATMOUNT_FS_SUBTYPE)
		printf("\tfs_subtype: %s\n", smbuf->str + smbuf->fs_subtype);

	if (smbuf->mask & STATMOUNT_MNT_NS_ID)
		printf("\tmnt_ns_id: 0x%llx\n",
				(unsigned long long)smbuf->mnt_ns_id);

	if (smbuf->mask & STATMOUNT_MNT_OPTS)
		printf("\tmnt_opts: %s\n", smbuf->str + smbuf->mnt_opts);
	if (smbuf->mask & STATMOUNT_SB_SOURCE)
		printf("\tsb_source: %s\n", smbuf->str + smbuf->sb_source);

	if (smbuf->mask & STATMOUNT_SUPPORTED_MASK) {
		if (rawflag) {
			printf("\tsupported_mask: 0x%llx\n",
					(unsigned long long)smbuf->supported_mask);
		} else {
			mask_to_string(statmount_funcs, smbuf->supported_mask,
					",", buf, sizeof(buf));
			printf("\tsupported_mask: {%s}\n", buf);
		}
	}

out_smbuf:
	free(smbuf);
}

#define NR_MNT_IDS		7

static int
listmount_f(
	int			argc,
	char			**argv)
{
	uint64_t		mnt_ids[NR_MNT_IDS];
	uint64_t		cursor = LISTMOUNT_INIT_CURSOR;
	uint64_t		statmount_flags = -1ULL;
	uint64_t		mnt_id = LSMT_ROOT;
	const char		*fstype = NULL;
	unsigned long long	rows = 0;
	int			mnt_ns_fd = DEFAULT_MOUNTNS_FD;
	int			rawflag = 0;
	int			c;
	int			ret;

	while ((c = getopt(argc, argv, "f:i:n:rt:")) > 0) {
		switch (c) {
		case 'f':
			errno = 0;
			statmount_flags = strtoull(optarg, NULL, 0);
			if (errno) {
				perror(optarg);
				return 1;
			}
			break;
		case 'i':
			errno = 0;
			mnt_id = strtoull(optarg, NULL, 0);
			if (errno) {
				perror(optarg);
				return 1;
			}
			break;
		case 'n':
			mnt_ns_fd = open(optarg, O_RDONLY);
			if (mnt_ns_fd < 0) {
				perror(optarg);
				return 1;
			}
			break;
		case 'r':
			rawflag++;
			break;
		case 't':
			fstype = optarg;
			break;
		default:
			listmount_help();
			return 1;
		}
	}

	while ((ret = libfrog_listmount(mnt_id, mnt_ns_fd, &cursor,
					mnt_ids, NR_MNT_IDS)) > 0) {
		for (c = 0; c < ret; c++)
			dump_mountinfo(mnt_ns_fd, statmount_flags, rawflag,
					rows++, fstype, mnt_ids[c]);
	}

	if (ret < 0)
		perror("listmount");

	return 0;
}

static const struct cmdinfo listmount_cmd = {
	.name		= "listmount",
	.cfunc		= listmount_f,
	.argmin		= -1,
	.argmax		= -1,
	.flags		= CMD_NOFILE_OK | CMD_FOREIGN_OK | CMD_NOMAP_OK,
	.oneline	= N_("list mounted filesystems"),
	.help		= listmount_help,
};

void
listmount_init(void)
{
	add_command(&listmount_cmd);
}
