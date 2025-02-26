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
use crate::xfs_fs;
use crate::repair;

/// Metadata types for an allocation group on the data device
#[derive(EnumSetType, Debug)]
#[allow(non_camel_case_types)]
enum XfsPeragMetadata {
    agf,
    agfl,
    agi,
    bnobt,
    cntbt,
    finobt,
    inobt,
    inodes,
    refcountbt,
    rmapbt,
    superblock,
}

impl XfsPeragMetadata {
    /// Convert from json to a metadata type
    pub fn from_json(v: &serde_json::Value) -> io::Result<XfsPeragMetadata> {
        let s = v.as_str().ok_or(baddata!("Not a string?", v))?;
        match s {
            "agf"           => Ok(XfsPeragMetadata::agf),
            "agfl"          => Ok(XfsPeragMetadata::agfl),
            "agi"           => Ok(XfsPeragMetadata::agi),
            "bnobt"         => Ok(XfsPeragMetadata::bnobt),
            "cntbt"         => Ok(XfsPeragMetadata::cntbt),
            "finobt"        => Ok(XfsPeragMetadata::finobt),
            "inobt"         => Ok(XfsPeragMetadata::inobt),
            "inodes"        => Ok(XfsPeragMetadata::inodes),
            "refcountbt"    => Ok(XfsPeragMetadata::refcountbt),
            "rmapbt"        => Ok(XfsPeragMetadata::rmapbt),
            "super"         => Ok(XfsPeragMetadata::superblock),
            _ => Err(baddata!("Unknown AG metadata", s)),
        }
    }

    /// Convert from an array of json to a set of metadata types
    pub fn set_from_json(v: &serde_json::Value) -> io::Result<EnumSet<XfsPeragMetadata>> {
        let array = v.as_array().ok_or(baddata!("Not an array?", v))?;
        let mut set = EnumSet::new();

        for value in array {
            set |= XfsPeragMetadata::from_json(value)?;
        }
        Ok(set)
    }

    /// Convert to scrub type
    fn to_scrub(&self) -> Option<u32> {
        match self {
            XfsPeragMetadata::agf           => Some(xfs_fs::XFS_SCRUB_TYPE_AGF),
            XfsPeragMetadata::agfl          => Some(xfs_fs::XFS_SCRUB_TYPE_AGFL),
            XfsPeragMetadata::agi           => Some(xfs_fs::XFS_SCRUB_TYPE_AGI),
            XfsPeragMetadata::bnobt         => Some(xfs_fs::XFS_SCRUB_TYPE_BNOBT),
            XfsPeragMetadata::cntbt         => Some(xfs_fs::XFS_SCRUB_TYPE_CNTBT),
            XfsPeragMetadata::finobt        => Some(xfs_fs::XFS_SCRUB_TYPE_FINOBT),
            XfsPeragMetadata::inobt         => Some(xfs_fs::XFS_SCRUB_TYPE_INOBT),
            XfsPeragMetadata::refcountbt    => Some(xfs_fs::XFS_SCRUB_TYPE_REFCNTBT),
            XfsPeragMetadata::rmapbt        => Some(xfs_fs::XFS_SCRUB_TYPE_RMAPBT),
            XfsPeragMetadata::superblock    => Some(xfs_fs::XFS_SCRUB_TYPE_SB),
            _ => None,
        }
    }
}

/// Extract group number from a json value
fn xfs_gno(v: &serde_json::Value) -> io::Result<u32> {
    let m = v.as_u64().ok_or(baddata!("Not a group number?", v))?;
    if m > u32::MAX as u64 {
        Err(baddata!("Group number too large", v))
    } else {
        Ok(m as u32)
    }
}

/// XFS perag health event
#[derive(Debug)]
struct XfsPeragEvent {
    /// Allocation group number
    group: u32,

    /// What is being reported on?
    metadata: EnumSet<XfsPeragMetadata>,

    /// Reported state
    status: XfsHealthStatus,

    /// Timestamp of event
    timestamp: XfsHealthEventTime,
}

/// Create a per-AG health event from json
pub fn create_perag_event(v: serde_json::Value) ->
        io::Result<Box<dyn XfsHealthEvent>> {
    Ok(Box::new(XfsPeragEvent {
        group: xfs_gno(&v["group"])?,
        metadata: XfsPeragMetadata::set_from_json(&v["structures"])?,
        timestamp: XfsHealthEventTime::from_json(&v["time_ns"])?,
        status: XfsHealthStatus::from_json(&v["type"])?,
    }))
}

