// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
use std::io;
use std::time;
use crate::repair;

#[macro_export]
macro_rules! baddata {
    ($left:expr , $right:expr) => ({
        match (&$left, &$right) {
            (left_val, right_val) => {
                let s = format!("{right_val}: {left_val}");
                io::Error::new(io::ErrorKind::InvalidData, s)
            }
        }
    });
}

/// Common behaviors of all health events
pub trait XfsHealthEvent {
    /// Print the health event to stdout
    fn log(&self);

    /// Generate the inputs to a kernel scrub ioctl
    fn schedule_repair(&self) -> Option<Vec<repair::Repair>>;
}

/// Event for the kernel losing events due to us being slow
struct LostEvent {
    /// Timestamp of event
    timestamp: XfsHealthEventTime,
}

impl XfsHealthEvent for LostEvent {
    fn log(&self) {
        println!("{}: events lost", self.timestamp);
    }

    fn schedule_repair(&self) -> Option<Vec<repair::Repair>> {
        None
    }
}

/// Create event for the kernel telling us that it lost an event
pub fn create_lost_event(v: serde_json::Value) ->
        io::Result<Box<dyn XfsHealthEvent>> {
    Ok(Box::new(LostEvent {
        timestamp: XfsHealthEventTime::from_json(&v["time_ns"])?,
    }))
}

/// Health status for metadata events
#[derive(PartialEq, Debug)]
pub enum XfsHealthStatus {
    /// Problems have been observed at runtime
    Sick,

    /// Problems have been observed by xfs_scrub
    Corrupt,

    /// No problems at all
    Healthy,
}

impl XfsHealthStatus {
    /// Convert from json to health status enum
    pub fn from_json(v: &serde_json::Value) -> io::Result<XfsHealthStatus> {
        let s = v.as_str().ok_or(baddata!("Not a string?", v))?;
        match s {
            "sick"      => Ok(XfsHealthStatus::Sick),
            "corrupt"   => Ok(XfsHealthStatus::Corrupt),
            "healthy"   => Ok(XfsHealthStatus::Healthy),
            _ => Err(baddata!("Unknown event status", s)),
        }
    }
}

/// Timestamp of a health event
#[derive(Debug)]
pub struct XfsHealthEventTime {
    /// Internal time representation
    #[allow(dead_code)]
    time: time::SystemTime,
}

impl std::fmt::Display for XfsHealthEventTime {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        let date_time: chrono::DateTime<chrono::Local> = self.time.into();
        write!(f, "{}", date_time.format("%Y-%m-%d %H:%M:%S"))
    }
}

impl XfsHealthEventTime {
    /// Extract the time of an event from a json value
    pub fn from_json(v: &serde_json::Value) -> io::Result<XfsHealthEventTime> {
        let m = v.as_u64().ok_or(baddata!("Not a timestamp?", v))?;
        let time = time::UNIX_EPOCH + time::Duration::from_nanos(m);
        Ok(XfsHealthEventTime { time })
    }
}
