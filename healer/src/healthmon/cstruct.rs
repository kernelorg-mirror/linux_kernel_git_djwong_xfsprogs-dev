// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
use crate::baddata;
use crate::healthmon::event::LostEvent;
use crate::healthmon::event::RunningEvent;
use crate::healthmon::event::UnknownEvent;
use crate::healthmon::event::XfsHealthEvent;
use crate::healthmon::event::XfsHealthStatus;
use crate::healthmon::fs::XfsMediaErrorEvent;
use crate::healthmon::fs::XfsUnmountEvent;
use crate::healthmon::fs::{XfsShutdownEvent, XfsShutdownReason};
use crate::healthmon::fs::{XfsWholeFsEvent, XfsWholeFsMetadata};
use crate::healthmon::groups::{XfsPeragEvent, XfsPeragMetadata};
use crate::healthmon::groups::{XfsRtgroupEvent, XfsRtgroupMetadata};
use crate::healthmon::inodes::{XfsFileIoErrorEvent, XfsFileIoErrorType};
use crate::healthmon::inodes::{XfsInodeEvent, XfsInodeMetadata};
use crate::healthmon::xfs_ioc_health_monitor;
use crate::xfs_fs;
use crate::xfs_fs::xfs_health_monitor;
use crate::xfs_fs::xfs_health_monitor_event;
use crate::xfs_types::{XfsAgNumber, XfsRgNumber};
use crate::xfs_types::{XfsDevice, XfsPhysRange};
use crate::xfs_types::{XfsFid, XfsFileRange, XfsIgeneration, XfsIno, XfsIoLen, XfsPos};
use crate::xfsprogs::M_;
use anyhow::{Context, Result};
use std::fs::File;
use std::io::BufReader;
use std::io::ErrorKind;
use std::io::Read;
use std::os::fd::AsRawFd;
use std::os::fd::FromRawFd;
use std::path::Path;

/// Boilerplate to stamp out functions to convert a u32 mask to an enumset
/// of the given enum type.
macro_rules! enum_set_from_mask {
    ($enum_type:ty , $err_msg:expr , { $($a:ident => $b:ident,)+ } ) => {
        impl $enum_type {
            /// Convert from a bitmask to a set of enum
            pub fn from_mask(mask: u32) -> std::io::Result<enumset::EnumSet<$enum_type>> {
                let mut ret = enumset::EnumSet::new();
                let badmask = 0 |
                $($crate::xfs_fs::$a | )+
                0;
                if mask & !badmask != 0 { return Err(baddata!($err_msg, $enum_type, mask)); }
                $(if mask & $crate::xfs_fs::$a != 0 { ret |= <$enum_type>::$b; })+
                Ok(ret)
            }
        }
    };
}

/// Boilerplate to stamp out functions to convert a u32 field to the given enum
/// type.
macro_rules! enum_from_field {
    ($enum_type:ty , { $($a:ident => $b:ident,)+ } ) => {
        impl $enum_type {
            /// Convert from a u32 field to an enum
            pub fn from_value(value: u32) -> std::io::Result<$enum_type> {
                $(if value == $crate::xfs_fs::$a { return Ok(<$enum_type>::$b); })+
                Err(baddata!($crate::xfsprogs::M_("Unknown value"), $enum_type, value))
            }
        }
    };
}

/// Iterator object that returns health events in binary
pub struct CStructMonitor<'a> {
    /// health monitor fd
    objiter: BufReader<File>,

    /// path to the filesystem mountpoint
    mountpoint: &'a Path,
}

impl CStructMonitor<'_> {
    /// Open a health monitor for an open file on an XFS filesystem
    pub fn try_new(fp: File, mountpoint: &Path, everything: bool) -> Result<CStructMonitor> {
        let mut hminfo = xfs_health_monitor {
            format: xfs_fs::XFS_HEALTH_MONITOR_FMT_CSTRUCT as u8,
            ..Default::default()
        };

        if everything {
            hminfo.flags |= xfs_fs::XFS_HEALTH_MONITOR_VERBOSE as u64;
        }

        // SAFETY: Trusting the kernel ioctl not to corrupt stack contents, and to return us a valid
        // file description number.
        let health_fp = unsafe {
            let health_fd = xfs_ioc_health_monitor(fp.as_raw_fd(), &hminfo)?;
            File::from_raw_fd(health_fd)
        };
        drop(fp);

        Ok(CStructMonitor {
            objiter: BufReader::new(health_fp),
            mountpoint,
        })
    }
}

