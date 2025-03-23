// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
use crate::baddata;
use crate::xfs_fs::xfs_fsop_handlereq;
use crate::xfs_fs::xfs_handle;
use crate::xfsprogs::M_;
use anyhow::{Error, Result};
use nix::ioctl_readwrite;
use nix::libc::O_LARGEFILE;
use std::fmt::Display;
use std::fmt::Formatter;
use std::fs::File;
use std::io::ErrorKind;
use std::os::fd::AsRawFd;
use std::os::raw::c_void;
use std::path::Path;

ioctl_readwrite!(xfs_ioc_fd_to_handle, 'X', 106, xfs_fsop_handlereq);

/* just pick a value we know is more than big enough */
const MAXHANSIZ: usize = 64;

impl PartialEq for xfs_handle {
    fn eq(&self, other: &Self) -> bool {
        // SAFETY: accessing an arm of a union that exists only to force memory alignment
        unsafe { self.ha_u._ha_fsid == other.ha_u._ha_fsid && self.ha_fid == other.ha_fid }
    }
}

impl TryFrom<&File> for xfs_handle {
    type Error = Error;

    /// Create an xfs_handle for an open file
    fn try_from(fp: &File) -> Result<xfs_handle> {
        assert!(MAXHANSIZ >= std::mem::size_of::<xfs_handle>());

        let mut value: Vec<u8> = vec![0; MAXHANSIZ];
        let mut hreq: xfs_fsop_handlereq = Default::default();
        let mut hlen: u32 = 0;

        hreq.fd = fp.as_raw_fd() as u32;
        hreq.oflags = O_LARGEFILE as u32;
        hreq.ohandle = value.as_mut_ptr() as *mut c_void;
        hreq.ohandlen = &mut hlen;

        // SAFETY: Trusting the kernel not to corrupt hreq, value, or anything else.  This is wildly
        // incorrect because the kernel interface does not require userspace to pass in the size of
        // the object ohandle, so it writes blindly to *ohandle.
        unsafe {
            xfs_ioc_fd_to_handle(fp.as_raw_fd(), &mut hreq)?;
        }
        if hlen as usize != std::mem::size_of::<xfs_handle>() {
            return Err(baddata!(M_("Bad file handle size"), xfs_handle, hlen).into());
        }

        // SAFETY: We asserted above that value is large enough to store an xfs_handle, so we can
        // cast and struct copy here.
        unsafe {
            let hanp: *const xfs_handle = value.as_ptr() as *const xfs_handle;
            let ret: xfs_handle = *hanp;
            Ok(ret)
        }
    }
}

/// Filesystem handle that can be disconnected from any open files
pub struct WeakHandle<'a> {
    /// path to the filesystem mountpoint
    mountpoint: &'a Path,

    /// Filesystem handle
    handle: xfs_handle,
}

impl WeakHandle<'_> {
    /// Try to reopen the filesystem from which we got the handle.
    pub fn reopen(&self) -> Result<File> {
        let fp = File::open(self.mountpoint)?;

        if xfs_handle::try_from(&fp)? != self.handle {
            let s = format!(
                "{} {}: {}",
                M_("reopening"),
                self.mountpoint.display(),
                M_("Stale file handle")
            );
            return Err(std::io::Error::new(ErrorKind::Other, s).into());
        }

        Ok(fp)
    }

    /// Report mountpoint in a displayable manner
    pub fn mountpoint(&self) -> String {
        self.mountpoint.display().to_string()
    }

    /// Create a soft handle from an open file descriptor and its mount point
    pub fn try_new<'a>(fp: &File, mountpoint: &'a Path) -> Result<WeakHandle<'a>> {
        Ok(WeakHandle {
            mountpoint,
            handle: xfs_handle::try_from(fp)?,
        })
    }
}

impl Display for WeakHandle<'_> {
    fn fmt(&self, f: &mut Formatter) -> std::fmt::Result {
        write!(f, "{}", self.mountpoint.display())
    }
}
