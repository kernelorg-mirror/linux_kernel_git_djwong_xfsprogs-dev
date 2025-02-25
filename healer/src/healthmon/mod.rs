// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
use std::io;
use std::io::BufReader;
use std::io::BufRead;
use std::fs::File;
use std::os::fd::AsRawFd;
use std::os::fd::FromRawFd;
use std::mem::MaybeUninit;
use nix::ioctl_write_ptr;
use serde_json;
use crate::xfs_fs;

ioctl_write_ptr!(xfs_ioc_health_monitor, 'X', 68, xfs_fs::xfs_health_monitor);

/// Iterator object that returns health events in json
pub struct XfsHealthMonitor {
    /// health monitor fd, but wrapped to iterate lines as they come in
    lineiter: std::io::Lines<std::io::BufReader<std::fs::File>>,

    /// path to the filesystem mountpoint
    mountpoint: std::path::PathBuf,

    /// are we debugging?
    debug: bool,
}

impl XfsHealthMonitor {
    /// Open a health monitor for an open file on an XFS filesystem
    pub fn try_from(fp: File, mountpoint: &std::path::PathBuf,
                    everything: bool, debug: bool) ->
                    Result<XfsHealthMonitor, io::Error> {
        let health_fp = unsafe {
            let mut hminfo: xfs_fs::xfs_health_monitor = MaybeUninit::zeroed().assume_init();
            hminfo.format = xfs_fs::XFS_HEALTH_MONITOR_FMT_JSON as u8;

            if everything {
                hminfo.flags |= xfs_fs::XFS_HEALTH_MONITOR_VERBOSE as u64;
            }

            let health_fd = xfs_ioc_health_monitor(fp.as_raw_fd(), &hminfo)?;
            File::from_raw_fd(health_fd)
        };
        drop(fp);

        Ok(XfsHealthMonitor {
            lineiter: BufReader::new(health_fp).lines(),
            mountpoint: mountpoint.clone(),
            debug,
        })
    }

    /// Read strings from the health monitor until we have a json blob
    /// describing a health event.
    fn get_event_json(&mut self) -> Option<String> {
        let mut jsonstr = "".to_string();

        loop {
            match self.lineiter.next() {
                // read lines until we encounter a closing brace by itself
                Some(Ok(line))  => {
                    if self.debug {
                        println!("new line: \"{}\"", line);
                    }
                    let done = line == "}";
                    jsonstr.push_str(&line);
                    if done {
                        break
                    }
                    continue
                },

                // ran out of data
                None => return None,

                // io error on the monitoring fd, stop reading
                Some(Err(e)) => {
                    eprintln!("{}: {}", self.mountpoint.display(), e);
                    return None
                }
            }
        }

        if self.debug {
            println!("new event: \"{jsonstr}\"");
        }
        Some(jsonstr)
    }
}

impl Iterator for XfsHealthMonitor {
    type Item = serde_json::Value;

    /// Return health monitoring events
    fn next(&mut self) -> Option<Self::Item> {
        loop {
            let jsonstr = self.get_event_json()?;

            match serde_json::from_str(&jsonstr) {
                Ok(json) => return Some(json),

                // json parsing errors aren't fatal
                Err(e) => {
                    eprintln!("{}: {}", self.mountpoint.display(), e);
                    continue;
                },
            }
        }
    }
}
