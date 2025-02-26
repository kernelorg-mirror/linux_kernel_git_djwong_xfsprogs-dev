// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
use std::io;
use serde_json;
use enumset::EnumSet;
use enumset::EnumSetType;
use crate::healthmon::event::XfsHealthStatus;
use crate::healthmon::event::XfsHealthEvent;
use crate::healthmon::event::XfsHealthEventTime;
use crate::baddata;
use crate::repair;
use crate::xfs_fs;

/// Metadata types for an XFS whole-fs metadata
#[derive(EnumSetType, Debug)]
#[allow(non_camel_case_types)]
enum XfsWholeFsMetadata {
    fscounters,
    grpquota,
    metadir,
    metapath,
    nlinks,
    prjquota,
    quotacheck,
    usrquota,
}

impl XfsWholeFsMetadata {
    /// Convert from json to a metadata type
    pub fn from_json(v: &serde_json::Value) -> io::Result<XfsWholeFsMetadata> {
        let s = v.as_str().ok_or(baddata!("Not a string?", v))?;
        match s {
            "fscounters"    => Ok(XfsWholeFsMetadata::fscounters),
            "grpquota"      => Ok(XfsWholeFsMetadata::grpquota),
            "metadir"       => Ok(XfsWholeFsMetadata::metadir),
            "metapath"      => Ok(XfsWholeFsMetadata::metapath),
            "nlinks"        => Ok(XfsWholeFsMetadata::nlinks),
            "prjquota"      => Ok(XfsWholeFsMetadata::prjquota),
            "quotacheck"    => Ok(XfsWholeFsMetadata::quotacheck),
            "usrquota"      => Ok(XfsWholeFsMetadata::usrquota),
            _ => Err(baddata!("Unknown whole-fs metadata", s)),
        }
    }

    /// Convert from an array of json to a set of metadata types
    pub fn set_from_json(v: &serde_json::Value) -> io::Result<EnumSet<XfsWholeFsMetadata>> {
        let array = v.as_array().ok_or(baddata!("Not an array?", v))?;
        let mut set = EnumSet::new();

        for value in array {
            set |= XfsWholeFsMetadata::from_json(value)?;
        }
        Ok(set)
    }

    /// Convert to scrub type
    fn to_scrub(&self) -> Option<u32> {
        match self {
            XfsWholeFsMetadata::fscounters  => Some(xfs_fs::XFS_SCRUB_TYPE_FSCOUNTERS),
            XfsWholeFsMetadata::grpquota    => Some(xfs_fs::XFS_SCRUB_TYPE_GQUOTA),
            XfsWholeFsMetadata::nlinks      => Some(xfs_fs::XFS_SCRUB_TYPE_NLINKS),
            XfsWholeFsMetadata::prjquota    => Some(xfs_fs::XFS_SCRUB_TYPE_PQUOTA),
            XfsWholeFsMetadata::quotacheck  => Some(xfs_fs::XFS_SCRUB_TYPE_QUOTACHECK),
            XfsWholeFsMetadata::usrquota    => Some(xfs_fs::XFS_SCRUB_TYPE_UQUOTA),
            _ => None
        }
    }
}

/// XFS whole-fs health event
#[derive(Debug)]
struct XfsWholeFsEvent {
    /// What is being reported on?
    metadata: EnumSet<XfsWholeFsMetadata>,

    /// Reported state
    status: XfsHealthStatus,

    /// Timestamp of event
    timestamp: XfsHealthEventTime,
}

/// Create an whole-fs health event from json
pub fn create_wholefs_event(v: serde_json::Value) ->
        io::Result<Box<dyn XfsHealthEvent>> {
    Ok(Box::new(XfsWholeFsEvent {
        metadata:   XfsWholeFsMetadata::set_from_json(&v["structures"])?,
        timestamp:  XfsHealthEventTime::from_json(&v["time_ns"])?,
        status:     XfsHealthStatus::from_json(&v["type"])?,
    }))
}

impl XfsHealthEvent for XfsWholeFsEvent {
    fn log(&self) {
        println!("{}: metadata {:?} status {:?}",
                 self.timestamp, self.metadata, self.status);
    }

    fn schedule_repair(&self) -> Option<Vec<repair::Repair>> {
        if self.status != XfsHealthStatus::Sick {
            return None
        }
        let mut ret = Vec::new();
        for f in self.metadata {
            if let Some(sm_type) = f.to_scrub() {
                ret.push(repair::Repair::from_whole_fs(sm_type));
            }
        }
        Some(ret)
    }
}

/// Reasons for a filesystem shutdown event
#[derive(EnumSetType, Debug)]
#[allow(non_camel_case_types)]
enum XfsShutdownReason {
    corrupt_incore,
    corrupt_ondisk,
    device_removed,
    force_umount,
    log_ioerr,
    meta_ioerr,
}

