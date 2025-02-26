// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
use std::io;
use std::ptr;
use std::fs::File;
use std::mem::MaybeUninit;
use std::os::fd::AsRawFd;
use std::os::raw::{c_void, c_int};
use crate::libhandle;
use crate::libhandle::size_t;
use crate::xfs_fs;
use crate::baddata;

// dumb wrappers around libhandle functions and data structures

fn fd_to_handle(
        fd: c_int,
        hanp: *mut *mut c_void,
        hlen: *mut size_t
    ) -> nix::Result<c_int> {
    unsafe {
        nix::errno::Errno::result(libhandle::fd_to_handle(fd, hanp, hlen))
    }
}

fn free_handle(hanp: *mut c_void, hlen: size_t) {
    unsafe {
        libhandle::free_handle(hanp, hlen)
    }
}

impl PartialEq for xfs_fs::xfs_handle {
    fn eq(&self, other: &Self) -> bool {
        unsafe { self.ha_u._ha_fsid == other.ha_u._ha_fsid &&
            self.ha_fid == other.ha_fid }
    }
}

impl xfs_fs::xfs_handle {
    /// Create an xfs_handle for an open file
    pub fn from_file(fp: &File) -> io::Result<xfs_fs::xfs_handle> {
        let mut hanp = ptr::null_mut();
        let mut hlen: size_t = 0;

        let mut handle = MaybeUninit::<xfs_fs::xfs_handle>::uninit();
        let ptr = handle.as_mut_ptr();

        fd_to_handle(fp.as_raw_fd(), &mut hanp, &mut hlen)?;

        if hlen > usize::MAX as size_t ||
           hlen as usize != std::mem::size_of::<xfs_fs::xfs_handle>() {
            free_handle(hanp, hlen);
            return Err(baddata!("Bad file handle size", "xfs_handle"))
        }

        unsafe {
            hanp.copy_to_nonoverlapping(ptr as *mut c_void, hlen as usize);
            libhandle::free_handle(hanp, hlen);
            Ok(handle.assume_init())
        }
    }
}

/// Soft handle that can be reconnected to a filesystem
pub struct SoftHandle {
    /// path to the filesystem mountpoint
    mountpoint: std::path::PathBuf,

    /// Filesystem handle
    handle: xfs_fs::xfs_handle,
}

impl SoftHandle {
    /// Try to reopen the filesystem from which we got the handle.
    pub fn reopen(&self) -> io::Result<File> {
        let fp = File::open(&self.mountpoint)?;

        if xfs_fs::xfs_handle::from_file(&fp)? != self.handle {
            let s = format!("{}: Stale file handle", self.mountpoint.display());
            return Err(io::Error::new(io::ErrorKind::Other, s))
        }

        Ok(fp)
    }

    /// Create a soft handle from an open file descriptor and its mount point
    pub fn try_from(fp: &File, mountpoint: &std::path::PathBuf) ->
            io::Result<SoftHandle> {
        Ok(SoftHandle {
            mountpoint: mountpoint.clone(),
            handle: xfs_fs::xfs_handle::from_file(&fp)?,
        })
    }
}
