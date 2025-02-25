// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
use enumset::EnumSet;
use enumset::EnumSetType;
use std::fmt::Display;

/// Simple macro for creating errors for badly formatted event data.  The first
/// parameter describes why the data is bad; the second is the value.
#[macro_export]
macro_rules! baddata {
    ($message:expr , $value:expr) => {{
        match (&$message, &$value) {
            (message_val, value_val) => {
                let s = format!("{value_val}: {message_val}");
                std::io::Error::new(std::io::ErrorKind::InvalidData, s)
            }
        }
    }};
}

/// Format an enum set into a string
pub fn format_set<T: EnumSetType + Display>(f: EnumSet<T>) -> String {
    let mut ret = "".to_string();
    let mut is_first = true;

    for v in f {
        if !is_first {
            ret.push_str(" | ");
        }
        is_first = false;
        ret.push_str(&format!("{}", v));
    }

    ret
}
