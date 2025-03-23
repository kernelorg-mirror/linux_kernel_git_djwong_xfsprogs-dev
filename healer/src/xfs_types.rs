// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
use crate::baddata;
use crate::display_for_enum;
use crate::xfs_fs::xfs_scrub_metadata;
use crate::xfsprogs::M_;
use anyhow::{Error, Result};
use std::fmt::Display;
use std::fmt::Formatter;

/// Allocation group number on the data device
#[derive(Debug, Copy, Clone)]
pub struct XfsAgNumber(u32);

impl TryFrom<u64> for XfsAgNumber {
    type Error = Error;

    fn try_from(v: u64) -> Result<Self> {
        if v > i32::MAX as u64 {
            Err(baddata!(M_("AG number too large"), Self, v).into())
        } else {
            Ok(XfsAgNumber(v as u32))
        }
    }
}

impl Display for XfsAgNumber {
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        write!(f, "{} {}", M_("agno"), self.0)
    }
}

impl From<XfsAgNumber> for u32 {
    fn from(val: XfsAgNumber) -> Self {
        val.0
    }
}

impl From<xfs_scrub_metadata> for XfsAgNumber {
    fn from(val: xfs_scrub_metadata) -> Self {
        XfsAgNumber(val.sm_agno)
    }
}

/// Realtime group number on the realtime device
#[derive(Debug, Copy, Clone)]
pub struct XfsRgNumber(u32);

impl Display for XfsRgNumber {
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        write!(f, "{} {}", M_("rgno"), self.0)
    }
}

impl TryFrom<u64> for XfsRgNumber {
    type Error = Error;

    fn try_from(v: u64) -> Result<Self> {
        if v > i32::MAX as u64 {
            Err(baddata!(M_("rtgroup number too large"), Self, v).into())
        } else {
            Ok(XfsRgNumber(v as u32))
        }
    }
}

impl From<XfsRgNumber> for u32 {
    fn from(val: XfsRgNumber) -> Self {
        val.0
    }
}

impl From<xfs_scrub_metadata> for XfsRgNumber {
    fn from(val: xfs_scrub_metadata) -> Self {
        XfsRgNumber(val.sm_agno)
    }
}

/// Disk devices
#[derive(Debug)]
pub enum XfsDevice {
    Data,
    Log,
    Realtime,
}

display_for_enum!(XfsDevice, {
    Data     => M_("datadev"),
    Log      => M_("logdev"),
    Realtime => M_("rtdev"),
});

/// Disk address, in 512b units
#[derive(Debug)]
pub struct XfsDaddr(u64);

impl Display for XfsDaddr {
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        write!(f, "{} {:#x}", M_("daddr"), self.0)
    }
}

impl From<u64> for XfsDaddr {
    fn from(v: u64) -> XfsDaddr {
        XfsDaddr(v)
    }
}

/// Disk space length, in 512b units
#[derive(Debug)]
pub struct XfsBbcount(u64);

impl Display for XfsBbcount {
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        write!(f, "{} {:#x}", M_("bbcount"), self.0)
    }
}

impl From<u64> for XfsBbcount {
    fn from(v: u64) -> XfsBbcount {
        XfsBbcount(v)
    }
}

/// Range of physical storage
#[derive(Debug)]
pub struct XfsPhysRange {
    /// Which device is this?
    pub device: XfsDevice,

    /// Start of the range, in 512b units
    pub daddr: XfsDaddr,

    /// Size of the range, in 512b units
    pub bbcount: XfsBbcount,
}

impl Display for XfsPhysRange {
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        write!(f, "{} {} {}", self.device, self.daddr, self.bbcount)
    }
}

/// Inode number
#[derive(Debug, Copy, Clone)]
pub struct XfsIno(u64);

impl Display for XfsIno {
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        write!(f, "{} {}", M_("ino"), self.0)
    }
}

impl TryFrom<u64> for XfsIno {
    type Error = Error;

    fn try_from(v: u64) -> Result<Self> {
        if v > i64::MAX as u64 {
            Err(baddata!(M_("inode number too large"), Self, v).into())
        } else {
            Ok(XfsIno(v))
        }
    }
}

impl From<XfsIno> for u64 {
    fn from(val: XfsIno) -> Self {
        val.0
    }
}

/// Inode generation number
#[derive(Debug, Copy, Clone)]
pub struct XfsIgeneration(u32);

impl Display for XfsIgeneration {
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        write!(f, "{} {:#x}", M_("gen"), self.0)
    }
}

impl TryFrom<u64> for XfsIgeneration {
    type Error = Error;

    fn try_from(v: u64) -> Result<Self> {
        if v > u32::MAX as u64 {
            Err(baddata!(M_("inode generation number too large"), Self, v).into())
        } else {
            Ok(XfsIgeneration(v as u32))
        }
    }
}

impl From<XfsIgeneration> for u32 {
    fn from(val: XfsIgeneration) -> Self {
        val.0
    }
}

/// Miniature FID for a handle
#[derive(Debug, Copy, Clone)]
pub struct XfsFid {
    /// Inode number
    pub ino: XfsIno,

    /// Inode generation
    pub gen: XfsIgeneration,
}

impl Display for XfsFid {
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        write!(f, "{} {}", self.ino, self.gen)
    }
}

impl From<xfs_scrub_metadata> for XfsFid {
    fn from(val: xfs_scrub_metadata) -> Self {
        XfsFid {
            ino: XfsIno(val.sm_ino),
            gen: XfsIgeneration(val.sm_gen),
        }
    }
}

/// File position
#[derive(Debug)]
pub struct XfsPos(u64);

impl Display for XfsPos {
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        write!(f, "{} {}", M_("pos"), self.0)
    }
}

impl TryFrom<u64> for XfsPos {
    type Error = Error;

    fn try_from(v: u64) -> Result<Self> {
        if v > i64::MAX as u64 {
            Err(baddata!(M_("file position too large"), Self, v).into())
        } else {
            Ok(XfsPos(v))
        }
    }
}

/// File IO length
#[derive(Debug)]
pub struct XfsIoLen(i64);

impl Display for XfsIoLen {
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        write!(f, "{} {}", M_("len"), self.0)
    }
}

impl TryFrom<u64> for XfsIoLen {
    type Error = Error;

    fn try_from(v: u64) -> Result<Self> {
        if v > i64::MAX as u64 {
            Err(baddata!(M_("file IO length too large"), Self, v).into())
        } else {
            Ok(XfsIoLen(v as i64))
        }
    }
}

/// Range of a file's bytes
#[derive(Debug)]
pub struct XfsFileRange {
    /// Start of range, in bytes
    pub pos: XfsPos,

    /// Length of range, in bytes
    pub len: XfsIoLen,
}

impl Display for XfsFileRange {
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        write!(f, "{} {}", self.pos, self.len)
    }
}