enum_from_field!(XfsHealthStatus, {
    XFS_HEALTH_MONITOR_TYPE_SICK    => Sick,
    XFS_HEALTH_MONITOR_TYPE_CORRUPT => Corrupt,
    XFS_HEALTH_MONITOR_TYPE_HEALTHY => Healthy,
});

enum_set_from_mask!(XfsPeragMetadata, M_("Unknown per-AG metadata"), {
    XFS_AG_GEOM_SICK_AGF      => Agf,
    XFS_AG_GEOM_SICK_AGFL     => Agfl,
    XFS_AG_GEOM_SICK_AGI      => Agi,
    XFS_AG_GEOM_SICK_BNOBT    => Bnobt,
    XFS_AG_GEOM_SICK_CNTBT    => Cntbt,
    XFS_AG_GEOM_SICK_FINOBT   => Finobt,
    XFS_AG_GEOM_SICK_INOBT    => Inobt,
    XFS_AG_GEOM_SICK_INODES   => Inodes,
    XFS_AG_GEOM_SICK_REFCNTBT => Refcountbt,
    XFS_AG_GEOM_SICK_RMAPBT   => Rmapbt,
    XFS_AG_GEOM_SICK_SB       => Super,
});

/// Create a per-AG health event from C structure
fn perag_event_from_cstruct(v: xfs_health_monitor_event) -> Result<Box<dyn XfsHealthEvent>> {
    // SAFETY: Union access checked by caller
    let ge = unsafe { v.e.group };

    Ok(Box::new(XfsPeragEvent::new(
        XfsAgNumber::try_from(ge.gno as u64).with_context(|| M_("Reading per-AG event"))?,
        XfsPeragMetadata::from_mask(ge.mask).with_context(|| M_("Reading per-AG event"))?,
        XfsHealthStatus::from_value(v.type_).with_context(|| M_("Reading per-AG event"))?,
    )))
}

/// Create a rtgroup health event from C structure
fn rtgroup_event_from_cstruct(v: xfs_health_monitor_event) -> Result<Box<dyn XfsHealthEvent>> {
    // SAFETY: Union access checked by caller
    let ge = unsafe { v.e.group };

    Ok(Box::new(XfsRtgroupEvent::new(
        XfsRgNumber::try_from(ge.gno as u64).with_context(|| M_("Reading rtgroup event"))?,
        XfsRtgroupMetadata::from_mask(ge.mask).with_context(|| M_("Reading rtgroup event"))?,
        XfsHealthStatus::from_value(v.type_).with_context(|| M_("Reading rtgroup event"))?,
    )))
}

enum_set_from_mask!(XfsRtgroupMetadata, M_("Unknown rtgroup metadata"), {
    XFS_RTGROUP_GEOM_SICK_BITMAP   => Bitmap,
    XFS_RTGROUP_GEOM_SICK_SUMMARY  => Summary,
    XFS_RTGROUP_GEOM_SICK_REFCNTBT => Refcountbt,
    XFS_RTGROUP_GEOM_SICK_RMAPBT   => Rmapbt,
    XFS_RTGROUP_GEOM_SICK_SUPER    => Super,
});

enum_set_from_mask!(XfsInodeMetadata, M_("Unknown inode metadata"), {
    XFS_BS_SICK_BMBTA          => Bmapbta,
    XFS_BS_SICK_BMBTC          => Bmapbtc,
    XFS_BS_SICK_BMBTD          => Bmapbtd,
    XFS_BS_SICK_INODE          => Core,
    XFS_BS_SICK_DIR            => Directory,
    XFS_BS_SICK_DIRTREE        => Dirtree,
    XFS_BS_SICK_PARENT         => Parent,
    XFS_BS_SICK_SYMLINK        => Symlink,
    XFS_BS_SICK_XATTR          => Xattr,
});

