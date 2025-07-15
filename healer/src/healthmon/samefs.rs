// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
use crate::healthmon::xfs_ioc_health_samefs;
use crate::xfs_fs::xfs_health_samefs;
use std::fs::File;
use std::os::fd::AsRawFd;
use std::os::raw::c_int;

pub struct SameFs(c_int);

/// Predicate object that unsafely borrows the raw fd from a health monitor to check if a reopened
/// file is actually on the same fs.
impl SameFs {
    /// Create a new predicate from the given raw file descriptor.  Caller must ensure that the
    /// fd is not closed before this object is destroyed.
    pub fn new(fd: c_int) -> SameFs {
        SameFs(fd)
    }

    /// Does this file point to the same filesystem as the health monitor?
    pub fn is_same_fs(&self, fp: &File) -> bool {
        let hms = xfs_health_samefs {
            fd: fp.as_raw_fd(),
            ..Default::default()
        };

        // any error means this isn't the same fs mount
        !matches!(unsafe { xfs_ioc_health_samefs(self.0, &hms) }, Err(_e))
    }
}
