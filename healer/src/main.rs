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

/// Interpret command line arguments
#[derive(Parser, Debug)]
struct Cli {
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
    let fh = SoftHandle::try_from(&fp, &args.path)?;
    let hmon = XfsHealthMonitor::try_from(fp, &args.path, args.everything,
                                          args.debug)?;

    for f in hmon {
        if args.log {
            f.log();
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