impl XfsHealthEvent for XfsPeragEvent {
    fn log(&self) {
        println!("{}: agno {} metadata {:?} status {:?}",
                 self.timestamp, self.group, self.metadata, self.status);
    }

    fn schedule_repair(&self) -> Option<Vec<repair::Repair>> {
        if self.status != XfsHealthStatus::Sick {
            return None
        }
        let mut ret = Vec::new();
        for f in self.metadata {
            if let Some(sm_type) = f.to_scrub() {
                ret.push(repair::Repair::from_perag(sm_type, self.group));
            }
        }
        Some(ret)
    }
}

/// Metadata types for an allocation group on the realtime device
#[derive(EnumSetType, Debug)]
#[allow(non_camel_case_types)]
enum XfsRtgroupMetadata {
    bitmap,
    summary,
    refcountbt,
    rmapbt,
    superblock,
}

impl XfsRtgroupMetadata {
    /// Convert from json to a metadata type
    pub fn from_json(v: &serde_json::Value) -> io::Result<XfsRtgroupMetadata> {
        let s = v.as_str().ok_or(baddata!("Not a string?", v))?;
        match s {
            "bitmap"        => Ok(XfsRtgroupMetadata::bitmap),
            "summary"       => Ok(XfsRtgroupMetadata::summary),
            "refcountbt"    => Ok(XfsRtgroupMetadata::refcountbt),
            "rmapbt"        => Ok(XfsRtgroupMetadata::rmapbt),
            "super"         => Ok(XfsRtgroupMetadata::superblock),
            _ => Err(baddata!("Unknown rtgroup metadata", s)),
        }
    }

    /// Convert from an array of json to a set of metadata types
    pub fn set_from_json(v: &serde_json::Value) -> io::Result<EnumSet<XfsRtgroupMetadata>> {
        let array = v.as_array().ok_or(baddata!("Not an array?", v))?;
        let mut set = EnumSet::new();

        for value in array {
            set |= XfsRtgroupMetadata::from_json(value)?;
        }
        Ok(set)
    }

    /// Convert to scrub type
    fn to_scrub(&self) -> Option<u32> {
        match self {
            XfsRtgroupMetadata::bitmap      => Some(xfs_fs::XFS_SCRUB_TYPE_RTBITMAP),
            XfsRtgroupMetadata::summary     => Some(xfs_fs::XFS_SCRUB_TYPE_RTSUM),
            XfsRtgroupMetadata::refcountbt  => Some(xfs_fs::XFS_SCRUB_TYPE_RTREFCBT),
            XfsRtgroupMetadata::rmapbt      => Some(xfs_fs::XFS_SCRUB_TYPE_RTRMAPBT),
            XfsRtgroupMetadata::superblock  => Some(xfs_fs::XFS_SCRUB_TYPE_RGSUPER),
        }
    }
}

/// XFS rtgroup health event
#[derive(Debug)]
struct XfsRtgroupEvent {
    /// Allocation group number
    group: u32,

    /// What is being reported on?
    metadata: EnumSet<XfsRtgroupMetadata>,

    /// Reported state
    status: XfsHealthStatus,

    /// Timestamp of event
    timestamp: XfsHealthEventTime,
}

pub fn create_rtgroup_event(v: serde_json::Value) ->
        io::Result<Box<dyn XfsHealthEvent>> {
    Ok(Box::new(XfsRtgroupEvent {
        group: xfs_gno(&v["group"])?,
        metadata: XfsRtgroupMetadata::set_from_json(&v["structures"])?,
        timestamp: XfsHealthEventTime::from_json(&v["time_ns"])?,
        status: XfsHealthStatus::from_json(&v["type"])?,
    }))
}

impl XfsHealthEvent for XfsRtgroupEvent {
    fn log(&self) {
        println!("{}: rgno {} metadata {:?} status {:?}",
                 self.timestamp, self.group, self.metadata, self.status);
    }

    fn schedule_repair(&self) -> Option<Vec<repair::Repair>> {
        if self.status != XfsHealthStatus::Sick {
            return None
        }
        let mut ret = Vec::new();
        for f in self.metadata {
            if let Some(sm_type) = f.to_scrub() {
                ret.push(repair::Repair::from_rtgroup(sm_type, self.group));
            }
        }
        Some(ret)
    }
}
