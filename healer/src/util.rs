// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
use enumset::EnumSet;
use enumset::EnumSetType;
use std::fmt::Display;

/// Simple macro for creating errors for badly formatted event data.  The first parameter describes
/// why the data is bad; the second is the target type; and the third is value provided.
#[macro_export]
macro_rules! baddata {
    ($message:expr , $type:tt , $value:expr) => {{
        match (&$message, &$value) {
            (message_val, value_val) => {
                let s = format!(
                    "{}: {} {} {}.",
                    value_val,
                    message_val,
                    $crate::xfsprogs::M_("for"),
                    std::any::type_name::<$type>()
                );
                std::io::Error::new(std::io::ErrorKind::InvalidData, s)
            }
        }
    }};
}

/// Write a line to standard output and flush it.
#[macro_export]
macro_rules! printlogln {
    ( $($t:tt)* ) => {
        {
            use std::io::Write;
            let mut h = std::io::stdout().lock();
            write!(h, $($t)* ).unwrap();
            write!(h, "\n").unwrap();
            h.flush().unwrap();
        }
    }
}

/// Boilerplate to stamp out functions to convert an enum to some sort of pretty string.
// XXX: This could have been a derive macro
#[macro_export]
macro_rules! display_for_enum {
    ($enum_type:ty , { $($a:ident => $b:expr,)+ } ) => {
        impl std::fmt::Display for $enum_type {
            /// Convert from an enum to a string
            fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
                write!(f, "{}", match self { $(<$enum_type>::$a => $b,)+ })
            }
        }
    };
}

/// Format an enum set into a string
pub fn format_set<T: EnumSetType + Display>(f: EnumSet<T>) -> String {
    let mut ret = "".to_string();
    let mut is_first = true;

    for v in f {
        if !is_first {
            ret.push_str(", ");
        }
        is_first = false;
        ret.push_str(&format!("{}", v));
    }

    ret
}
