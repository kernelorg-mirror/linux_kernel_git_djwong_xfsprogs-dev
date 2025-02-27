// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
use std::io;
use std::fs::File;
use clap::Parser;
use xfs_healer::healthmon::XfsHealthMonitor;
use xfs_healer::softhandle::SoftHandle;
use xfs_healer::xfs_fs::xfs_fsop_geom;

/// Interpret command line arguments
#[derive(Parser, Debug)]
struct Cli {
    /// Check to see if monitoring is supported.
    #[arg(short, long)]
    check: bool,

    /// Enable debugging messages.
    #[arg(short, long)]
    debug: bool,

    /// Log health events to stdout.
    #[arg(short, long)]
    log: bool,

    /// Capture all events.
    #[arg(short, long)]
    everything: bool,

    /// Repair broken metadata?
    #[arg(short, long)]
    repair: bool,

    /// XFS filesystem mountpoint to monitor.
    path: std::path::PathBuf,
}

fn __main(args: &Cli) -> io::Result<i32> {
    if args.debug {
        println!("{:?}", args);
    }

    let fp = File::open(&args.path)?;

    /* Just a presence check? */
    if args.check {
        match XfsHealthMonitor::is_supported(&fp) {
            Ok(_)  => std::process::exit(0),
            Err(_) => std::process::exit(1),
        }
    }

    /* Complain a bit if repairs won't be entirely effective. */
    let fsgeom = xfs_fsop_geom::try_from(&fp)?;
    if args.repair {
        if ! fsgeom.has_rmapbt() {
            println!("{}: XFS online repair is less effective without rmap btrees",
                     args.path.display());
        }
        if ! fsgeom.has_parent() {
            println!("{}: XFS online repair is less effective without parent pointers",
                     args.path.display());
        }
    }

    let fh = SoftHandle::try_from(&fp, &args.path, &fsgeom)?;
    let hmon = XfsHealthMonitor::try_from(fp, &args.path, args.everything,
                                          args.debug)?;

    for f in hmon {
        if args.log {
            f.log(&fh);
        }
        if args.repair {
            match f.schedule_repair() {
                Some(vec) => {
                    for mut repair in vec {
                        repair.perform(&fh)
                    }
                },
                _ => continue,
            }
        }
    }

    Ok(0)
}

fn main() {
    let args = Cli::parse();

    match __main(&args) {
        Ok(f)   => std::process::exit(f),
        Err(e)  => {
            eprintln!("{}: {}", args.path.display(), e);
            std::process::exit(1);
        }
    };
}
