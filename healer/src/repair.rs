// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
use std::io;
use std::os::fd::AsRawFd;
use nix::ioctl_readwrite;
use crate::xfs_fs;
use crate::softhandle;

ioctl_readwrite!(xfs_ioc_scrub_metadata, 'X', 60, xfs_fs::xfs_scrub_metadata);

#[derive(Debug)]
enum RepairGroup {
    WholeFs,
    PerAg,
    RtGroup,
    File,
}

/// Information about a repair
pub struct Repair {
    /// Actual details of the repair
    blob: xfs_fs::xfs_scrub_metadata,

    /// What group does this belong to?
    group: RepairGroup,
}

impl Repair {
    /// Schedule a full-filesystem metadata repair
    pub fn from_whole_fs(sm_type: u32) -> Repair {
        Repair {
            group: RepairGroup::WholeFs,
            blob: xfs_fs::xfs_scrub_metadata {
                sm_type,
                sm_flags: xfs_fs::XFS_SCRUB_IFLAG_REPAIR,
                sm_ino: 0,
                sm_gen: 0,
                sm_agno: 0,
                sm_reserved: [0, 0, 0, 0, 0],
            }
        }
    }

    /// Schedule a per-AG repair
    pub fn from_perag(sm_type: u32, group: u32)  -> Repair {
        Repair {
            group: RepairGroup::PerAg,
            blob: xfs_fs::xfs_scrub_metadata {
                sm_type,
                sm_flags: xfs_fs::XFS_SCRUB_IFLAG_REPAIR,
                sm_ino: 0,
                sm_gen: 0,
                sm_agno: group,
                sm_reserved: [0, 0, 0, 0, 0],
            }
        }
    }

    /// Schedule a rtgroup repair
    pub fn from_rtgroup(sm_type: u32, group: u32) -> Repair {
        Repair {
            group: RepairGroup::RtGroup,
            blob: xfs_fs::xfs_scrub_metadata {
                sm_type,
                sm_flags: xfs_fs::XFS_SCRUB_IFLAG_REPAIR,
                sm_ino: 0,
                sm_gen: 0,
                sm_agno: group,
                sm_reserved: [0, 0, 0, 0, 0],
            }
        }
    }

    /// Schedule a file metadata repair
    pub fn from_file(sm_type: u32, ino: u64, gen: u32) -> Repair {
        Repair {
            group: RepairGroup::File,
            blob: xfs_fs::xfs_scrub_metadata {
                sm_type,
                sm_flags: xfs_fs::XFS_SCRUB_IFLAG_REPAIR,
                sm_ino: ino,
                sm_gen: gen,
                sm_agno: 0,
                sm_reserved: [0, 0, 0, 0, 0],
            }
        }
    }

