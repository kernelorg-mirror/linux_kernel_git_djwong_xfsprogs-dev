// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
use crate::display_for_enum;
use crate::healthmon::event::XfsHealthEvent;
use crate::healthmon::event::XfsHealthStatus;
use crate::util::format_set;
use crate::xfs_types::XfsPhysRange;
use crate::xfsprogs::M_;
use enumset::EnumSet;
use enumset::EnumSetType;
use strum_macros::EnumString;

/// Metadata types for an XFS whole-fs metadata
#[derive(EnumSetType, Debug, EnumString)]
#[strum(serialize_all = "lowercase")]
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

display_for_enum!(XfsWholeFsMetadata, {
    FsCounters => M_("fscounters"),
    GrpQuota =>   M_("grpquota"),
    MetaDir =>    M_("metadir"),
    MetaPath =>   M_("metapath"),
    NLinks =>     M_("nlinks"),
    PrjQuota =>   M_("prjquota"),
    QuotaCheck => M_("quotacheck"),
    UsrQuota =>   M_("usrquota"),
});

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
        format!(
            "{} {} {}",
            format_set(self.metadata),
            M_("status"),
            self.status
        )
    }
}

/// Reasons for a filesystem shutdown event
#[derive(EnumSetType, Debug, EnumString)]
#[strum(serialize_all = "snake_case")]
pub enum XfsShutdownReason {
    CorruptIncore,
    CorruptOndisk,
    DeviceRemoved,
    ForceUmount,
    LogIoerr,
    MetaIoerr,
}

display_for_enum!(XfsShutdownReason, {
    CorruptIncore => M_("in-memory state corruption"),
    CorruptOndisk => M_("ondisk metadata corruption"),
    DeviceRemoved => M_("device removed"),
    ForceUmount   => M_("forced unmount"),
    LogIoerr      => M_("log I/O error"),
    MetaIoerr     => M_("metadata I/O error"),
});

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
        format!(
            "{} {}",
            M_("filesystem shut down due to"),
            format_set(self.reasons)
        )
    }
}

/// Event for the filesystem being unmounted
pub struct XfsUnmountEvent {}

impl XfsHealthEvent for XfsUnmountEvent {
    fn must_log(&self) -> bool {
        true
    }

    fn format(&self) -> String {
        M_("filesystem unmounted")
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
        format!("{} {}", M_("media error on"), self.range)
    }
}
