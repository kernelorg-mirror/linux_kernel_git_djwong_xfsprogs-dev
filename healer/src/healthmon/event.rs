// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
use crate::display_for_enum;
use crate::xfsprogs::M_;

/// Common behaviors of all health events
pub trait XfsHealthEvent {
    /// Return true if this event should always be logged
    fn must_log(&self) -> bool {
        false
    }

    /// Format this event as something we can display
    fn format(&self) -> String;
}

/// Health status for metadata events
#[derive(Debug)]
pub enum XfsHealthStatus {
    /// Problems have been observed at runtime
    Sick,

    /// Problems have been observed by xfs_scrub
    Corrupt,

    /// No problems at all
    Healthy,
}

display_for_enum!(XfsHealthStatus, {
    Sick    => M_("sick"),
    Corrupt => M_("corrupt"),
    Healthy => M_("healthy"),
});

/// Event for the kernel losing events due to us being slow
pub struct LostEvent {
    /// Number of events lost
    count: u64,
}

impl LostEvent {
    /// Create a new lost event object
    pub fn new(count: u64) -> LostEvent {
        LostEvent { count }
    }
}

impl XfsHealthEvent for LostEvent {
    fn must_log(&self) -> bool {
        true
    }

    fn format(&self) -> String {
        format!(": {} {}", self.count, M_("events lost"))
    }
}

/// Event for the monitor starting up
pub struct RunningEvent {}

impl XfsHealthEvent for RunningEvent {
    fn format(&self) -> String {
        format!(": {}", M_("monitoring started"))
    }
}

/// Event for the program losing events due to unrecognized inputs
pub struct UnknownEvent {}

impl XfsHealthEvent for UnknownEvent {
    fn must_log(&self) -> bool {
        true
    }

    fn format(&self) -> String {
        format!(": {}", M_("unrecognized event"))
    }
}
