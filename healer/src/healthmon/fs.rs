// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
use crate::healthmon::event::XfsHealthEvent;
use crate::healthmon::event::XfsHealthStatus;
use crate::util::format_set;
use crate::xfs_types::XfsPhysRange;
use enumset::EnumSet;
use enumset::EnumSetType;

/// Metadata types for an XFS whole-fs metadata
#[derive(EnumSetType, Debug, strum_macros::Display)]
pub enum XfsWholeFsMetadata {
    FsCounters,
    GrpQuota,
    MetaDir,
    MetaPath,
    NLinks,
    PrjQuota,
    QuotaCheck,
    UsrQuota,
}

/// XFS whole-fs health event
#[derive(Debug)]
pub struct XfsWholeFsEvent {
    /// What is being reported on?
    metadata: EnumSet<XfsWholeFsMetadata>,

    /// Reported state
    status: XfsHealthStatus,
}

impl XfsWholeFsEvent {
    /// Create a new whole-fs event object
    pub fn new(metadata: EnumSet<XfsWholeFsMetadata>, status: XfsHealthStatus) -> XfsWholeFsEvent {
        XfsWholeFsEvent { metadata, status }
    }
}

impl XfsHealthEvent for XfsWholeFsEvent {
    fn format(&self) -> String {
        format!("{} status {}", format_set(self.metadata), self.status)
    }
}

/// Reasons for a filesystem shutdown event
#[derive(EnumSetType, Debug, strum_macros::Display)]
pub enum XfsShutdownReason {
    #[strum(to_string = "in-memory state corruption")]
    CorruptInCore,

    #[strum(to_string = "ondisk metadata corruption")]
    CorruptOnDisk,

    #[strum(to_string = "device removed")]
    DeviceRemoved,

    #[strum(to_string = "forced unmount")]
    ForceUmount,

    #[strum(to_string = "log I/O error")]
    LogIoerr,

    #[strum(to_string = "metadata I/O error")]
    MetaIoerr,
}

/// XFS shutdown health event
#[derive(Debug)]
pub struct XfsShutdownEvent {
    /// Why did the filesystem shut down?
    reasons: EnumSet<XfsShutdownReason>,
}

impl XfsShutdownEvent {
    /// Create a new whole-fs event object
    pub fn new(reasons: EnumSet<XfsShutdownReason>) -> XfsShutdownEvent {
        XfsShutdownEvent { reasons }
    }
}

impl XfsHealthEvent for XfsShutdownEvent {
    fn must_log(&self) -> bool {
        true
    }

    fn format(&self) -> String {
        format!("filesystem shut down due to {}", format_set(self.reasons))
    }
}

/// Event for the filesystem being unmounted
pub struct XfsUnmountEvent {}

impl XfsHealthEvent for XfsUnmountEvent {
    fn must_log(&self) -> bool {
        true
    }

    fn format(&self) -> String {
        "filesystem unmounted".to_string()
    }
}

/// Media error event
#[derive(Debug)]
pub struct XfsMediaErrorEvent {
    /// Where was the media error?
    range: XfsPhysRange,
}

impl XfsMediaErrorEvent {
    /// Create a new file IO error event object
    pub fn new(range: XfsPhysRange) -> XfsMediaErrorEvent {
        XfsMediaErrorEvent { range }
    }
}

impl XfsHealthEvent for XfsMediaErrorEvent {
    fn format(&self) -> String {
        format!("media error on {}", self.range)
    }
}
