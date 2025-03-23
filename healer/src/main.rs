// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
use clap::{value_parser, Arg, ArgAction, ArgMatches, Command};
use std::fs::File;
use std::io::Result;
use std::path::PathBuf;
use std::process::ExitCode;
use xfs_healer::healthmon::cstruct::CStructMonitor;
use xfs_healer::healthmon::event::XfsHealthEvent;
use xfs_healer::healthmon::json::JsonMonitor;
use xfs_healer::printlogln;
use xfs_healer::repair::Repair;
use xfs_healer::weakhandle::WeakHandle;
use xfs_healer::xfs_fs::xfs_fsop_geom;
use xfs_healer::xfsprogs;
use xfs_healer::xfsprogs::M_;

/// Contains command line arguments
#[derive(Debug)]
struct Cli(ArgMatches);

impl Cli {
    pub fn new() -> Self {
        Cli(Command::new("xfs_healer")
            .disable_version_flag(true)
            .about(M_("Automatically heal damage to XFS filesystem metadata"))
            .arg(
                Arg::new("version")
                    .short('V')
                    .help(M_("Print version"))
                    .action(ArgAction::SetTrue),
            )
            .arg(
                Arg::new("debug")
                    .long("debug")
                    .help(M_("Enable debugging messages"))
                    .action(ArgAction::SetTrue),
            )
            .arg(
                Arg::new("log")
                    .long("log")
                    .help(M_("Log health events to stdout"))
                    .action(ArgAction::SetTrue),
            )
            .arg(
                Arg::new("everything")
                    .long("everything")
                    .help(M_("Capture all events"))
                    .action(ArgAction::SetTrue),
            )
            .arg(
                Arg::new("path")
                    .help(M_("XFS filesystem mountpoint to monitor"))
                    .value_parser(value_parser!(PathBuf))
                    .required_unless_present("version"),
            )
            .arg(
                Arg::new("json")
                    .long("json")
                    .help(M_("Use the JSON kernel interface instead of C"))
                    .action(ArgAction::SetTrue),
            )
            .arg(
                Arg::new("repair")
                    .long("repair")
                    .help(M_("Always repair corrupt metadata"))
                    .action(ArgAction::SetTrue),
            )
            .get_matches())
    }
}

/// Contains all the global program state but allows more flexibility.
#[derive(Debug)]
struct App {
    debug: bool,
    log: bool,
    everything: bool,
    json: bool,
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
                    let (maybe_path, message) = event.format(fh);
                    match maybe_path {
                        Some(x) => printlogln!("{}: {}", x.display(), message),
                        None => printlogln!("{}: {}", self.path.display(), message),
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

    /// Complain if repairs won't be entirely effective.
    fn check_repair(&self, fp: &File, fsgeom: &xfs_fsop_geom) -> Option<ExitCode> {
        if !Repair::is_supported(fp) {
            printlogln!(
                "{}: {}",
                self.path.display(),
                M_("XFS online repair is not supported, exiting")
            );
            return Some(ExitCode::FAILURE);
        }

        if !fsgeom.has_rmapbt() {
            printlogln!(
                "{}: {}",
                self.path.display(),
                M_("XFS online repair is less effective without rmap btrees")
            );
        }
        if !fsgeom.has_parent() {
            printlogln!(
                "{}: {}",
                self.path.display(),
                M_("XFS online repair is less effective without parent pointers")
            );
        }

        None
    }

    /// Main app method
    fn main(&self) -> Result<ExitCode> {
        let fp = File::open(&self.path)?;

        // Make sure that we can initiate repairs
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
            debug: cli.0.get_flag("debug"),
            log: cli.0.get_flag("log"),
            everything: cli.0.get_flag("everything"),
            path: cli.0.get_one::<PathBuf>("path").unwrap().to_path_buf(),
            json: cli.0.get_flag("json"),
            repair: cli.0.get_flag("repair"),
        }
    }
}

fn main() -> ExitCode {
    xfsprogs::init_localization();

    let args = Cli::new();
    if args.0.get_flag("version") {
        printlogln!("{} {}", M_("xfs_healer version"), xfsprogs::VERSION);
        return ExitCode::SUCCESS;
    }

    if args.0.get_flag("debug") {
        printlogln!("args: {:?}", args);
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
