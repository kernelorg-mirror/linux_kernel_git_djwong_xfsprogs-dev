// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
use std::io;
use std::fs::File;
use std::os::fd::AsRawFd;
use std::cmp::min;
use std::ffi::CStr;
use nix::ioctl_readwrite;
use crate::softhandle;
use crate::xfs_fs;

ioctl_readwrite!(xfs_ioc_getparents_by_handle, 'X', 63, xfs_fs::xfs_getparents_by_handle);

const GETPARENTS_BUFSIZE: usize = 65536;

/// File parent
#[derive(Debug)]
struct XfsParent {
    /// Filename within a directory
    filename: String,

    /// Handle to the parent
    handle: xfs_fs::xfs_handle,
}

/// Iterator for all parents of this file
struct XfsGetParents<'a> {
    /// Open file with which we can call the ioctl
    fp: &'a File,

    /// Head object to pass to GETPARENTS call
    request: xfs_fs::xfs_getparents_by_handle,

    /// Buffer for receiving GETPARENTS information from kernel
    buf: Vec<u8>,

    /// Position of next parent record in buffer
    bufpos: usize,
}

impl Iterator for XfsGetParents<'_> {
    type Item = io::Result<XfsParent>;

    /// Return parent pointer objects
    fn next(&mut self) -> Option<Self::Item> {
        // Ran out of buffer...
        if self.bufpos == GETPARENTS_BUFSIZE {
            const STOP_FLAGS: u32 = xfs_fs::XFS_GETPARENTS_OFLAG_DONE |
                                    xfs_fs::XFS_GETPARENTS_OFLAG_ROOT;

            // If the last request got all the parent pointers, stop
            if self.request.gph_request.gp_oflags &
               STOP_FLAGS as xfs_fs::__u16 != 0 {
                return None
            }

            // Go get more parent data
            match unsafe {
                xfs_ioc_getparents_by_handle(self.fp.as_raw_fd(), &mut self.request)
            } {
                Err(e) => return Some(Err(e.into())),
                Ok(_) => self.bufpos = 0,
            }
        }

        // If the kernel says this is the root directory, return a parent
        // with an empty filename, because errors abort the iterator.
        if self.request.gph_request.gp_oflags &
                xfs_fs::XFS_GETPARENTS_OFLAG_ROOT as xfs_fs::__u16 != 0 {
            self.bufpos = GETPARENTS_BUFSIZE;
            return Some(Ok(XfsParent {
                filename: "".to_string(),
                handle: self.request.gph_handle,
            }));
        }

        // Cast the buffer contents to a getparents record
        let ret = unsafe {
            let gpr: *const xfs_fs::xfs_getparents_rec =
                    self.buf.as_ptr().offset(self.bufpos as isize)
                    as *const xfs_fs::xfs_getparents_rec;

            // Advance the buffer pointer
            self.bufpos += min(GETPARENTS_BUFSIZE - self.bufpos,
                               (*gpr).gpr_reclen as usize);

            // Sketchily construct the filename and mash any broken utf8
            let slice = CStr::from_ptr((*gpr).gpr_name.as_ptr());
            let filename = slice.to_string_lossy().to_string();

            XfsParent {
                filename: filename,
                handle: (*gpr).gpr_parent,
            }
        };

        Some(Ok(ret))
    }
}

/// Create an iterator to walk the parents of a given handle, using the open
/// file.
fn from_handle(fp: &File, handle: xfs_fs::xfs_handle) ->
               io::Result<XfsGetParents> {
    let mut value: Vec<u8> = vec![0; GETPARENTS_BUFSIZE];

    Ok(XfsGetParents {
        request: xfs_fs::xfs_getparents_by_handle {
            gph_request: xfs_fs::xfs_getparents {
                gp_cursor: xfs_fs::xfs_attrlist_cursor {
                    opaque: [0, 0, 0, 0],
                },
                gp_iflags: 0,
                gp_oflags: 0,
                gp_bufsize: GETPARENTS_BUFSIZE as xfs_fs::__u32,
                gp_reserved: 0,
                gp_buffer: value.as_mut_ptr() as xfs_fs::__u64,
            },
            gph_handle: handle,
        },
        fp,
        buf: value,
        bufpos: GETPARENTS_BUFSIZE,
    })
}

fn find_path_components(fp: &File, handle: xfs_fs::xfs_handle,
                        components: &mut Vec<String>) -> io::Result<bool> {
    let parents = from_handle(&fp, handle)?;

    for p in parents {
        match p {
            Err(x) => return Err(x),
            Ok(parent) => {
                if parent.filename == "" {
                    return Ok(true)
                }

                components.push(parent.filename);
                if find_path_components(&fp, parent.handle, components)? {
                    return Ok(true)
                }
                components.pop();
            },
        }
    }

    return Ok(false)
}

impl softhandle::SoftHandle {
    /// Return a path to the root for the given soft handle and ino/gen info.
    pub fn path_from(&self, ino: u64, gen: u32) -> io::Result<Option<String>> {
        if !self.can_get_parents() {
            return Ok(None);
        }

        let fp = self.reopen()?;
        let handle = self.subst(ino, gen);
        let mut path_components: Vec<String> = Vec::new();

        if ! find_path_components(&fp, handle, &mut path_components)? {
            return Ok(None)
        }

        let mut ret = self.mountpoint();
        for component in path_components.iter().rev() {
            ret.push_str("/");
            ret.push_str(&component);
        }
        Ok(Some(ret))
    }
}
