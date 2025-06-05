// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
use anyhow::Result;
use libc::{endmntent, mntent, setmntent, FILE};
use std::ffi::{c_char, c_int, CStr, CString};
use std::io;
use std::mem::MaybeUninit;
use std::path::{Path, PathBuf};

/*
 * XXX: link directly to getmntent_r because the libc crate in Debian 12 is too old.  Note that
 * the bindgen'd xfs_fs.rs pulls in a similar but not totally identical version so we need to
 * turn off that warning.
 */
#[allow(clashing_extern_declarations)]
extern "C" {
    pub fn getmntent_r(
        stream: *mut FILE,
        mntbuf: *mut mntent,
        buf: *mut c_char,
        buflen: c_int,
    ) -> *mut mntent;
}

const NAME_BUFSIZE: usize = 4096;

/// Iterator object that returns mountpoint entries
pub struct MountEntries {
    /// mntent file
    fp: *mut FILE,

    /// local storage for mtab parsing
    mntbuf: mntent,
    namebuf: Vec<c_char>,
}

impl MountEntries {
    pub fn try_new_from(mountfile: &Path) -> Result<MountEntries> {
        let path = CString::new(
            mountfile
                .to_str()
                .ok_or(io::Error::new(io::ErrorKind::Other, "bad mntent path"))?,
        )?;
        let mode = CString::new("r")?;
        let fp = unsafe { setmntent(path.as_ptr(), mode.as_ptr()) };
        if fp.is_null() {
            return Err(io::Error::new(io::ErrorKind::Other, "setmntent failed").into());
        }

        let mntbuf = MaybeUninit::<mntent>::zeroed();
        let namebuf: Vec<c_char> = Vec::with_capacity(NAME_BUFSIZE);
        Ok(MountEntries {
            fp,
            namebuf,
            mntbuf: unsafe { mntbuf.assume_init() },
        })
    }

    pub fn try_new() -> Result<MountEntries> {
        MountEntries::try_new_from(std::path::Path::new("/proc/self/mounts"))
    }
}

#[derive(Debug)]
pub struct MountEntry {
    /// filesystem name
    pub fsname: String,

    /// mountpoint
    pub dir: PathBuf,

    /// filesystem type
    pub fstype: String,
}

impl Iterator for MountEntries {
    type Item = MountEntry;

    /// Return mount points
    fn next(&mut self) -> Option<Self::Item> {
        let ent = unsafe {
            getmntent_r(
                self.fp,
                &mut self.mntbuf,
                self.namebuf.as_mut_ptr() as *mut c_char,
                NAME_BUFSIZE as i32,
            )
        };
        if ent.is_null() {
            return None;
        }

        let f0 = unsafe { CStr::from_ptr((*ent).mnt_type) };
        let fstype = String::from_utf8_lossy(f0.to_bytes()).to_string();

        let f0 = unsafe { CStr::from_ptr((*ent).mnt_fsname) };
        let fsname = String::from_utf8_lossy(f0.to_bytes()).to_string();

        let f0 = unsafe { CStr::from_ptr((*ent).mnt_dir) };
        let dir = PathBuf::from(f0.to_str().unwrap());

        Some(MountEntry {
            fsname,
            fstype,
            dir,
        })
    }
}

impl Drop for MountEntries {
    fn drop(&mut self) {
        unsafe { endmntent(self.fp) };
    }
}
