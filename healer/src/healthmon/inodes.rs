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

/// Metadata types for an XFS inode
#[derive(EnumSetType, Debug)]
#[allow(non_camel_case_types)]
enum XfsInodeMetadata {
    bmapbta,
    bmapbta_zapped,
    bmapbtc,
    bmapbtd,
    bmapbtd_zapped,
    core,
    directory,
    directory_zapped,
    dirtree,
    parent,
    symlink,
    symlink_zapped,
    xattr,
}

impl XfsInodeMetadata {
    /// Convert from json to a metadata type
    pub fn from_json(v: &serde_json::Value) -> io::Result<XfsInodeMetadata> {
        let s = v.as_str().ok_or(baddata!("Not a string?", v))?;
        match s {
            "bmapbta"           => Ok(XfsInodeMetadata::bmapbta),
            "bmapbta_zapped"    => Ok(XfsInodeMetadata::bmapbta_zapped),
            "bmapbtc"           => Ok(XfsInodeMetadata::bmapbtc),
            "bmapbtd"           => Ok(XfsInodeMetadata::bmapbtd),
            "bmapbtd_zapped"    => Ok(XfsInodeMetadata::bmapbtd_zapped),
            "core"              => Ok(XfsInodeMetadata::core),
            "directory"         => Ok(XfsInodeMetadata::directory),
            "directory_zapped"  => Ok(XfsInodeMetadata::directory_zapped),
            "dirtree"           => Ok(XfsInodeMetadata::dirtree),
            "parent"            => Ok(XfsInodeMetadata::parent),
            "symlink"           => Ok(XfsInodeMetadata::symlink),
            "symlink_zapped"    => Ok(XfsInodeMetadata::symlink_zapped),
            "xattr"             => Ok(XfsInodeMetadata::xattr),
            _ => Err(baddata!("Unknown inode metadata", s)),
        }
    }

    /// Convert from an array of json to a set of metadata types
    pub fn set_from_json(v: &serde_json::Value) -> io::Result<EnumSet<XfsInodeMetadata>> {
        let array = v.as_array().ok_or(baddata!("Not an array?", v))?;
        let mut set = EnumSet::new();

        for value in array {
            set |= XfsInodeMetadata::from_json(value)?;
        }
        Ok(set)
    }
}

/// Extract inode number from a json value
fn xfs_ino(v: &serde_json::Value) -> io::Result<u64> {
    let m = v.as_u64().ok_or(baddata!("Not an inode number?", v))?;
    Ok(m)
}

/// Extract inode generation from a json value
fn xfs_gen(v: &serde_json::Value) -> io::Result<u32> {
    let m = v.as_u64().ok_or(baddata!("Not a generation number?", v))?;
    if m > u32::MAX as u64 {
        Err(baddata!("Generation too large", v))
    } else {
        Ok(m as u32)
    }
}

/// XFS inode health event
#[derive(Debug)]
struct XfsInodeEvent {
    /// Inode number
    ino: u64,

    /// Inode generation
    gen: u32,

    /// What is being reported on?
    metadata: EnumSet<XfsInodeMetadata>,

    /// Reported state
    status: XfsHealthStatus,

    /// Timestamp of event
    timestamp: XfsHealthEventTime,
}

/// Create an inode health event from json
pub fn create_inode_event(v: serde_json::Value) ->
        io::Result<Box<dyn XfsHealthEvent>> {
    Ok(Box::new(XfsInodeEvent {
        ino:        xfs_ino(&v["inumber"])?,
        gen:        xfs_gen(&v["generation"])?,
        metadata:   XfsInodeMetadata::set_from_json(&v["structures"])?,
        timestamp:  XfsHealthEventTime::from_json(&v["time_ns"])?,
        status:     XfsHealthStatus::from_json(&v["type"])?,
    }))
}

impl XfsHealthEvent for XfsInodeEvent {
    fn log(&self) {
        println!("{}: ino {} gen {} metadata {:?} status {:?}",
                 self.timestamp, self.ino, self.gen, self.metadata, self.status);
    }
}

/// File I/O types
#[derive(Debug)]
#[allow(non_camel_case_types)]
enum XfsFileIoErrorType {
    readahead,
    writeback,
    dioread,
    diowrite,
}

impl XfsFileIoErrorType {
    /// Convert from json to a file IO error type
    pub fn from_json(v: &serde_json::Value) -> io::Result<XfsFileIoErrorType> {
        let s = v.as_str().ok_or(baddata!("Not a string?", v))?;
        match s {
            "readahead"     => Ok(XfsFileIoErrorType::readahead),
            "writeback"     => Ok(XfsFileIoErrorType::writeback),
            "dioread"       => Ok(XfsFileIoErrorType::dioread),
            "diowrite"      => Ok(XfsFileIoErrorType::diowrite),
            _ => Err(baddata!("Unknown file I/O error type", s)),
        }
    }
}

/// XFS file I/O error event
#[derive(Debug)]
struct XfsFileIoErrorEvent {
    /// What file I/O went wrong?
    iotype: XfsFileIoErrorType,

    /// Inode number
    ino: u64,

    /// Inode generation
    gen: u32,

    /// Position of I/O error
    pos: u64,

    /// Length of failed I/O
    len: u64,

    /// Timestamp of event
    timestamp: XfsHealthEventTime,
}

/// Extract file position from a json value
fn pos(v: &serde_json::Value) -> io::Result<u64> {
    let m = v.as_u64().ok_or(baddata!("Not a file position?", v))?;
    Ok(m)
}

/// Extract file byte count from a json value
fn len(v: &serde_json::Value) -> io::Result<u64> {
    let m = v.as_u64().ok_or(baddata!("Not a file length?", v))?;
    Ok(m)
}

/// Create a file I/O error event from json
pub fn create_file_io_error_event(v: serde_json::Value) ->
        io::Result<Box<dyn XfsHealthEvent>> {
    Ok(Box::new(XfsFileIoErrorEvent {
        iotype:     XfsFileIoErrorType::from_json(&v["type"])?,
        ino:        xfs_ino(&v["inumber"])?,
        gen:        xfs_gen(&v["generation"])?,
        pos:        pos(&v["pos"])?,
        len:        len(&v["len"])?,
        timestamp:  XfsHealthEventTime::from_json(&v["time_ns"])?,
    }))
}

impl XfsHealthEvent for XfsFileIoErrorEvent {
    fn log(&self) {
        println!("{}: ino {} gen {} op {:?} pos {} len {}",
                 self.timestamp, self.ino, self.gen, self.iotype, self.pos,
                 self.len);
    }
}