    /// Return a description of what we tried to repair
    fn name(&self) -> &str {
        match self.blob.sm_type {
            xfs_fs::XFS_SCRUB_TYPE_PROBE => "probe",
            xfs_fs::XFS_SCRUB_TYPE_SB => "sb",
            xfs_fs::XFS_SCRUB_TYPE_AGF => "agf",
            xfs_fs::XFS_SCRUB_TYPE_AGFL => "agfl",
            xfs_fs::XFS_SCRUB_TYPE_AGI => "agi",
            xfs_fs::XFS_SCRUB_TYPE_BNOBT => "bnobt",
            xfs_fs::XFS_SCRUB_TYPE_CNTBT => "cntbt",
            xfs_fs::XFS_SCRUB_TYPE_INOBT => "inobt",
            xfs_fs::XFS_SCRUB_TYPE_FINOBT => "finobt",
            xfs_fs::XFS_SCRUB_TYPE_RMAPBT => "rmapbt",
            xfs_fs::XFS_SCRUB_TYPE_REFCNTBT => "refcountbt",
            xfs_fs::XFS_SCRUB_TYPE_INODE => "inode",
            xfs_fs::XFS_SCRUB_TYPE_BMBTD => "bmapbtd",
            xfs_fs::XFS_SCRUB_TYPE_BMBTA => "bmapbta",
            xfs_fs::XFS_SCRUB_TYPE_BMBTC => "bmapbtc",
            xfs_fs::XFS_SCRUB_TYPE_DIR => "directory",
            xfs_fs::XFS_SCRUB_TYPE_XATTR => "xattr",
            xfs_fs::XFS_SCRUB_TYPE_SYMLINK => "symlink",
            xfs_fs::XFS_SCRUB_TYPE_PARENT => "parent",
            xfs_fs::XFS_SCRUB_TYPE_RTBITMAP => "rtbitmap",
            xfs_fs::XFS_SCRUB_TYPE_RTSUM => "rtsummary",
            xfs_fs::XFS_SCRUB_TYPE_UQUOTA => "usrquota",
            xfs_fs::XFS_SCRUB_TYPE_GQUOTA => "grpquota",
            xfs_fs::XFS_SCRUB_TYPE_PQUOTA => "prjquota",
            xfs_fs::XFS_SCRUB_TYPE_FSCOUNTERS => "fscounters",
            xfs_fs::XFS_SCRUB_TYPE_QUOTACHECK => "quotacheck",
            xfs_fs::XFS_SCRUB_TYPE_NLINKS => "nlinks",
            xfs_fs::XFS_SCRUB_TYPE_HEALTHY => "healthy",
            xfs_fs::XFS_SCRUB_TYPE_DIRTREE => "dirtree",
            xfs_fs::XFS_SCRUB_TYPE_METAPATH => "metapath",
            xfs_fs::XFS_SCRUB_TYPE_RGSUPER => "rgsuper",
            xfs_fs::XFS_SCRUB_TYPE_RTRMAPBT => "rtrmapbt",
            xfs_fs::XFS_SCRUB_TYPE_RTREFCBT => "rtrefcountbt",
            _ => "unknown",
        }
    }

    /// Return a description of what happened
    fn outcome(&self) -> &str {
        if self.blob.sm_flags & (xfs_fs::XFS_SCRUB_OFLAG_CORRUPT |
                                 xfs_fs::XFS_SCRUB_OFLAG_CORRUPT |
                                 xfs_fs::XFS_SCRUB_OFLAG_INCOMPLETE) != 0 {
            return "Repair unsuccessful; offline repair required."
        }

        if self.blob.sm_flags & xfs_fs::XFS_SCRUB_OFLAG_XFAIL != 0 {
            return "Seems correct but cross-referencing failed; offline repair recommended."
        }

        if self.blob.sm_flags & xfs_fs::XFS_SCRUB_OFLAG_NO_REPAIR_NEEDED != 0 {
            return "No modification needed."
        }

        return "Repairs successful."
    }

    /// Call the kernel to repair things
    fn repair(&mut self, fh: &softhandle::SoftHandle) -> io::Result<bool> {
        let fp = fh.reopen()?;

        unsafe { xfs_ioc_scrub_metadata(fp.as_raw_fd(), &mut self.blob)?; }
        Ok(true)
    }

    /// Report whatever we just did
    fn report(&self, fh: &softhandle::SoftHandle) {
        let what = self.name();
        let outcome = self.outcome();

        match self.group {
            RepairGroup::WholeFs => {
                println!("{}: repair of {}: {}", fh.mountpoint(), what, outcome)
            },
            RepairGroup::PerAg => {
                println!("{}: repair of AG {} {}: {}", fh.mountpoint(), what,
                         self.blob.sm_agno, outcome)
            },
            RepairGroup::RtGroup => {
                println!("{}: repair of rtgroup {} {}: {}", fh.mountpoint(),
                         what, self.blob.sm_agno, outcome)
            },
            RepairGroup::File => {
                println!("{}: repair of ino {} gen {} {}: {}", fh.mountpoint(),
                         what, self.blob.sm_ino, self.blob.sm_gen, outcome)
            },
        };
    }

    /// Try to repair something, or log whatever went wrong
    pub fn perform(&mut self, fh: &softhandle::SoftHandle) {
        match self.repair(fh) {
            Err(e) => {
                eprintln!("{}: {}", fh, e);
            },
            _ => {
                self.report(fh);
            },
        };
    }
}
