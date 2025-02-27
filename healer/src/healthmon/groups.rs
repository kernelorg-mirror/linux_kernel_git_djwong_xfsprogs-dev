// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
use crate::healthmon::event::schedule_repairs;
use crate::healthmon::event::XfsHealthEvent;
use crate::healthmon::event::XfsHealthStatus;
use crate::repair::Repair;
use crate::util::format_set;
use crate::weakhandle::WeakHandle;
use crate::xfs_types::{XfsPeragNumber, XfsRtgroupNumber};
use enumset::EnumSet;
use enumset::EnumSetType;
use std::path::PathBuf;
use strum_macros::EnumString;

/// Metadata types for an allocation group on the data device
#[derive(EnumSetType, Debug, strum_macros::Display, EnumString)]
#[strum(serialize_all = "lowercase")]
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
    group: XfsPeragNumber,

    /// What is being reported on?
    metadata: EnumSet<XfsPeragMetadata>,

    /// Reported state
    status: XfsHealthStatus,
}

impl XfsPeragEvent {
    /// Create a new perag event object
    pub fn new(
        group: XfsPeragNumber,
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
    fn format(&self, _: &WeakHandle) -> (Option<PathBuf>, String) {
        (
            None,
            format!(
                "{} {} {}",
                self.group,
                format_set(self.metadata),
                self.status
            ),
        )
    }

    schedule_repairs!(XfsPeragEvent, |s: &XfsPeragEvent, sm_type| {
        Repair::from_perag(sm_type, s.group)
    });
}

/// Metadata types for an allocation group on the realtime device
#[derive(EnumSetType, Debug, strum_macros::Display, EnumString)]
#[strum(serialize_all = "lowercase")]
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
    group: XfsRtgroupNumber,

    /// What is being reported on?
    metadata: EnumSet<XfsRtgroupMetadata>,

    /// Reported state
    status: XfsHealthStatus,
}

impl XfsRtgroupEvent {
    /// Create a new rtgroup event object
    pub fn new(
        group: XfsRtgroupNumber,
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
    fn format(&self, _: &WeakHandle) -> (Option<PathBuf>, String) {
        (
            None,
            format!(
                "{} {} {}",
                self.group,
                format_set(self.metadata),
                self.status
            ),
        )
    }

    schedule_repairs!(XfsRtgroupEvent, |s: &XfsRtgroupEvent, sm_type| {
        Repair::from_rtgroup(sm_type, s.group)
    });
}
