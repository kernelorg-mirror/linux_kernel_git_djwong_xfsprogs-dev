// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
use crate::xfs_fs;
use crate::xfs_fs::xfs_health_monitor;
use crate::xfs_fs::xfs_health_samefs;
use nix::ioctl_write_ptr;
use std::fs::File;
use std::os::fd::AsRawFd;
use std::os::fd::FromRawFd;

pub mod cstruct;
pub mod event;
pub mod fs;
pub mod groups;
pub mod inodes;
pub mod json;
pub mod samefs;

ioctl_write_ptr!(xfs_ioc_health_monitor, 'X', 68, xfs_health_monitor);
ioctl_write_ptr!(xfs_ioc_health_samefs, 'X', 69, xfs_health_samefs);

/// Check if the open file supports a health monitor.
pub fn is_supported(fp: &File, use_json: bool) -> bool {
    let hminfo = xfs_health_monitor {
        format: if use_json {
            xfs_fs::XFS_HEALTH_MONITOR_FMT_JSON as u8
        } else {
            xfs_fs::XFS_HEALTH_MONITOR_FMT_CSTRUCT as u8
        },
        ..Default::default()
    };

    // SAFETY: Trusting the kernel not to corrupt our memory, and for it to return a valid file
    // description number, which we immediately convert to a File and drop to close the fd.
    unsafe {
        match xfs_ioc_health_monitor(fp.as_raw_fd(), &hminfo) {
            Ok(x) => {
                File::from_raw_fd(x);
                true
            }
            Err(_) => false,
        }
    }
}
