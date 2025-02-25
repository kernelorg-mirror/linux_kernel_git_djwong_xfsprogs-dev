// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
use crate::healthmon::xfs_ioc_health_monitor;
use crate::xfs_fs;
use crate::xfs_fs::xfs_health_monitor;
use serde_json::from_str;
use serde_json::Value;
use std::fmt::Display;
use std::fmt::Formatter;
use std::fs::File;
use std::io::BufRead;
use std::io::BufReader;
use std::io::Lines;
use std::io::Result;
use std::os::fd::AsRawFd;
use std::os::fd::FromRawFd;
use std::path::Path;

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
