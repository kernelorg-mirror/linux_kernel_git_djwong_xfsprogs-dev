// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
use crate::healthmon::fs::XfsWholeFsMetadata;
use crate::healthmon::groups::{XfsPeragMetadata, XfsRtgroupMetadata};
use crate::healthmon::inodes::XfsInodeMetadata;
use crate::weakhandle::WeakHandle;
use crate::xfs_fs;
use crate::xfs_fs::xfs_scrub_metadata;
use crate::xfs_types::{XfsAgNumber, XfsFid, XfsRgNumber};
use nix::ioctl_readwrite;
use std::fmt::Display;
use std::fmt::Formatter;
use std::fs::File;
use std::io::Result;
use std::os::fd::AsRawFd;

ioctl_readwrite!(xfs_ioc_scrub_metadata, 'X', 60, xfs_scrub_metadata);

/// Determine if repairs are supported by this kernel
pub fn is_supported(fp: &File) -> bool {
    let mut detail = xfs_scrub_metadata {
        sm_type: xfs_fs::XFS_SCRUB_TYPE_PROBE,
        sm_flags: xfs_fs::XFS_SCRUB_IFLAG_REPAIR,
        ..Default::default()
    };

    // SAFETY: Trusting the kernel not to corrupt memory.
    unsafe { xfs_ioc_scrub_metadata(fp.as_raw_fd(), &mut detail).is_ok() }
}

/// Classification information for later reporting
#[derive(Debug)]
enum RepairGroup {
    WholeFs,
    PerAg,
    RtGroup,
    File,
}

/// What happened when we tried to repair something?
#[derive(Debug)]
enum RepairOutcome {
    Queued,
    Success,
    Unnecessary,
    MightBeOk,
    Failed,
}

impl Display for RepairOutcome {
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        let s = match self {
            RepairOutcome::Queued => "Repair queued.",
            RepairOutcome::Failed => "Repair unsuccessful; offline repair required.",
            RepairOutcome::MightBeOk => {
                "Seems correct but cross-referencing failed; offline repair recommended."
            }
            RepairOutcome::Unnecessary => "No modification needed.",
            RepairOutcome::Success => "Repairs successful.",
        };
        write!(f, "{}", s)
    }
}

/// Kernel scrub type code
#[derive(Debug)]
pub struct XfsScrubType(pub u32);

/// Boilerplate to stamp out functions to convert json array to the given enum
/// type; or return an error with the given message.
macro_rules! metadata_to_scrub_type {
    ($enum_type:ty , { $($a:ident => $b:ident,)+ } ) => {
        impl $enum_type {
            /// Convert to scrub type
            #[allow(unreachable_patterns)]
            pub fn to_scrub(self) -> Option<XfsScrubType> {
                match self {
                    $(<$enum_type>::$a => Some($crate::repair::XfsScrubType($crate::xfs_fs::$b)),)+
                    _ => None,
                }
            }
        }
    };
}

metadata_to_scrub_type!(XfsPeragMetadata, {
    Agf        => XFS_SCRUB_TYPE_AGF,
    Agfl       => XFS_SCRUB_TYPE_AGFL,
    Agi        => XFS_SCRUB_TYPE_AGI,
    Bnobt      => XFS_SCRUB_TYPE_BNOBT,
    Cntbt      => XFS_SCRUB_TYPE_CNTBT,
    Finobt     => XFS_SCRUB_TYPE_FINOBT,
    Inobt      => XFS_SCRUB_TYPE_INOBT,
    Refcountbt => XFS_SCRUB_TYPE_REFCNTBT,
    Rmapbt     => XFS_SCRUB_TYPE_RMAPBT,
    Super      => XFS_SCRUB_TYPE_SB,
});

metadata_to_scrub_type!(XfsRtgroupMetadata, {
    Bitmap     => XFS_SCRUB_TYPE_RTBITMAP,
    Summary    => XFS_SCRUB_TYPE_RTSUM,
    Refcountbt => XFS_SCRUB_TYPE_RTREFCBT,
    Rmapbt     => XFS_SCRUB_TYPE_RTRMAPBT,
    Super      => XFS_SCRUB_TYPE_RGSUPER,
});

metadata_to_scrub_type!(XfsInodeMetadata, {
    Bmapbta   => XFS_SCRUB_TYPE_BMBTA,
    Bmapbtc   => XFS_SCRUB_TYPE_BMBTC,
    Bmapbtd   => XFS_SCRUB_TYPE_BMBTD,
    Core      => XFS_SCRUB_TYPE_INODE,
    Directory => XFS_SCRUB_TYPE_DIR,
    Dirtree   => XFS_SCRUB_TYPE_DIRTREE,
    Parent    => XFS_SCRUB_TYPE_PARENT,
    Symlink   => XFS_SCRUB_TYPE_SYMLINK,
    Xattr     => XFS_SCRUB_TYPE_XATTR,
});

