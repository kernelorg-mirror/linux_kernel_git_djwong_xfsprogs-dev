// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
use crate::badness;
use crate::display_for_enum;
use crate::healthmon::fs::XfsWholeFsMetadata;
use crate::healthmon::groups::{XfsPeragMetadata, XfsRtgroupMetadata};
use crate::healthmon::inodes::XfsInodeMetadata;
use crate::printlogln;
use crate::weakhandle::WeakHandle;
use crate::xfs_fs;
use crate::xfs_fs::xfs_scrub_metadata;
use crate::xfs_types::{XfsAgNumber, XfsFid, XfsRgNumber};
use crate::xfsprogs;
use crate::xfsprogs::M_;
use anyhow::{Context, Result};
use nix::ioctl_readwrite;
use std::fs::File;
use std::os::fd::AsRawFd;
use std::process::Command;

ioctl_readwrite!(xfs_ioc_scrub_metadata, 'X', 60, xfs_scrub_metadata);

/// Classification information for later reporting
#[derive(Debug, PartialEq)]
enum RepairGroup {
    WholeFs,
    PerAg,
    RtGroup,
    File,
    FullRepair,
}

/// What happened when we tried to repair something?
#[derive(Debug, PartialEq)]
enum RepairOutcome {
    Queued,
    Success,
    Unnecessary,
    MightBeOk,
    Failed,
    Running,
}

display_for_enum!(RepairOutcome, {
    Queued      => M_("Repair queued."),
    Failed      => M_("Repair unsuccessful; offline repair required."),
    MightBeOk   => M_("Seems correct but cross-referencing failed; offline repair recommended."),
    Unnecessary => M_("No modification needed."),
    Success     => M_("Repairs successful."),
    Running     => M_("Repairs in progress."),
});

/// Kernel scrub type code
#[derive(Debug)]
pub struct XfsScrubType(pub u32);

/// Boilerplate to stamp out functions to convert scrub enum to scrub ioctl types;
/// or return an error with the given message.
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
    FsCounters => XFS_SCRUB_TYPE_FSCOUNTERS,
    GrpQuota   => XFS_SCRUB_TYPE_GQUOTA,
    NLinks     => XFS_SCRUB_TYPE_NLINKS,
    PrjQuota   => XFS_SCRUB_TYPE_PQUOTA,
    QuotaCheck => XFS_SCRUB_TYPE_QUOTACHECK,
    UsrQuota   => XFS_SCRUB_TYPE_UQUOTA,
});

/// Boilerplate to stamp out functions to print the scrub type newtype as a pretty string.
macro_rules! display_for_newtype {
    ($newtype:ty , { $($a:ident => $b:expr,)+ } ) => {
        impl std::fmt::Display for $newtype {
            fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
                write!(f, "{}", match self.0 {
                    $(xfs_fs::$a => $b,)+
                    _ => $crate::xfsprogs::M_("unknown")
                })
            }
        }
    };
}

display_for_newtype!(XfsScrubType, {
    XFS_SCRUB_TYPE_PROBE      => M_("probe"),
    XFS_SCRUB_TYPE_SB         => M_("sb"),
    XFS_SCRUB_TYPE_AGF        => M_("agf"),
    XFS_SCRUB_TYPE_AGFL       => M_("agfl"),
    XFS_SCRUB_TYPE_AGI        => M_("agi"),
    XFS_SCRUB_TYPE_BNOBT      => M_("bnobt"),
    XFS_SCRUB_TYPE_CNTBT      => M_("cntbt"),
    XFS_SCRUB_TYPE_INOBT      => M_("inobt"),
    XFS_SCRUB_TYPE_FINOBT     => M_("finobt"),
    XFS_SCRUB_TYPE_RMAPBT     => M_("rmapbt"),
    XFS_SCRUB_TYPE_REFCNTBT   => M_("refcountbt"),
    XFS_SCRUB_TYPE_INODE      => M_("inode"),
    XFS_SCRUB_TYPE_BMBTD      => M_("bmapbtd"),
    XFS_SCRUB_TYPE_BMBTA      => M_("bmapbta"),
    XFS_SCRUB_TYPE_BMBTC      => M_("bmapbtc"),
    XFS_SCRUB_TYPE_DIR        => M_("directory"),
    XFS_SCRUB_TYPE_XATTR      => M_("xattr"),
    XFS_SCRUB_TYPE_SYMLINK    => M_("symlink"),
    XFS_SCRUB_TYPE_PARENT     => M_("parent"),
    XFS_SCRUB_TYPE_RTBITMAP   => M_("rtbitmap"),
    XFS_SCRUB_TYPE_RTSUM      => M_("rtsummary"),
    XFS_SCRUB_TYPE_UQUOTA     => M_("usrquota"),
    XFS_SCRUB_TYPE_GQUOTA     => M_("grpquota"),
    XFS_SCRUB_TYPE_PQUOTA     => M_("prjquota"),
    XFS_SCRUB_TYPE_FSCOUNTERS => M_("fscounters"),
    XFS_SCRUB_TYPE_QUOTACHECK => M_("quotacheck"),
    XFS_SCRUB_TYPE_NLINKS     => M_("nlinks"),
    XFS_SCRUB_TYPE_HEALTHY    => M_("healthy"),
    XFS_SCRUB_TYPE_DIRTREE    => M_("dirtree"),
    XFS_SCRUB_TYPE_METAPATH   => M_("metapath"),
});