/// Create an inode health event from C structure
fn inode_event_from_cstruct(v: xfs_health_monitor_event) -> Result<Box<dyn XfsHealthEvent>> {
    // SAFETY: Union access checked by caller
    let ie = unsafe { v.e.inode };

    Ok(Box::new(XfsInodeEvent::new(
        XfsFid {
            ino: XfsIno::try_from(ie.ino).with_context(|| M_("Reading inode event"))?,
            gen: XfsIgeneration::try_from(ie.gen as u64)
                .with_context(|| M_("Reading inode event"))?,
        },
        XfsInodeMetadata::from_mask(ie.mask).with_context(|| M_("Reading inode event"))?,
        XfsHealthStatus::from_value(v.type_).with_context(|| M_("Reading inode event"))?,
    )))
}

enum_from_field!(XfsFileIoErrorType, {
    XFS_HEALTH_MONITOR_TYPE_BUFREAD  => Readahead,
    XFS_HEALTH_MONITOR_TYPE_BUFWRITE => Writeback,
    XFS_HEALTH_MONITOR_TYPE_DIOREAD  => DirectioRead,
    XFS_HEALTH_MONITOR_TYPE_DIOWRITE => DirectioWrite,
});

/// Create a file I/O error event from a C struct
fn file_io_error_event_from_cstruct(
    v: xfs_health_monitor_event,
) -> Result<Box<dyn XfsHealthEvent>> {
    // SAFETY: Union access checked by caller
    let fe = unsafe { v.e.filerange };

    Ok(Box::new(XfsFileIoErrorEvent::new(
        XfsFileIoErrorType::from_value(v.type_).with_context(|| M_("Reading file I/O event"))?,
        XfsFid {
            ino: XfsIno::try_from(fe.ino).with_context(|| M_("Reading file I/O event"))?,
            gen: XfsIgeneration::try_from(fe.gen as u64)
                .with_context(|| M_("Reading file I/O event"))?,
        },
        XfsFileRange {
            pos: XfsPos::try_from(fe.pos).with_context(|| M_("Reading file I/O event"))?,
            len: XfsIoLen::try_from(fe.len).with_context(|| M_("Reading file I/O event"))?,
        },
    )))
}

enum_set_from_mask!(XfsWholeFsMetadata, M_("Unknown whole-fs metadata"), {
    XFS_FSOP_GEOM_SICK_COUNTERS   => FsCounters,
    XFS_FSOP_GEOM_SICK_GQUOTA     => GrpQuota,
    XFS_FSOP_GEOM_SICK_NLINKS     => NLinks,
    XFS_FSOP_GEOM_SICK_PQUOTA     => PrjQuota,
    XFS_FSOP_GEOM_SICK_QUOTACHECK => QuotaCheck,
    XFS_FSOP_GEOM_SICK_UQUOTA     => UsrQuota,
    XFS_FSOP_GEOM_SICK_METADIR    => MetaDir,
    XFS_FSOP_GEOM_SICK_METAPATH   => MetaPath,
});

/// Create an whole-fs health event from a C struct
fn wholefs_event_from_cstruct(v: xfs_health_monitor_event) -> Result<Box<dyn XfsHealthEvent>> {
    // SAFETY: Union access checked by caller
    let fe = unsafe { v.e.fs };

    Ok(Box::new(XfsWholeFsEvent::new(
        XfsWholeFsMetadata::from_mask(fe.mask).with_context(|| M_("Reading whole-fs event"))?,
        XfsHealthStatus::from_value(v.type_).with_context(|| M_("Reading whole-fs event"))?,
    )))
}

enum_set_from_mask!(XfsShutdownReason, M_("Unknown fs shutdown reason"), {
    XFS_HEALTH_SHUTDOWN_META_IO_ERROR  => MetaIoerr,
    XFS_HEALTH_SHUTDOWN_LOG_IO_ERROR   => LogIoerr,
    XFS_HEALTH_SHUTDOWN_FORCE_UMOUNT   => ForceUmount,
    XFS_HEALTH_SHUTDOWN_CORRUPT_INCORE => CorruptIncore,
    XFS_HEALTH_SHUTDOWN_CORRUPT_ONDISK => CorruptOndisk,
    XFS_HEALTH_SHUTDOWN_DEVICE_REMOVED => DeviceRemoved,
});

/// Create an shutdown event from a C struct
fn shutdown_event_from_cstruct(v: xfs_health_monitor_event) -> Result<Box<dyn XfsHealthEvent>> {
    // SAFETY: Union access checked by caller
    let se = unsafe { v.e.shutdown };

    Ok(Box::new(XfsShutdownEvent::new(
        XfsShutdownReason::from_mask(se.reasons)
            .with_context(|| M_("Reading fs shutdown event"))?,
    )))
}

