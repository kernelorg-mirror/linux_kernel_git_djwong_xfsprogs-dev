// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
use crate::display_for_enum;
use crate::healthmon::event::XfsHealthEvent;
use crate::healthmon::event::XfsHealthStatus;
use crate::util::format_set;
use crate::xfs_types::{XfsFid, XfsFileRange};
use crate::xfsprogs::M_;
use enumset::EnumSet;
use enumset::EnumSetType;

/// Metadata types for an XFS inode
#[derive(EnumSetType, Debug)]
pub enum XfsInodeMetadata {
    Bmapbta,
    Bmapbtc,
    Bmapbtd,
    Core,
    Directory,
    Dirtree,
    Parent,
    Symlink,
    Xattr,
}

display_for_enum!(XfsInodeMetadata, {
    Bmapbta   => M_("attrfork"),
    Bmapbtc   => M_("cowfork"),
    Bmapbtd   => M_("datafork"),
    Core      => M_("core"),
    Directory => M_("directory"),
    Dirtree   => M_("dirtree"),
    Parent    => M_("parent"),
    Symlink   => M_("symlink"),
    Xattr     => M_("xattr"),
});

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
#[derive(Debug)]
pub enum XfsFileIoErrorType {
    Readahead,
    Writeback,
    DirectioRead,
    DirectioWrite,
}

display_for_enum!(XfsFileIoErrorType, {
    Readahead     => M_("readahead"),
    Writeback     => M_("writeback"),
    DirectioRead  => M_("directio_read"),
    DirectioWrite => M_("directio_write"),
});

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
