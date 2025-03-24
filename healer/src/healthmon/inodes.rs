// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
use crate::healthmon::event::XfsHealthEvent;
use crate::healthmon::event::XfsHealthStatus;
use crate::util::format_set;
use crate::xfs_types::{XfsFid, XfsFileRange};
use enumset::EnumSet;
use enumset::EnumSetType;

/// Metadata types for an XFS inode
#[derive(EnumSetType, Debug, strum_macros::Display)]
pub enum XfsInodeMetadata {
    #[strum(to_string = "attrfork")]
    Bmapbta,

    #[strum(to_string = "cowfork")]
    Bmapbtc,

    #[strum(to_string = "datafork")]
    Bmapbtd,

    Core,
    Directory,

    Dirtree,
    Parent,
    Symlink,

    Xattr,
}

/// XFS inode health event
#[derive(Debug)]
pub struct XfsInodeEvent {
    /// File information
    fid: XfsFid,

    /// What is being reported on?
    metadata: EnumSet<XfsInodeMetadata>,

    /// Reported state
    status: XfsHealthStatus,
}

impl XfsInodeEvent {
    /// Create a new inode metadata event object
    pub fn new(
        fid: XfsFid,
        metadata: EnumSet<XfsInodeMetadata>,
        status: XfsHealthStatus,
    ) -> XfsInodeEvent {
        XfsInodeEvent {
            fid,
            metadata,
            status,
        }
    }
}

impl XfsHealthEvent for XfsInodeEvent {
    fn format(&self) -> String {
        format!("{} {} {}", self.fid, format_set(self.metadata), self.status)
    }
}

/// File I/O types
#[derive(Debug, strum_macros::Display)]
pub enum XfsFileIoErrorType {
    #[strum(to_string = "readahead")]
    ReadAhead,

    #[strum(to_string = "writeback")]
    WriteBack,

    #[strum(to_string = "directio_read")]
    DioRead,

    #[strum(to_string = "directio_write")]
    DioWrite,
}

/// XFS file I/O error event
#[derive(Debug)]
pub struct XfsFileIoErrorEvent {
    /// What file I/O went wrong?
    iotype: XfsFileIoErrorType,

    /// Which file?
    fid: XfsFid,

    /// Which file and where?
    range: XfsFileRange,
}

impl XfsFileIoErrorEvent {
    /// Create a new file IO error event object
    pub fn new(
        iotype: XfsFileIoErrorType,
        fid: XfsFid,
        range: XfsFileRange,
    ) -> XfsFileIoErrorEvent {
        XfsFileIoErrorEvent { iotype, fid, range }
    }
}

impl XfsHealthEvent for XfsFileIoErrorEvent {
    fn format(&self) -> String {
        format!("{} {} {}", self.fid, self.iotype, self.range)
    }
}