enum_from_field!(XfsDevice, {
    XFS_HEALTH_MONITOR_DOMAIN_DATADEV => Data,
    XFS_HEALTH_MONITOR_DOMAIN_RTDEV   => Realtime,
    XFS_HEALTH_MONITOR_DOMAIN_LOGDEV  => Log,
});

/// Create a media error event from a C struct
fn media_error_event_from_cstruct(v: xfs_health_monitor_event) -> Result<Box<dyn XfsHealthEvent>> {
    // SAFETY: Union access checked by caller
    let me = unsafe { v.e.media };

    Ok(Box::new(XfsMediaErrorEvent::new(XfsPhysRange {
        device: XfsDevice::from_value(v.domain).with_context(|| M_("Reading media error event"))?,
        daddr: me.daddr.into(),
        bbcount: me.bbcount.into(),
    })))
}

/// Create event for the kernel telling us that it lost an event
fn lost_event_from_cstruct(v: xfs_health_monitor_event) -> Result<Box<dyn XfsHealthEvent>> {
    // SAFETY: Union access checked by caller
    let le = unsafe { v.e.lost };

    Ok(Box::new(LostEvent::new(le.count)))
}

impl xfs_health_monitor_event {
    /// Return an event object that can react to a health event.
    pub fn cook(self) -> Result<Box<dyn XfsHealthEvent>> {
        match self.domain {
            xfs_fs::XFS_HEALTH_MONITOR_DOMAIN_RTGROUP => rtgroup_event_from_cstruct(self),

            xfs_fs::XFS_HEALTH_MONITOR_DOMAIN_AG => perag_event_from_cstruct(self),

            xfs_fs::XFS_HEALTH_MONITOR_DOMAIN_INODE => inode_event_from_cstruct(self),

            xfs_fs::XFS_HEALTH_MONITOR_DOMAIN_FS => wholefs_event_from_cstruct(self),

            xfs_fs::XFS_HEALTH_MONITOR_DOMAIN_MOUNT => match self.type_ {
                xfs_fs::XFS_HEALTH_MONITOR_TYPE_LOST => lost_event_from_cstruct(self),

                xfs_fs::XFS_HEALTH_MONITOR_TYPE_SHUTDOWN => shutdown_event_from_cstruct(self),

                xfs_fs::XFS_HEALTH_MONITOR_TYPE_UNMOUNT => Ok(Box::new(XfsUnmountEvent {})),

                xfs_fs::XFS_HEALTH_MONITOR_TYPE_RUNNING => Ok(Box::new(RunningEvent {})),

                _ => Ok(Box::new(UnknownEvent {})),
            },

            xfs_fs::XFS_HEALTH_MONITOR_DOMAIN_DATADEV
            | xfs_fs::XFS_HEALTH_MONITOR_DOMAIN_LOGDEV
            | xfs_fs::XFS_HEALTH_MONITOR_DOMAIN_RTDEV => media_error_event_from_cstruct(self),

            xfs_fs::XFS_HEALTH_MONITOR_DOMAIN_FILERANGE => file_io_error_event_from_cstruct(self),

            _ => Ok(Box::new(UnknownEvent {})),
        }
    }
}

impl Iterator for CStructMonitor<'_> {
    type Item = xfs_health_monitor_event;

    /// Return health monitoring events
    fn next(&mut self) -> Option<Self::Item> {
        let sz = std::mem::size_of::<xfs_health_monitor_event>();
        let mut buf: Vec<u8> = vec![0; sz];
        if let Err(e) = self.objiter.read_exact(&mut buf) {
            if e.kind() != ErrorKind::UnexpectedEof {
                eprintln!(
                    "{}: {}: {:#}",
                    self.mountpoint.display(),
                    M_("Reading event blob"),
                    e
                );
            }
            return None;
        };

        let hme: *const xfs_health_monitor_event = buf.as_ptr() as *const xfs_health_monitor_event;

        // SAFETY: Copying from a Vec that we sized to fit one xfs_health_monitor_event into an
        // object of that type.
        let ret: xfs_health_monitor_event = unsafe { *hme };
        Some(ret)
    }
}
