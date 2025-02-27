// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
use clap::Parser;
use std::fs::File;
use std::io::Result;
use std::path::PathBuf;
use std::process::ExitCode;
use xfs_healer::healthmon::event::XfsHealthEvent;
use xfs_healer::healthmon::json::XfsHealthMonitor as JsonMonitor;
use xfs_healer::repair;
use xfs_healer::weakhandle::WeakHandle;
use xfs_healer::xfs_fs::xfs_fsop_geom;
use xfs_healer::xfsprogs;

// The struct below is a magic struct that implements argument parsing.

/// Automatically heal damage to XFS filesystem metadata
#[derive(Parser, Debug)]
struct Cli {
    /// Print version
    #[arg(short = 'V')]
    version: bool,

    /// Enable debugging messages
    #[arg(long)]
    debug: bool,

    /// Log health events to stdout
    #[arg(long)]
    log: bool,

    /// Capture all events
    #[arg(long)]
    everything: bool,

    /// Repair broken metadata unconditionally
    #[arg(long)]
    repair: bool,

    /// XFS filesystem mountpoint to monitor
    path: PathBuf,
}

/// Contains all the global program state but allows more flexibility.
#[derive(Debug)]
struct App {
    version: bool,
    debug: bool,
    log: bool,
    everything: bool,
    repair: bool,
    path: PathBuf,
}

impl App {
    /// Return mountpoint as string, for printing messages
    fn mountpoint(&self) -> String {
        self.path.display().to_string()
    }

    /// Handle a health event that has been decoded into real objects
    fn process_event(&self, fh: &WeakHandle, cooked: Result<Box<dyn XfsHealthEvent>>) {
        match cooked {
            Err(e) => {
                eprintln!("{}: {}", self.path.display(), e)
            }
            Ok(event) => {
                if self.log || event.must_log() {
                    println!("{}: {}", self.path.display(), event.format());
                }
                if self.repair {
                    for mut repair in event.schedule_repairs() {
                        repair.perform(fh)
                    }
                }
            }
        }
    }

    /// Complain a bit if repairs won't be entirely effective.
    fn check_repair(&self, fp: &File, fsgeom: &xfs_fsop_geom) -> Option<ExitCode> {
        if !repair::is_supported(fp) {
            println!(
                "{}: XFS online repair is not supported",
                self.path.display()
            );
            return Some(ExitCode::FAILURE);
        }

        if !fsgeom.has_rmapbt() {
            println!(
                "{}: XFS online repair is less effective without rmap btrees",
                self.path.display()
            );
        }
        if !fsgeom.has_parent() {
            println!(
                "{}: XFS online repair is less effective without parent pointers",
                self.path.display()
            );
        }

        None
    }

    /// Main app method
    fn main(&self) -> Result<ExitCode> {
        if self.version {
            println!("xfs_healer {}", xfsprogs::VERSION);
            return Ok(ExitCode::SUCCESS);
        }

        let fp = File::open(&self.path)?;

        let fsgeom = xfs_fsop_geom::try_from(&fp)?;
        if self.repair {
            if let Some(ret) = self.check_repair(&fp, &fsgeom) {
                return Ok(ret);
            }
        }

        let fh = WeakHandle::try_new(&fp, &self.path)?;
        let hmon = JsonMonitor::try_new(fp, &self.path, self.everything, self.debug)?;

        for raw_event in hmon {
            self.process_event(&fh, raw_event.cook());
        }

        Ok(ExitCode::SUCCESS)
    }
}

impl From<Cli> for App {
    fn from(cli: Cli) -> Self {
        App {
            version: cli.version,
            debug: cli.debug,
            log: cli.log,
            everything: cli.everything,
            repair: cli.repair,
            path: cli.path,
        }
    }
}

fn main() -> ExitCode {
    let args = Cli::parse();

    if args.debug {
        println!("args: {:?}", args);
    }

    let app: App = args.into();

    match app.main() {
        Ok(f) => f,
        Err(e) => {
            eprintln!("{}: {}", app.mountpoint(), e);
            ExitCode::FAILURE
        }
    }
}
