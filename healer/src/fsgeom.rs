// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
use std::io;
use std::fs::File;
use std::os::fd::AsRawFd;
use std::mem::MaybeUninit;
use nix::ioctl_read;
use crate::xfs_fs;

ioctl_read!(xfs_ioc_fsgeometry, 'X', 126, xfs_fs::xfs_fsop_geom);

impl TryFrom<&File> for xfs_fs::xfs_fsop_geom {
    type Error = io::Error;

    /// Retrieve the XFS geometry of an open file.
    fn try_from(fp: &File) -> io::Result<xfs_fs::xfs_fsop_geom> {
        unsafe {
            let mut ret: xfs_fs::xfs_fsop_geom = MaybeUninit::zeroed().assume_init();

            xfs_ioc_fsgeometry(fp.as_raw_fd(), &mut ret)?;
            Ok(ret)
        }
    }
}

impl xfs_fs::xfs_fsop_geom {
    /// Does this filesystem have reverse space mappings?
    pub fn has_rmapbt(&self) -> bool {
        self.flags & xfs_fs::XFS_FSOP_GEOM_FLAGS_RMAPBT != 0
    }
    /// Does this filesystem have parent pointers?
    pub fn has_parent(&self) -> bool {
        self.flags & xfs_fs::XFS_FSOP_GEOM_FLAGS_PARENT != 0
    }
}
