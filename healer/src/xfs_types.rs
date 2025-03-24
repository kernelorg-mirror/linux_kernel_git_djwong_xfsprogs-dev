// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
use crate::baddata;
use std::fmt::Display;
use std::fmt::Formatter;
use std::io::Error;
use std::io::Result;
use strum_macros::EnumString;

/// Allocation group number on the data device
#[derive(Debug)]
pub struct XfsAgNumber(u32);

impl TryFrom<u64> for XfsAgNumber {
    type Error = Error;

    fn try_from(v: u64) -> Result<Self> {
        if v > i32::MAX as u64 {
            Err(baddata!("AG number too large", Self, v))
        } else {
            Ok(XfsAgNumber(v as u32))
        }
    }
}

impl Display for XfsAgNumber {
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        write!(f, "agno {}", self.0)
    }
}

/// Realtime group number on the realtime device
#[derive(Debug)]
pub struct XfsRgNumber(u32);

impl Display for XfsRgNumber {
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        write!(f, "rgno {}", self.0)
    }
}

impl TryFrom<u64> for XfsRgNumber {
    type Error = Error;

    fn try_from(v: u64) -> Result<Self> {
        if v > i32::MAX as u64 {
            Err(baddata!("rtgroup number too large", Self, v))
        } else {
            Ok(XfsRgNumber(v as u32))
        }
    }
}

/// Disk devices
#[derive(Debug, strum_macros::Display, EnumString)]
pub enum XfsDevice {
    #[strum(serialize = "datadev", to_string = "datadev")]
    Data,

    #[strum(serialize = "logdev", to_string = "logdev")]
    Log,

    #[strum(serialize = "rtdev", to_string = "rtdev")]
    Realtime,
}

/// Disk address, in 512b units
#[derive(Debug)]
pub struct XfsDaddr(u64);

impl Display for XfsDaddr {
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        write!(f, "daddr {:#x}", self.0)
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
        write!(f, "bbcount {:#x}", self.0)
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
#[derive(Debug)]
pub struct XfsIno(u64);

impl Display for XfsIno {
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        write!(f, "ino {}", self.0)
    }
}

impl TryFrom<u64> for XfsIno {
    type Error = Error;

    fn try_from(v: u64) -> Result<Self> {
        if v > i64::MAX as u64 {
            Err(baddata!("inode number too large", Self, v))
        } else {
            Ok(XfsIno(v))
        }
    }
}

/// Inode generation number
#[derive(Debug)]
pub struct XfsIgeneration(u32);

impl Display for XfsIgeneration {
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        write!(f, "gen {:#x}", self.0)
    }
}

impl TryFrom<u64> for XfsIgeneration {
    type Error = Error;

    fn try_from(v: u64) -> Result<Self> {
        if v > u32::MAX as u64 {
            Err(baddata!("inode generation number too large", Self, v))
        } else {
            Ok(XfsIgeneration(v as u32))
        }
    }
}

/// Miniature FID for a handle
#[derive(Debug)]
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

/// File position
#[derive(Debug)]
pub struct XfsPos(u64);

impl Display for XfsPos {
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        write!(f, "pos {}", self.0)
    }
}

impl TryFrom<u64> for XfsPos {
    type Error = Error;

    fn try_from(v: u64) -> Result<Self> {
        if v > i64::MAX as u64 {
            Err(baddata!("file position too large", Self, v))
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
        write!(f, "len {}", self.0)
    }
}

impl TryFrom<u64> for XfsIoLen {
    type Error = Error;

    fn try_from(v: u64) -> Result<Self> {
        if v > i64::MAX as u64 {
            Err(baddata!("file IO length too large", Self, v))
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
