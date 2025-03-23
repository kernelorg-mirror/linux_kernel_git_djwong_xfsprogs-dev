// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
use crate::xfs_fs::xfs_health_monitor;
use nix::ioctl_write_ptr;

pub mod cstruct;
pub mod event;
pub mod fs;
pub mod groups;
pub mod inodes;

ioctl_write_ptr!(xfs_ioc_health_monitor, 'X', 68, xfs_health_monitor);
