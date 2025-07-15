// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
use crate::weakhandle::WeakHandle;
use crate::xfs_fs;
use crate::xfs_fs::xfs_getparents;
use crate::xfs_fs::xfs_getparents_by_handle;
use crate::xfs_fs::xfs_getparents_rec;
use crate::xfs_fs::xfs_handle;
use crate::xfs_types::XfsFid;
use nix::ioctl_readwrite;
use std::cmp::min;
use std::ffi::CStr;
use std::ffi::OsStr;
use std::fs::File;
use std::io::Result;
use std::os::fd::AsRawFd;
use std::os::unix::ffi::OsStrExt;
use std::path::Path;
use std::path::PathBuf;

ioctl_readwrite!(
    xfs_ioc_getparents_by_handle,
    'X',
    63,
    xfs_getparents_by_handle
);

const GETPARENTS_BUFSIZE: usize = 65536;

/// File parent
#[derive(Debug)]
struct XfsParent {
    /// Filename within a directory
    filename: PathBuf,

    /// Handle to the parent
    handle: xfs_handle,
}

/// Iterator for all parents of this file
struct XfsGetParents<'a> {
    /// Open file with which we can call the ioctl
    fp: &'a File,

    /// Head object to pass to GETPARENTS call
    request: xfs_getparents_by_handle,

    /// Buffer for receiving GETPARENTS information from kernel
    buf: Vec<u8>,

    /// Position of next parent record in buffer
    bufpos: usize,
}

impl Iterator for XfsGetParents<'_> {
    type Item = Result<XfsParent>;

    /// Return parent pointer objects
    fn next(&mut self) -> Option<Self::Item> {
        // Ran out of buffer...
        if self.bufpos == GETPARENTS_BUFSIZE {
            const STOP_FLAGS: u32 =
                xfs_fs::XFS_GETPARENTS_OFLAG_DONE | xfs_fs::XFS_GETPARENTS_OFLAG_ROOT;

            // If the last request got all the parent pointers, stop
            if self.request.gph_request.gp_oflags & STOP_FLAGS as xfs_fs::__u16 != 0 {
                return None;
            }

            // SAFETY: Trusting the kernel to give us more parent data without corrupting memory.
            match unsafe { xfs_ioc_getparents_by_handle(self.fp.as_raw_fd(), &mut self.request) } {
                Err(e) => return Some(Err(e.into())),
                Ok(_) => self.bufpos = 0,
            }
        }

        // If the kernel says this is the root directory, return a parent
        // with an empty filename, because errors abort the iterator.
        if self.request.gph_request.gp_oflags & xfs_fs::XFS_GETPARENTS_OFLAG_ROOT as xfs_fs::__u16
            != 0
        {
            self.bufpos = GETPARENTS_BUFSIZE;
            return Some(Ok(XfsParent {
                filename: PathBuf::from(""),
                handle: self.request.gph_handle,
            }));
        }

        // Cast the buffer contents to a getparents record
        let ret = unsafe {
            // SAFETY: Casting a pointer (encoded as a u64 to avoid thunking issues) to a raw
            // pointer.  getparents.c in libfrog does the same thing.
            let gpr: *const xfs_getparents_rec =
                self.buf.as_ptr().add(self.bufpos) as *const xfs_getparents_rec;

            // Advance the buffer pointer
            self.bufpos += min(GETPARENTS_BUFSIZE - self.bufpos, (*gpr).gpr_reclen as usize);

            // Construct a PathBuf from the raw bytes.  Don't use a slice here because the buffer
            // contents will change with the next ioctl.  SAFETY: gpr_name is defined to be a
            // null-terminated sequence, aka a C string.
            let slice = CStr::from_ptr((*gpr).gpr_name.as_ptr());
            let osstr = OsStr::from_bytes(slice.to_bytes());
            let filename: &Path = osstr.as_ref();

            // SAFETY: Copying from a raw pointer to a buffer containing xfs_handle to the
            // xfs_handle in our new XfsParent object.
            XfsParent {
                filename: filename.to_path_buf(),
                handle: (*gpr).gpr_parent,
            }
        };

        Some(Ok(ret))
    }
}

/// Create an iterator to walk the parents of a given handle, using the open
/// file.
fn from_handle(fp: &File, handle: xfs_handle) -> Result<XfsGetParents> {
    let mut value: Vec<u8> = vec![0; GETPARENTS_BUFSIZE];

    Ok(XfsGetParents {
        request: xfs_getparents_by_handle {
            gph_request: xfs_getparents {
                gp_bufsize: GETPARENTS_BUFSIZE as xfs_fs::__u32,
                gp_buffer: value.as_mut_ptr() as xfs_fs::__u64,
                ..Default::default()
            },
            gph_handle: handle,
        },
        fp,
        buf: value,
        bufpos: GETPARENTS_BUFSIZE,
    })
}

/// Recursively fill the path component vector.  Returns true if we walked up
/// to the root directory and hence have a valid path, false if not, or None
/// if some error occurred.  We only use paths for display purposes, so that's
/// why we don't pass back errors.
fn find_path_components(
    fp: &File,
    handle: xfs_handle,
    depth: u32,
    components: &mut Vec<PathBuf>,
) -> Option<bool> {
    // Don't let us go too deep in the directory hierarchy because this is a
    // recursive function.
    if depth > 256 {
        return Some(false);
    }

    let parents = match from_handle(fp, handle) {
        Err(_) => return None,
        Ok(x) => x,
    };

    for p in parents {
        match p {
            Err(_) => return None,
            Ok(parent) => {
                if parent.filename == PathBuf::from("") {
                    return Some(true);
                }

                components.push(parent.filename);
                match find_path_components(fp, parent.handle, depth + 1, components) {
                    None => return None,
                    Some(true) => return Some(true),
                    Some(false) => components.pop(),
                };
            }
        };
    }

    Some(false)
}

impl WeakHandle {
    /// Return a path to the root for the given soft handle and ino/gen info,
    /// or None if errors occurred or we couldn't find the root.
    pub fn path_for(&self, fid: XfsFid) -> Option<PathBuf> {
        if !self.can_get_parents() {
            return None;
        }

        let fp = match self.reopen(|_| true) {
            Err(_) => return None,
            Ok(x) => x,
        };
        let handle = self.subst(fid);
        let mut path_components: Vec<PathBuf> = Vec::new();

        match find_path_components(&fp, handle, 0, &mut path_components) {
            None => None,
            Some(false) => None,
            Some(true) => {
                let mut ret: PathBuf = self.mountpoint().into();
                for component in path_components.iter().rev() {
                    ret.push(component);
                }
                Some(ret)
            }
        }
    }
}
