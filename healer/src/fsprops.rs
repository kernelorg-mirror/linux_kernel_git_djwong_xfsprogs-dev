// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
use libc::fgetxattr;
use std::ffi::CString;
use std::fs::File;
use std::os::fd::AsRawFd;
use std::os::raw::c_void;
use std::str::FromStr;
use strum_macros::EnumString;

/// Property name for coordinating automatic fsck
const AUTOFSCK_NAME: &str = "autofsck";

/// Boilerplate to stamp out functions to convert json array to the given enum
/// type; or return an error with the given message.
macro_rules! fsprop_from_string {
    ($enum_type:ty , $default:ident) => {
        impl From<Option<String>> for $enum_type {
            /// Convert from a json value to an enum
            fn from(v: Option<String>) -> $enum_type {
                if let Some(value) = v {
                    match <$enum_type>::from_str(&value) {
                        Ok(o) => o,
                        Err(_) => <$enum_type>::$default,
                    }
                } else {
                    <$enum_type>::$default
                }
            }
        }
    };
}

/// Values for the autofsck property
#[derive(Debug, strum_macros::Display, EnumString)]
#[strum(serialize_all = "lowercase")]
pub enum XfsAutofsck {
    /// No value set
    Unset,

    /// Do not do background repairs
    None,

    /// Check but do not change anything
    Check,

    /// Optimize only, do not repair
    Optimize,

    /// Repair and optimize
    Repair,
}
fsprop_from_string!(XfsAutofsck, Unset);

const FSPROP_MAX_VALUELEN: usize = 256;

fn propname(realname: &str) -> String {
    let mut ret: String = "trusted.xfs:".to_owned();
    ret.push_str(realname);
    ret
}

/// Return the value of a filesystem property as a string.  Returns None on
/// any kind of error, or an empty value.
fn get(fp: &File, property: &str) -> Option<String> {
    let cname: CString = match CString::new(propname(property)) {
        Ok(x) => x,
        Err(_) => return None,
    };
    let mut value: Vec<u8> = vec![0; FSPROP_MAX_VALUELEN];

    // SAFETY: Trusting the kernel not to corrupt either buffer that we pass in.
    let ret = unsafe {
        fgetxattr(
            fp.as_raw_fd(),
            cname.as_ptr(),
            value.as_mut_ptr() as *mut c_void,
            FSPROP_MAX_VALUELEN,
        )
    };
    if ret < 1 {
        return None;
    }

    let cvalue: CString = match CString::new(&value[0..ret as usize]) {
        Ok(x) => x,
        _ => return None,
    };
    match cvalue.into_string() {
        Ok(x) => Some(x),
        _ => None,
    }
}

/// Return the autofsck filesystem property.
pub fn get_autofsck(fp: &File) -> XfsAutofsck {
    get(fp, AUTOFSCK_NAME).into()
}