/// Information about a repair
pub struct Repair {
    /// Actual details of the repair
    detail: xfs_scrub_metadata,

    /// What group does this belong to?
    group: RepairGroup,

    /// What scrub type did we actually pick?
    scrub_type: XfsScrubType,

    /// What happened when repairs were tried?
    outcome: RepairOutcome,
}

impl Repair {
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
            scrub_type: t,
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
            scrub_type: t,
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
            scrub_type: t,
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
            scrub_type: t,
        }
    }

    /// Schedule the full online fsck
    pub fn full_repair() -> Repair {
        Repair {
            group: RepairGroup::FullRepair,
            detail: xfs_scrub_metadata {
                ..Default::default()
            },
            outcome: RepairOutcome::Queued,
            scrub_type: XfsScrubType(0),
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

    /// Summarize this repair for reporting
    fn summary(&self) -> String {
        match self.group {
            RepairGroup::WholeFs => {
                format!("{} {}", M_("Repair of"), self.scrub_type)
            }
            RepairGroup::PerAg => {
                let agno: XfsAgNumber = self.detail.into();

                format!("{} {} {}", M_("Repair of"), agno, self.scrub_type)
            }
            RepairGroup::RtGroup => {
                let rgno: XfsRgNumber = self.detail.into();

                format!("{} {} {}", M_("Repair of"), rgno, self.scrub_type)
            }
            RepairGroup::File => {
                let fid: XfsFid = self.detail.into();

                format!("{} {} {}", M_("Repair of"), fid, self.scrub_type)
            }
            RepairGroup::FullRepair => M_("Full repair"),
        }
    }

    /// Translate the target of this repair into a filesystem path
    fn repair_path(&self, fh: &WeakHandle) -> String {
        if let RepairGroup::File = self.group {
            let fid: XfsFid = self.detail.into();

            if let Some(path) = fh.path_for(fid) {
                return path.display().to_string();
            }
        }

        fh.mountpoint()
    }

    /// Start the background xfs_scrub service on a filesystem in the hopes that its autofsck
    /// setting allows repairs.  Does not wait for the service to complete.  Multiple activations
    /// while the service runs will be coalesced into a single service instance.
    fn run_full_repair(&self, fh: &WeakHandle) -> Result<bool> {
        let unit_name = fh.instance_unit_name(xfsprogs::XFS_SCRUB_SVCNAME)?;

        let output = Command::new("systemctl")
            .arg("start")
            .arg("--no-block")
            .arg(unit_name)
            .output()?;

        if !output.status.success() {
            return Err(badness!(M_("Could not start xfs_scrub service.")).into());
        }

        Ok(true)
    }

    /// Call the kernel to repair things
    fn repair(&mut self, fh: &WeakHandle) -> Result<bool> {
        if self.group == RepairGroup::FullRepair {
            let started = self
                .run_full_repair(fh)
                .with_context(|| self.summary().to_string())?;
            if started {
                self.outcome = RepairOutcome::Running;
            }
            return Ok(started);
        }

        let fp = fh
            .reopen()
            .with_context(|| M_("Reopening filesystem to repair metadata"))?;

        // SAFETY: Trusting the kernel not to corrupt memory.
        unsafe {
            xfs_ioc_scrub_metadata(fp.as_raw_fd(), &mut self.detail)
                .with_context(|| self.summary().to_string())?;
        }

        self.outcome = Repair::outcome(&self.detail);
        Ok(true)
    }

    /// Try to repair something, or log whatever went wrong
    pub fn perform(&mut self, fh: &WeakHandle) {
        match self.repair(fh) {
            Err(e) => {
                eprintln!("{}: {:#}", self.repair_path(fh), e);
            }
            _ => {
                printlogln!(
                    "{}: {}: {}",
                    self.repair_path(fh),
                    self.summary(),
                    self.outcome
                );

                // Transform into a full repair if we failed to fix things.
                if self.outcome == RepairOutcome::Failed && self.group != RepairGroup::FullRepair {
                    self.group = RepairGroup::FullRepair;
                    self.perform(fh);
                }
            }
        };
    }
}
