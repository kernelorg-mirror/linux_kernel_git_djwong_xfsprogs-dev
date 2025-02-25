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
use crate::xfs_types::{XfsDevice, XfsPhysRange};
use crate::xfs_types::{XfsFid, XfsFileRange, XfsIgeneration, XfsInumber, XfsIoLen, XfsPos};
use crate::xfs_types::{XfsPeragNumber, XfsRtgroupNumber};
use serde_json::from_str;
use serde_json::Value;
use std::fmt::Display;
use std::fmt::Formatter;
use std::fs::File;
use std::io::BufRead;
use std::io::BufReader;
use std::io::Error;
use std::io::Lines;
use std::io::Result;
use std::os::fd::AsRawFd;
use std::os::fd::FromRawFd;
use std::path::Path;
use std::str::FromStr;

/// Boilerplate to stamp out functions to convert json array to an enumset
/// of the given enum type; or return an error with the given message.
// XXX: Not sure how to make this a TryFrom on EnumSet<T>.
macro_rules! enum_set_from_json {
    ($enum_type:ty , $err_msg:literal) => {
        impl $enum_type {
            /// Convert from an array of json to a set of enum
            pub fn try_set_from(
                v: &serde_json::Value,
            ) -> std::io::Result<enumset::EnumSet<$enum_type>> {
                let array = v.as_array().ok_or(baddata!("Not an array?", v))?;
                let mut set = enumset::EnumSet::new();

                for jsvalue in array {
                    let value = jsvalue.as_str().ok_or(baddata!("Not a string?", jsvalue))?;
                    set |= match <$enum_type>::from_str(value) {
                        Ok(o) => o,
                        Err(_) => return Err(baddata!($err_msg, value)),
                    };
                }
                Ok(set)
            }
        }
    };
}

/// Boilerplate to stamp out functions to convert json array to the given enum
/// type; or return an error with the given message.
macro_rules! enum_from_json {
    ($enum_type:ty , $err_msg:literal) => {
        impl TryFrom<&serde_json::Value> for $enum_type {
            type Error = std::io::Error;

            /// Convert from a json value to an enum
            fn try_from(v: &serde_json::Value) -> std::io::Result<$enum_type> {
                let value = v.as_str().ok_or(baddata!("Not a string?", v))?;
                match <$enum_type>::from_str(value) {
                    Ok(o) => Ok(o),
                    Err(_) => return Err(baddata!($err_msg, value)),
                }
            }
        }
    };
}

/// Iterator object that returns health events in json
pub struct XfsHealthMonitor<'a> {
    /// health monitor fd, but wrapped to iterate lines as they come in
    lineiter: Lines<BufReader<File>>,

    /// path to the filesystem mountpoint
    mountpoint: &'a Path,

    /// are we debugging?
    debug: bool,
}

impl XfsHealthMonitor<'_> {
    /// Open a health monitor for an open file on an XFS filesystem
    pub fn try_new(
        fp: File,
        mountpoint: &Path,
        everything: bool,
        debug: bool,
    ) -> Result<XfsHealthMonitor> {
        let mut hminfo = xfs_health_monitor {
            format: xfs_fs::XFS_HEALTH_MONITOR_FMT_JSON as u8,
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

        Ok(XfsHealthMonitor {
            lineiter: BufReader::new(health_fp).lines(),
            mountpoint,
            debug,
        })
    }
}

/// Raw health event, used to create the real objects
pub struct XfsHealthRawEvent(Vec<String>);

impl XfsHealthRawEvent {
    /// Push a string into the event string collection
    fn push(&mut self, s: String) {
        self.0.push(s)
    }
}

impl TryFrom<XfsHealthRawEvent> for Value {
    type Error = serde_json::Error;

    /// Return a json value from this raw event
    fn try_from(val: XfsHealthRawEvent) -> serde_json::Result<Self> {
        from_str(&val.0.join(""))
    }
}

