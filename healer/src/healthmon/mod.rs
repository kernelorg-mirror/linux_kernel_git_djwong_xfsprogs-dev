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

pub mod event;
pub mod group;
pub mod inodes;
pub mod fs;

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

    /// Check if the open file supports a health monitor.
    pub fn is_supported(fp: &File) -> io::Result<bool> {
        unsafe {
            let mut hminfo: xfs_fs::xfs_health_monitor = MaybeUninit::zeroed().assume_init();
            hminfo.format = xfs_fs::XFS_HEALTH_MONITOR_FMT_JSON as u8;

            xfs_ioc_health_monitor(fp.as_raw_fd(), &hminfo)?;
        };
        Ok(true)
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

    fn mount_event_from_json(&mut self, json: serde_json::Value) ->
            Option<Box<dyn event::XfsHealthEvent>> {
        let m = match json["type"].as_str() {
            Some("lost")        => event::create_lost_event(json),
            Some("shutdown")    => fs::create_shutdown_event(json),
            _                   => event::create_lost_event(json),
        };
        match m {
            Err(e) => {
                eprintln!("{}", e);
                return None
            },
            Ok(o) => Some(o),
        }
    }

    fn event_from_json(&mut self, json: serde_json::Value) ->
            Option<Box<dyn event::XfsHealthEvent>> {
        let m = match json["domain"].as_str() {
            Some("rtgroup") => group::create_rtgroup_event(json),
            Some("perag")   => group::create_perag_event(json),
            Some("inode")   => inodes::create_inode_event(json),
            Some("fs")      => fs::create_wholefs_event(json),
            Some("mount")   => return self.mount_event_from_json(json),
            _               => event::create_lost_event(json),
        };
        match m {
            Err(e) => {
                eprintln!("{}", e);
                return None
            },
            Ok(o) => Some(o),
        }
    }
}

impl Iterator for XfsHealthMonitor {
    type Item = Box<dyn event::XfsHealthEvent>;

    /// Return health monitoring events
    fn next(&mut self) -> Option<Self::Item> {
        loop {
            let jsonstr = self.get_event_json()?;

            match serde_json::from_str(&jsonstr) {
                Ok(json) => {
                    // no event object means there was some error that was
                    // already reported; just keep going with the loop
                    match self.event_from_json(json) {
                        None => {
                            continue;
                        },
                        Some(x) => return Some(x),
                    }
                },

                // json parsing errors aren't fatal
                Err(e) => {
                    eprintln!("{}: {}", self.mountpoint.display(), e);
                    continue;
                },
            }
        }
    }
}