impl XfsShutdownReason {
    /// Convert from json to a shutdown reason
    pub fn from_json(v: &serde_json::Value) -> io::Result<XfsShutdownReason> {
        let s = v.as_str().ok_or(baddata!("Not a string?", v))?;
        match s {
            "corrupt_incore"    => Ok(XfsShutdownReason::corrupt_incore),
            "corrupt_ondisk"    => Ok(XfsShutdownReason::corrupt_ondisk),
            "device_removed"    => Ok(XfsShutdownReason::device_removed),
            "force_umount"      => Ok(XfsShutdownReason::force_umount),
            "log_ioerr"         => Ok(XfsShutdownReason::log_ioerr),
            "meta_ioerr"        => Ok(XfsShutdownReason::meta_ioerr),
            _ => Err(baddata!("Unknown shutdown reason", s)),
        }
    }

    /// Convert from an array of json to a set of metadata types
    pub fn set_from_json(v: &serde_json::Value) -> io::Result<EnumSet<XfsShutdownReason>> {
        let array = v.as_array().ok_or(baddata!("Not an array?", v))?;
        let mut set = EnumSet::new();

        for value in array {
            set |= XfsShutdownReason::from_json(value)?;
        }
        Ok(set)
    }
}

/// XFS shutdown health event
#[derive(Debug)]
struct XfsShutdownEvent {
    /// What did the filesystem shut down?
    reasons: EnumSet<XfsShutdownReason>,

    /// Reported state
    status: XfsHealthStatus,

    /// Timestamp of event
    timestamp: XfsHealthEventTime,
}

/// Create a shutdown event from json
pub fn create_shutdown_event(v: serde_json::Value) ->
        io::Result<Box<dyn XfsHealthEvent>> {
    Ok(Box::new(XfsShutdownEvent {
        reasons:    XfsShutdownReason::set_from_json(&v["reasons"])?,
        timestamp:  XfsHealthEventTime::from_json(&v["time_ns"])?,
        status:     XfsHealthStatus::from_json(&v["type"])?,
    }))
}

impl XfsHealthEvent for XfsShutdownEvent {
    fn log(&self) {
        println!("{}: reasons {:?} status {:?}",
                 self.timestamp, self.reasons, self.status);
    }

    fn schedule_repair(&self) -> Option<Vec<repair::Repair>> {
        None
    }
}

/// Devices
#[derive(Debug)]
#[allow(non_camel_case_types)]
enum XfsDevice {
    datadev,
    logdev,
    rtdev,
}

impl XfsDevice {
    /// Convert from json to an XFS device
    pub fn from_json(v: &serde_json::Value) -> io::Result<XfsDevice> {
        let s = v.as_str().ok_or(baddata!("Not a string?", v))?;
        match s {
            "datadev"   => Ok(XfsDevice::datadev),
            "logdev"    => Ok(XfsDevice::logdev),
            "rtdev"     => Ok(XfsDevice::rtdev),
            _ => Err(baddata!("Unknown XFS device", s)),
        }
    }
}

/// Media error event
#[derive(Debug)]
struct XfsMediaErrorEvent {
    /// Which device is being reported on?
    device: XfsDevice,

    /// Start of the error, in 512b units
    daddr: u64,

    /// Size of affected range, in 512b units
    bbcount: u64,

    /// Timestamp of event
    timestamp: XfsHealthEventTime,
}

/// Extract disk address from a json value
fn daddr(v: &serde_json::Value) -> io::Result<u64> {
    let m = v.as_u64().ok_or(baddata!("Not an disk address?", v))?;
    Ok(m)
}

/// Extract disk block count from a json value
fn bbcount(v: &serde_json::Value) -> io::Result<u64> {
    let m = v.as_u64().ok_or(baddata!("Not a basic block count?", v))?;
    Ok(m)
}

/// Create a media error event from json
pub fn create_media_error_event(v: serde_json::Value) ->
        io::Result<Box<dyn XfsHealthEvent>> {
    Ok(Box::new(XfsMediaErrorEvent {
        device:     XfsDevice::from_json(&v["domain"])?,
        daddr:      daddr(&v["daddr"])?,
        bbcount:    bbcount(&v["bbcount"])?,
        timestamp:  XfsHealthEventTime::from_json(&v["time_ns"])?,
    }))
}

impl XfsHealthEvent for XfsMediaErrorEvent {
    fn log(&self) {
        println!("{}: device {:?} daddr {} bbcount {}",
                 self.timestamp, self.device, self.daddr, self.bbcount);
    }

    fn schedule_repair(&self) -> Option<Vec<repair::Repair>> {
        None
    }
}