impl TryFrom<&Value> for XfsPeragNumber {
    type Error = Error;

    /// Extract group number from a json value
    fn try_from(v: &Value) -> Result<Self> {
        let m = v
            .as_u64()
            .ok_or(baddata!("AG number must be integer.", v))?;
        XfsPeragNumber::try_from(m)
    }
}

enum_from_json!(XfsHealthStatus, "Unknown health event status");

enum_set_from_json!(XfsPeragMetadata, "Unknown per-AG metadata");

/// Create a per-AG health event from json
fn perag_event_from_json(v: Value) -> Result<Box<dyn XfsHealthEvent>> {
    Ok(Box::new(XfsPeragEvent::new(
        XfsPeragNumber::try_from(&v["group"])?,
        XfsPeragMetadata::try_set_from(&v["structures"])?,
        XfsHealthStatus::try_from(&v["type"])?,
    )))
}

impl TryFrom<&Value> for XfsRtgroupNumber {
    type Error = Error;

    /// Extract group number from a json value
    fn try_from(v: &Value) -> Result<Self> {
        let m = v
            .as_u64()
            .ok_or(baddata!("rtgroup number must be integer.", v))?;
        XfsRtgroupNumber::try_from(m)
    }
}

enum_set_from_json!(XfsRtgroupMetadata, "Unknown rtgroup metadata");

fn rtgroup_event_from_json(v: Value) -> Result<Box<dyn XfsHealthEvent>> {
    Ok(Box::new(XfsRtgroupEvent::new(
        XfsRtgroupNumber::try_from(&v["group"])?,
        XfsRtgroupMetadata::try_set_from(&v["structures"])?,
        XfsHealthStatus::try_from(&v["type"])?,
    )))
}

/// Convert json values to a fid
fn to_fid(ino: &Value, gen: &Value) -> Result<XfsFid> {
    let i = ino
        .as_u64()
        .ok_or(baddata!("inode number must be integer.", ino))?;
    let g = gen
        .as_u64()
        .ok_or(baddata!("inode generation must be integer.", gen))?;

    Ok(XfsFid {
        ino: XfsInumber::try_from(i)?,
        gen: XfsIgeneration::try_from(g)?,
    })
}

enum_set_from_json!(XfsInodeMetadata, "Unknown inode metadata");

/// Create an inode health event from json
fn inode_event_from_json(v: Value) -> Result<Box<dyn XfsHealthEvent>> {
    Ok(Box::new(XfsInodeEvent::new(
        to_fid(&v["inumber"], &v["generation"])?,
        XfsInodeMetadata::try_set_from(&v["structures"])?,
        XfsHealthStatus::try_from(&v["type"])?,
    )))
}

/// Convert json values to a file range.
fn to_range(pos: &Value, len: &Value) -> Result<XfsFileRange> {
    let p = pos
        .as_u64()
        .ok_or(baddata!("file position must be integer.", pos))?;
    let l = len
        .as_u64()
        .ok_or(baddata!("file length must be integer.", len))?;

    Ok(XfsFileRange {
        pos: XfsPos::try_from(p)?,
        len: XfsIoLen::try_from(l)?,
    })
}

enum_from_json!(XfsFileIoErrorType, "Unknown file I/O error type");

/// Create a file I/O error event from json
pub fn file_io_error_event_from_json(v: Value) -> Result<Box<dyn XfsHealthEvent>> {
    Ok(Box::new(XfsFileIoErrorEvent::new(
        XfsFileIoErrorType::try_from(&v["type"])?,
        to_fid(&v["inumber"], &v["generation"])?,
        to_range(&v["pos"], &v["len"])?,
    )))
}

enum_set_from_json!(XfsWholeFsMetadata, "Unknown whole-fs metadata");

/// Create an whole-fs health event from json
fn wholefs_event_from_json(v: Value) -> Result<Box<dyn XfsHealthEvent>> {
    Ok(Box::new(XfsWholeFsEvent::new(
        XfsWholeFsMetadata::try_set_from(&v["structures"])?,
        XfsHealthStatus::try_from(&v["type"])?,
    )))
}

