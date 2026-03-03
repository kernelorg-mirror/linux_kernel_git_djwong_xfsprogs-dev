/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2026 Oracle.  All rights reserved.
 * All Rights Reserved.
 */
#ifndef __LIBFROG_STATMOUNT_H__
#define __LIBFROG_STATMOUNT_H__

/* This is the path to the current process' mount namespace file */
#define DEFAULT_MOUNTNS_FILE	"/proc/self/ns/mnt"

/*
 * Believe it or not, listmount and statmount treat a zero value for mnt_ns_fd
 * as if that means "use the current process' mount namespace" even though
 * Linus Torvalds roared about that with the BPF people.
 */
#define DEFAULT_MOUNTNS_FD	(0)

#ifdef OVERRIDE_SYSTEM_STATMOUNT
struct statmount {
	__u32 size;		/* Total size, including strings */
	__u32 mnt_opts;		/* [str] Options (comma separated, escaped) */
	__u64 mask;		/* What results were written */
	__u32 sb_dev_major;	/* Device ID */
	__u32 sb_dev_minor;
	__u64 sb_magic;		/* ..._SUPER_MAGIC */
	__u32 sb_flags;		/* SB_{RDONLY,SYNCHRONOUS,DIRSYNC,LAZYTIME} */
	__u32 fs_type;		/* [str] Filesystem type */
	__u64 mnt_id;		/* Unique ID of mount */
	__u64 mnt_parent_id;	/* Unique ID of parent (for root == mnt_id) */
	__u32 mnt_id_old;	/* Reused IDs used in proc/.../mountinfo */
	__u32 mnt_parent_id_old;
	__u64 mnt_attr;		/* MOUNT_ATTR_... */
	__u64 mnt_propagation;	/* MS_{SHARED,SLAVE,PRIVATE,UNBINDABLE} */
	__u64 mnt_peer_group;	/* ID of shared peer group */
	__u64 mnt_master;	/* Mount receives propagation from this ID */
	__u64 propagate_from;	/* Propagation from in current namespace */
	__u32 mnt_root;		/* [str] Root of mount relative to root of fs */
	__u32 mnt_point;	/* [str] Mountpoint relative to current root */
	__u64 mnt_ns_id;	/* ID of the mount namespace */
	__u32 fs_subtype;	/* [str] Subtype of fs_type (if any) */
	__u32 sb_source;	/* [str] Source string of the mount */
	__u32 opt_num;		/* Number of fs options */
	__u32 opt_array;	/* [str] Array of nul terminated fs options */
	__u32 opt_sec_num;	/* Number of security options */
	__u32 opt_sec_array;	/* [str] Array of nul terminated security options */
	__u64 supported_mask;	/* Mask flags that this kernel supports */
	__u64 __spare2[45];
	char str[];		/* Variable size part containing strings */
};
#endif

/* all the new flags added since the beginning of statmount */

#ifndef STATMOUNT_MNT_NS_ID
#define STATMOUNT_MNT_NS_ID		0x00000040U	/* Want/got mnt_ns_id */
#endif

#ifndef STATMOUNT_MNT_OPTS
#define STATMOUNT_MNT_OPTS		0x00000080U	/* Want/got mnt_opts */
#endif

#ifndef STATMOUNT_FS_SUBTYPE
#define STATMOUNT_FS_SUBTYPE		0x00000100U	/* Want/got fs_subtype */
#endif

#ifndef STATMOUNT_SB_SOURCE
#define STATMOUNT_SB_SOURCE		0x00000200U	/* Want/got sb_source */
#endif

#ifndef STATMOUNT_OPT_ARRAY
#define STATMOUNT_OPT_ARRAY		0x00000400U	/* Want/got opt_... */
#endif

#ifndef STATMOUNT_OPT_SEC_ARRAY
#define STATMOUNT_OPT_SEC_ARRAY		0x00000800U	/* Want/got opt_sec... */
#endif

#ifndef STATMOUNT_SUPPORTED_MASK
#define STATMOUNT_SUPPORTED_MASK	0x00001000U	/* Want/got supported mask flags */
#endif

/* flag bits for statmount */
#ifndef STATMOUNT_BY_FD
#define STATMOUNT_BY_FD		0x00000001U	/* want mountinfo for given fd */
#endif

#define LISTMOUNT_INIT_CURSOR		(0ULL)

int libfrog_listmount(uint64_t mnt_id, int mnt_ns_fd, uint64_t *cursor,
		uint64_t *mnt_ids, size_t nr_mnt_ids);

int libfrog_statmount(uint64_t mnt_id, int mnt_ns_fd, uint64_t statmount_flags,
		struct statmount *smbuf, size_t smbuf_size);

int libfrog_fstatmount(int fd, uint64_t statmount_flags,
		struct statmount *smbuf, size_t smbuf_size);

static inline size_t libfrog_statmount_sizeof(size_t strings_bytes)
{
	return sizeof(struct statmount) + strings_bytes;
}

#endif /* __LIBFROG_STATMOUNT_H__ */
