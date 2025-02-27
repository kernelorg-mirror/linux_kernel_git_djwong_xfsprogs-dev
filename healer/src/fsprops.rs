// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
use std::fs::File;
use std::os::fd::AsRawFd;
use std::ffi::CString;
use std::os::raw::c_void;
use libc;

/// Property name for coordinating automatic fsck
pub const AUTOFSCK_NAME: &str = "autofsck";

const FSPROP_MAX_VALUELEN: usize = 256;

fn propname(realname: &str) -> String {
    let mut ret: String = "trusted.xfs:".to_owned();
    ret.push_str(realname);
    ret
}

/// Return the value of a filesystem property as a string.  Returns None on
/// any kind of error, or an empty value.
pub fn get(fp: &File, property: &str) -> Option<String> {
    let cname: CString = match CString::new(propname(property)) {
        Ok(x) => x,
        Err(_) => return None,
    };
    let mut value: Vec<u8> = vec![0; FSPROP_MAX_VALUELEN];

    let ret = unsafe {
        libc::fgetxattr(fp.as_raw_fd(), cname.as_ptr(),
                        value.as_mut_ptr() as *mut c_void, FSPROP_MAX_VALUELEN)
    };
    if ret < 1 {
        return None
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