enum_set_from_json!(XfsShutdownReason, "Unknown shutdown reason");

/// Create a shutdown event from json
fn shutdown_event_from_json(v: Value) -> Result<Box<dyn XfsHealthEvent>> {
    Ok(Box::new(XfsShutdownEvent::new(
        XfsShutdownReason::try_set_from(&v["reasons"])?,
        XfsHealthStatus::try_from(&v["type"])?,
    )))
}

/// Convert json values to a physrange
fn to_phys(dev: &Value, daddr: &Value, bbcount: &Value) -> Result<XfsPhysRange> {
    let a = daddr
        .as_u64()
        .ok_or(baddata!("daddr must be integer.", daddr))?;
    let b = bbcount
        .as_u64()
        .ok_or(baddata!("bbcount must be integer.", bbcount))?;

    Ok(XfsPhysRange {
        device: XfsDevice::try_from(dev)?,
        daddr: a.into(),
        bbcount: b.into(),
    })
}

enum_from_json!(XfsDevice, "Unknown XFS device");

/// Create a media error event from json
fn media_error_event_from_json(v: Value) -> Result<Box<dyn XfsHealthEvent>> {
    Ok(Box::new(XfsMediaErrorEvent::new(to_phys(
        &v["domain"],
        &v["daddr"],
        &v["bbcount"],
    )?)))
}

/// Create event for the kernel telling us that it lost an event
fn lost_event_from_json(v: Value) -> Result<Box<dyn XfsHealthEvent>> {
    let r = &v["count"];
    let count = r.as_u64().ok_or(baddata!("Not a count?", r))?;

    Ok(Box::new(LostEvent::new(count)))
}

impl XfsHealthRawEvent {
    /// Return an event object that can react to a health event.
    pub fn cook(self) -> Result<Box<dyn XfsHealthEvent>> {
        let json = Value::try_from(self)?;
        match json["domain"].as_str() {
            Some("rtgroup") => rtgroup_event_from_json(json),
            Some("perag") => perag_event_from_json(json),
            Some("inode") => inode_event_from_json(json),
            Some("fs") => wholefs_event_from_json(json),
            Some("mount") => match json["type"].as_str() {
                Some("lost") => lost_event_from_json(json),
                Some("shutdown") => shutdown_event_from_json(json),
                Some("unmount") => Ok(Box::new(XfsUnmountEvent {})),
                Some("running") => Ok(Box::new(RunningEvent {})),
                _ => Ok(Box::new(UnknownEvent {})),
            },
            Some("datadev") | Some("rtdev") | Some("logdev") => media_error_event_from_json(json),

            Some("filerange") => file_io_error_event_from_json(json),

            _ => Ok(Box::new(UnknownEvent {})),
        }
    }
}

impl Display for XfsHealthRawEvent {
    /// Turn this collection of strings into a single string
    fn fmt(&self, f: &mut Formatter) -> std::fmt::Result {
        write!(f, "{}", self.0.join(""))
    }
}

impl Iterator for XfsHealthMonitor<'_> {
    type Item = XfsHealthRawEvent;

    /// Return health monitoring events
    fn next(&mut self) -> Option<Self::Item> {
        let mut ret = XfsHealthRawEvent(Vec::new());
        loop {
            match self.lineiter.next() {
                // read lines until we encounter a closing brace by itself
                Some(Ok(line)) => {
                    if self.debug {
                        println!("new line: \"{}\"", line);
                    }
                    let done = line == "}";
                    ret.push(line);
                    if done {
                        break;
                    }
                    continue;
                }

                // ran out of data
                None => return None,

                // io error on the monitoring fd, stop reading
                Some(Err(e)) => {
                    eprintln!("{}: {}", self.mountpoint.display(), e);
                    return None;
                }
            }
        }
        Some(ret)
    }
}