metadata_to_scrub_type!(XfsWholeFsMetadata, {
    FsCounters  => XFS_SCRUB_TYPE_FSCOUNTERS,
    GrpQuota    => XFS_SCRUB_TYPE_GQUOTA,
    NLinks      => XFS_SCRUB_TYPE_NLINKS,
    PrjQuota    => XFS_SCRUB_TYPE_PQUOTA,
    QuotaCheck  => XFS_SCRUB_TYPE_QUOTACHECK,
    UsrQuota    => XFS_SCRUB_TYPE_UQUOTA,
});

/// Information about a repair
pub struct Repair {
    /// Actual details of the repair
    detail: xfs_scrub_metadata,

    /// What group does this belong to?
    group: RepairGroup,

    /// What happened when repairs were tried?
    outcome: RepairOutcome,
}

impl Repair {
    /// Schedule a full-filesystem metadata repair
    pub fn from_whole_fs(t: XfsScrubType) -> Repair {
        Repair {
            group: RepairGroup::WholeFs,
            detail: xfs_scrub_metadata {
                sm_type: t.0,
                sm_flags: xfs_fs::XFS_SCRUB_IFLAG_REPAIR,
                ..Default::default()
            },
            outcome: RepairOutcome::Queued,
        }
    }

    /// Schedule a per-AG repair
    pub fn from_perag(t: XfsScrubType, group: XfsAgNumber) -> Repair {
        Repair {
            group: RepairGroup::PerAg,
            detail: xfs_scrub_metadata {
                sm_type: t.0,
                sm_flags: xfs_fs::XFS_SCRUB_IFLAG_REPAIR,
                sm_agno: group.into(),
                ..Default::default()
            },
            outcome: RepairOutcome::Queued,
        }
    }

    /// Schedule a rtgroup repair
    pub fn from_rtgroup(t: XfsScrubType, group: XfsRgNumber) -> Repair {
        Repair {
            group: RepairGroup::RtGroup,
            detail: xfs_scrub_metadata {
                sm_type: t.0,
                sm_flags: xfs_fs::XFS_SCRUB_IFLAG_REPAIR,
                sm_agno: group.into(),
                ..Default::default()
            },
            outcome: RepairOutcome::Queued,
        }
    }

    /// Schedule a file metadata repair
    pub fn from_file(t: XfsScrubType, fid: XfsFid) -> Repair {
        Repair {
            group: RepairGroup::File,
            detail: xfs_scrub_metadata {
                sm_type: t.0,
                sm_flags: xfs_fs::XFS_SCRUB_IFLAG_REPAIR,
                sm_ino: fid.ino.into(),
                sm_gen: fid.gen.into(),
                ..Default::default()
            },
            outcome: RepairOutcome::Queued,
        }
    }

    /// Return a description of what we tried to repair
    fn name(&self) -> &str {
        match self.detail.sm_type {
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

    /// Decode what happened when we tried to repair
    fn outcome(detail: &xfs_scrub_metadata) -> RepairOutcome {
        const REPAIR_FAILED: u32 =
            xfs_fs::XFS_SCRUB_OFLAG_CORRUPT | xfs_fs::XFS_SCRUB_OFLAG_INCOMPLETE;

        if detail.sm_flags & REPAIR_FAILED != 0 {
            RepairOutcome::Failed
        } else if detail.sm_flags & xfs_fs::XFS_SCRUB_OFLAG_XFAIL != 0 {
            RepairOutcome::MightBeOk
        } else if detail.sm_flags & xfs_fs::XFS_SCRUB_OFLAG_NO_REPAIR_NEEDED != 0 {
            RepairOutcome::Unnecessary
        } else {
            RepairOutcome::Success
        }
    }

    /// Call the kernel to repair things
    fn repair(&mut self, fh: &WeakHandle) -> Result<bool> {
        let fp = fh.reopen()?;

        // SAFETY: Trusting the kernel not to corrupt memory.
        unsafe {
            xfs_ioc_scrub_metadata(fp.as_raw_fd(), &mut self.detail)?;
        }

        self.outcome = Repair::outcome(&self.detail);
        Ok(true)
    }

    /// Report whatever we just did
    fn report(&self, fh: &WeakHandle) {
        let what = self.name();

        match self.group {
            RepairGroup::WholeFs => {
                println!("{}: repair of {}: {}", fh.mountpoint(), what, self.outcome)
            }
            RepairGroup::PerAg => {
                let agno: XfsAgNumber = self.detail.into();

                println!(
                    "{}: repair of {} {}: {}",
                    fh.mountpoint(),
                    agno,
                    what,
                    self.outcome
                )
            }
            RepairGroup::RtGroup => {
                let rgno: XfsRgNumber = self.detail.into();

                println!(
                    "{}: repair of {} {}: {}",
                    fh.mountpoint(),
                    rgno,
                    what,
                    self.outcome
                )
            }
            RepairGroup::File => {
                let fid: XfsFid = self.detail.into();

                println!(
                    "{}: repair of {} {}: {}",
                    fh.mountpoint(),
                    fid,
                    what,
                    self.outcome
                )
            }
        };
    }

    /// Try to repair something, or log whatever went wrong
    pub fn perform(&mut self, fh: &WeakHandle) {
        match self.repair(fh) {
            Err(e) => {
                eprintln!("{}: {}", fh, e);
            }
            _ => {
                self.report(fh);
            }
        };
    }
}
