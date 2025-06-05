// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
use crate::baddata;
use crate::badness;
use crate::getmntent::MountEntries;
use crate::xfs_fs::xfs_fid;
use crate::xfs_fs::xfs_fsop_geom;
use crate::xfs_fs::xfs_fsop_handlereq;
use crate::xfs_fs::xfs_handle;
use crate::xfs_types::XfsFid;
use crate::xfsprogs::M_;
use nix::ioctl_readwrite;
use nix::libc::O_LARGEFILE;
use std::ffi::OsString;
use std::fmt::Display;
use std::fmt::Formatter;
use std::fs::File;
use std::io::Error;
use std::io::ErrorKind;
use std::io::Result;
use std::os::fd::AsRawFd;
use std::os::raw::c_void;
use std::os::unix::ffi::OsStringExt;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::Arc;

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
            return Err(baddata!(M_("Bad file handle size"), xfs_handle, hlen));
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
pub struct WeakHandle {
    /// device for the xfs filesystem
    fsname: String,

    /// path to the filesystem mountpoint
    mountpoint: Arc<PathBuf>,

    /// Filesystem handle
    handle: xfs_handle,

    /// Does this filesystem support parent pointers?
    has_parent: bool,
}

impl WeakHandle {
    /// Try to reopen the filesystem with a given mountpoint
    fn reopen_from(&self, mountpoint: &Path) -> Result<File> {
        let fp = File::open(mountpoint)?;

        if xfs_handle::try_from(&fp)? != self.handle {
            let s = format!(
                "{} {}: {}",
                M_("reopening"),
                mountpoint.display(),
                M_("Stale file handle")
            );
            return Err(Error::new(ErrorKind::Other, s));
        }

        Ok(fp)
    }

    /// Try to reopen the filesystem from which we got the handle.
    pub fn reopen(&self) -> Result<File> {
        // First try the original mountpoint
        let orig_result = self.reopen_from(&self.mountpoint);
        if let Ok(x) = orig_result {
            return Ok(x);
        }

        // Now scan /proc/self/mounts for any other bind mounts of this filesystem
        let entries = MountEntries::try_new()?;
        for mntent in entries.filter(|x| x.fstype == "xfs" && x.fsname == self.fsname) {
            if let Ok(x) = self.reopen_from(&mntent.dir) {
                return Ok(x);
            }
        }

        // Return original error
        orig_result
    }

    /// Report mountpoint in a displayable manner
    pub fn mountpoint(&self) -> String {
        self.mountpoint.display().to_string()
    }

    /// Report xfs device in a displayable manner
    pub fn fsname(&self) -> String {
        self.fsname.clone()
    }

    /// Create a soft handle from an open file descriptor and its mount point
    pub fn try_new(
        fp: &File,
        mountpoint: Arc<PathBuf>,
        fsgeom: xfs_fsop_geom,
    ) -> Result<WeakHandle> {
        let mut entries = MountEntries::try_new()?;
        let fsname = match entries.find(|x| x.fstype == "xfs" && x.dir == *mountpoint) {
            None => {
                let s = format!("{}: {}", mountpoint.display(), M_("Cannot find xfs device"));
                return Err(Error::new(ErrorKind::Other, s));
            }
            Some(mntent) => mntent.fsname,
        };

        Ok(WeakHandle {
            mountpoint,
            fsname,
            handle: xfs_handle::try_from(fp)?,
            has_parent: fsgeom.has_parent(),
        })
    }

    /// Create a new file handle from this one
    pub fn subst(&self, fid: XfsFid) -> xfs_handle {
        xfs_handle {
            ha_fid: xfs_fid {
                fid_ino: fid.ino.into(),
                fid_gen: fid.gen.into(),
                ..self.handle.ha_fid
            },
            ..self.handle
        }
    }

    /// Can this filesystem do parent pointer lookups?
    pub fn can_get_parents(&self) -> bool {
        self.has_parent
    }

    /// Compute the systemd instance unit name for this mountpoint.
    pub fn instance_unit_name(&self, service_template: &str) -> Result<OsString> {
        let output = Command::new("systemd-escape")
            .arg("--template")
            .arg(service_template)
            .arg("--path")
            .arg(self.mountpoint.as_ref())
            .output()?;

        if !output.status.success() {
            return Err(badness!("Could not format systemd instance unit name."));
        }

        // systemd always adds a newline to the end of the output; remove it
        let trunc_out = &output.stdout[0..output.stdout.len() - 1];
        Ok(OsString::from_vec(trunc_out.to_vec()))
    }
}

impl Display for WeakHandle {
    fn fmt(&self, f: &mut Formatter) -> std::fmt::Result {
        write!(f, "{} {}", self.fsname, self.mountpoint.display())
    }
}
