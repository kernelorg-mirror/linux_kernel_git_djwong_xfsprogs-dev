// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
use crate::healthmon::event::XfsHealthEvent;
use crate::healthmon::event::XfsHealthStatus;
use crate::util::format_set;
use crate::xfs_types::{XfsAgNumber, XfsRgNumber};
use enumset::EnumSet;
use enumset::EnumSetType;

/// Metadata types for an allocation group on the data device
#[derive(EnumSetType, Debug, strum_macros::Display)]
pub enum XfsPeragMetadata {
    Agf,
    Agfl,
    Agi,
    Bnobt,
    Cntbt,
    Finobt,
    Inobt,
    Inodes,
    Refcountbt,
    Rmapbt,
    Super,
}

/// XFS perag health event
#[derive(Debug)]
pub struct XfsPeragEvent {
    /// Allocation group number
    group: XfsAgNumber,

    /// What is being reported on?
    metadata: EnumSet<XfsPeragMetadata>,

    /// Reported state
    status: XfsHealthStatus,
}

impl XfsPeragEvent {
    /// Create a new perag event object
    pub fn new(
        group: XfsAgNumber,
        metadata: EnumSet<XfsPeragMetadata>,
        status: XfsHealthStatus,
    ) -> XfsPeragEvent {
        XfsPeragEvent {
            group,
            metadata,
            status,
        }
    }
}

impl XfsHealthEvent for XfsPeragEvent {
    fn format(&self) -> String {
        format!(
            "{} {} {}",
            self.group,
            format_set(self.metadata),
            self.status
        )
    }
}

/// Metadata types for an allocation group on the realtime device
#[derive(EnumSetType, Debug, strum_macros::Display)]
pub enum XfsRtgroupMetadata {
    Bitmap,
    Summary,
    Refcountbt,
    Rmapbt,
    Super,
}

/// XFS rtgroup health event
#[derive(Debug)]
pub struct XfsRtgroupEvent {
    /// Allocation group number
    group: XfsRgNumber,

    /// What is being reported on?
    metadata: EnumSet<XfsRtgroupMetadata>,

    /// Reported state
    status: XfsHealthStatus,
}

impl XfsRtgroupEvent {
    /// Create a new rtgroup event object
    pub fn new(
        group: XfsRgNumber,
        metadata: EnumSet<XfsRtgroupMetadata>,
        status: XfsHealthStatus,
    ) -> XfsRtgroupEvent {
        XfsRtgroupEvent {
            group,
            metadata,
            status,
        }
    }
}

impl XfsHealthEvent for XfsRtgroupEvent {
    fn format(&self) -> String {
        format!(
            "{} {} {}",
            self.group,
            format_set(self.metadata),
            self.status
        )
    }
}
