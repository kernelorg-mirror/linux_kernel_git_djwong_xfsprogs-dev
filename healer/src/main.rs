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
use xfs_healer::fsprops;
use xfs_healer::fsprops::XfsAutofsck;
use xfs_healer::healthmon::cstruct::XfsHealthMonitor as CStructMonitor;
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

    /// Check to see if monitoring is supported
    #[arg(long)]
    check: bool,

    /// Decide what to do using the autofsck fs property
    #[arg(long)]
    autofsck: bool,

    /// Use the JSON interface?
    #[arg(long)]
    json: bool,

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
    check: bool,
    autofsck: bool,
    json: bool,
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
                    let (maybe_path, message) = event.format(fh);
                    match maybe_path {
                        Some(x) => println!("{}: {}", x.display(), message),
                        None => println!("{}: {}", self.path.display(), message),
                    };
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

    /// Set the behavior of the program from the autofsck fs property.
    /// Returns a u32 if we should exit the program.
    fn set_autofsck(&mut self, fp: &File) -> Option<ExitCode> {
        match fsprops::get_autofsck(fp) {
            XfsAutofsck::None => {
                println!(
                    "{}: Disabling healer per autofsck directive.",
                    self.path.display()
                );
                return Some(ExitCode::SUCCESS);
            }
            XfsAutofsck::Check | XfsAutofsck::Optimize | XfsAutofsck::Unset => {
                println!(
                    "{}: Will not automatically heal per autofsck directive.",
                    self.path.display()
                );
            }
            XfsAutofsck::Repair => {
                println!(
                    "{}: Automatically healing per autofsck directive.",
                    self.path.display()
                );
                self.repair = true;
            }
        }
        None
    }

    /// Main app method
    fn main(&mut self) -> Result<ExitCode> {
        if self.version {
            println!("xfs_healer {}", xfsprogs::VERSION);
            return Ok(ExitCode::SUCCESS);
        }

        let fp = File::open(&self.path)?;

        if self.check {
            return Ok(if xfs_healer::healthmon::is_supported(&fp, self.json) {
                ExitCode::SUCCESS
            } else {
                ExitCode::FAILURE
            });
        }

        // Decide if we're going to enable repairs, which must come before check_repair.
        if self.autofsck {
            if let Some(ret) = self.set_autofsck(&fp) {
                return Ok(ret);
            }
        }

        let fsgeom = xfs_fsop_geom::try_from(&fp)?;
        if self.repair {
            if let Some(ret) = self.check_repair(&fp, &fsgeom) {
                return Ok(ret);
            }
        }

        let fh = WeakHandle::try_new(&fp, &self.path, fsgeom)?;
        if self.json {
            let hmon = JsonMonitor::try_new(fp, &self.path, self.everything, self.debug)?;

            for raw_event in hmon {
                self.process_event(&fh, raw_event.cook());
            }
        } else {
            let hmon = CStructMonitor::try_new(fp, &self.path, self.everything)?;

            for raw_event in hmon {
                self.process_event(&fh, raw_event.cook());
            }
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
            check: cli.check,
            autofsck: cli.autofsck,
            json: cli.json,
            path: cli.path,
        }
    }
}

fn main() -> ExitCode {
    let args = Cli::parse();

    if args.debug {
        println!("args: {:?}", args);
    }

    let mut app: App = args.into();

    match app.main() {
        Ok(f) => f,
        Err(e) => {
            eprintln!("{}: {}", app.mountpoint(), e);
            ExitCode::FAILURE
        }
    }
}
