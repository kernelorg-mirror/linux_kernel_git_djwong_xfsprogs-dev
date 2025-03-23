// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
use crate::xfs_fs;
use crate::xfs_fs::xfs_fsop_geom;
use nix::ioctl_read;
use std::fs::File;
use std::io::Error;
use std::io::Result;
use std::os::fd::AsRawFd;

ioctl_read!(xfs_ioc_fsgeometry, 'X', 126, xfs_fsop_geom);

impl TryFrom<&File> for xfs_fsop_geom {
    type Error = Error;

    /// Retrieve the XFS geometry of an open file.
    fn try_from(fp: &File) -> Result<xfs_fsop_geom> {
        let mut ret: xfs_fsop_geom = Default::default();

        // SAFETY: Trusting the kernel not to corrupt memory.
        unsafe {
            xfs_ioc_fsgeometry(fp.as_raw_fd(), &mut ret)?;
            Ok(ret)
        }
    }
}

impl xfs_fsop_geom {
    /// Does this filesystem have reverse space mappings?
    pub fn has_rmapbt(&self) -> bool {
        self.flags & xfs_fs::XFS_FSOP_GEOM_FLAGS_RMAPBT != 0
    }

    /// Does this filesystem have parent pointers?
    pub fn has_parent(&self) -> bool {
        self.flags & xfs_fs::XFS_FSOP_GEOM_FLAGS_PARENT != 0
    }
}
