// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
use anyhow::{Context, Result};
use clap::{value_parser, Arg, ArgAction, ArgGroup, ArgMatches, Command};
use std::fs::File;
use std::path::PathBuf;
use std::process::ExitCode;
use std::sync::Arc;
use threadpool::ThreadPool;
use xfs_healer::fsprops;
use xfs_healer::fsprops::XfsAutofsck;
use xfs_healer::healthmon::cstruct::CStructMonitor;
use xfs_healer::healthmon::event::XfsHealthEvent;
use xfs_healer::healthmon::json::JsonEventWrapper;
use xfs_healer::healthmon::json::JsonMonitor;
use xfs_healer::healthmon::samefs::SameFs;
use xfs_healer::printlogln;
use xfs_healer::repair::Repair;
use xfs_healer::weakhandle::WeakHandle;
use xfs_healer::xfs_fs::xfs_fsop_geom;
use xfs_healer::xfs_fs::xfs_health_monitor_event;
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
            .arg(
                Arg::new("check")
                    .long("check")
                    .help(M_("Check that health monitoring is supported"))
                    .action(ArgAction::SetTrue),
            )
            .arg(
                Arg::new("autofsck")
                    .long("autofsck")
                    .help(M_("Use the \"autofsck\" fs property to decide to repair"))
                    .action(ArgAction::SetTrue),
            )
            .group(ArgGroup::new("decide_repair").args(["repair", "autofsck"]))
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
    check: bool,
    autofsck: bool,
    path: Arc<PathBuf>,
}

/// Contains all the per-thread state
#[derive(Debug)]
struct EventThread {
    log: bool,
    everything: bool,
    repair: bool,
}

impl EventThread {
    /// Create a new thread context from an App reference
    // XXX: I don't know how to do From<&App>
    fn new(app: &App) -> Self {
        EventThread {
            log: app.log,
            everything: app.everything,
            repair: app.repair,
        }
    }
}

/// Outcome of checking if the kernel supports metadata repair
enum CheckRepair {
    ExitWith(ExitCode),
    Downgrade,
    Proceed,
}

/// Outcome of looking at the autofsck fsproperty to decide if we will repair metadata
enum CheckAutofsck {
    ExitWith(ExitCode),
    Upgrade,
    Proceed,
}

impl App {
    /// Return mountpoint as string, for printing messages
    fn mountpoint(&self) -> String {
        self.path.display().to_string()
    }

    /// Handle a health event that has been decoded into real objects
    fn process_event(
        et: EventThread,
        samefs: Arc<SameFs>,
        fh: Arc<WeakHandle>,
        cooked: Result<Box<dyn XfsHealthEvent>>,
    ) {
        match cooked {
            Err(e) => {
                eprintln!("{}: {:#}", fh.mountpoint(), e)
            }
            Ok(event) => {
                if et.log || event.must_log() {
                    let (maybe_path, message) = event.format(&fh);
                    match maybe_path {
                        Some(x) => printlogln!("{}: {}", x.display(), message),
                        None => printlogln!("{}: {}", fh.mountpoint(), message),
                    };
                }
                if et.repair {
                    for mut repair in event.schedule_repairs(et.everything) {
                        repair.perform(&samefs, &fh)
                    }
                }
            }
        }
    }

    // fugly helpers to reduce the scope of the variables moved into the closure
    fn dispatch_json_event(
        threads: &ThreadPool,
        et: EventThread,
        samefs: Arc<SameFs>,
        fh: Arc<WeakHandle>,
        raw_event: JsonEventWrapper,
    ) {
        threads.execute(move || {
            App::process_event(et, samefs, fh, raw_event.cook());
        })
    }

    fn dispatch_cstruct_event(
        threads: &ThreadPool,
        et: EventThread,
        samefs: Arc<SameFs>,
        fh: Arc<WeakHandle>,
        raw_event: xfs_health_monitor_event,
    ) {
        threads.execute(move || {
            App::process_event(et, samefs, fh, raw_event.cook());
        })
    }

    /// Complain if repairs won't be entirely effective.
    fn check_repair(&self, fp: &File, fsgeom: &xfs_fsop_geom) -> CheckRepair {
        if !Repair::is_supported(fp) {
            if !self.autofsck {
                printlogln!(
                    "{}: {}",
                    self.path.display(),
                    M_("XFS online repair is not supported, exiting")
                );
                return CheckRepair::ExitWith(ExitCode::FAILURE);
            }

            printlogln!(
                "{}: {}",
                self.path.display(),
                M_("XFS online repair is not supported, will report only")
            );
            return CheckRepair::Downgrade;
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

        CheckRepair::Proceed
    }

    /// Set the behavior of the program from the autofsck fs property.
    fn check_autofsck(&self, fp: &File) -> CheckAutofsck {
        match fsprops::get_autofsck(fp) {
            XfsAutofsck::None => {
                printlogln!(
                    "{}: {}",
                    self.path.display(),
                    M_("Disabling healer per autofsck directive.")
                );
                return CheckAutofsck::ExitWith(ExitCode::SUCCESS);
            }
            XfsAutofsck::Check | XfsAutofsck::Optimize | XfsAutofsck::Unset => {
                printlogln!(
                    "{}: {}",
                    self.path.display(),
                    M_("Will not automatically heal per autofsck directive.")
                );
            }
            XfsAutofsck::Repair => {
                printlogln!(
                    "{}: {}",
                    self.path.display(),
                    M_("Automatically healing per autofsck directive.")
                );
                return CheckAutofsck::Upgrade;
            }
        }
        CheckAutofsck::Proceed
    }

    /// Main app method
    fn main(&mut self) -> Result<ExitCode> {
        let fp = File::open(&*self.path).with_context(|| M_("Opening filesystem failed"))?;

        // Decide if we're going to enable repairs, which must come before check_repair.
        if self.autofsck {
            match self.check_autofsck(&fp) {
                CheckAutofsck::ExitWith(ret) => return Ok(ret),
                CheckAutofsck::Upgrade => self.repair = true,
                CheckAutofsck::Proceed => {}
            }
        }

        // Make sure that we can initiate repairs
        let fsgeom =
            xfs_fsop_geom::try_from(&fp).with_context(|| M_("Querying filesystem geometry"))?;
        if self.repair {
            match self.check_repair(&fp, &fsgeom) {
                CheckRepair::ExitWith(ret) => return Ok(ret),
                CheckRepair::Downgrade => self.repair = false,
                CheckRepair::Proceed => {}
            }
        }

        // Now that we know that we can repair if the user wanted to, make sure that the kernel
        // supports reporting events if that was as far as the user wanted us to go.
        if self.check {
            return Ok(if xfs_healer::healthmon::is_supported(&fp, self.json) {
                ExitCode::SUCCESS
            } else {
                ExitCode::FAILURE
            });
        }

        let fh = Arc::new(
            WeakHandle::try_new(&fp, self.path.clone(), fsgeom)
                .with_context(|| M_("Configuring filesystem handle"))?,
        );

        // Creates a threadpool with nr_cpus workers.
        let threads = threadpool::Builder::new().build();

        if self.json {
            let hmon = JsonMonitor::try_new(fp, &self.path, self.everything, self.debug)
                .with_context(|| M_("Opening js health monitor file"))?;
            let samefs = hmon.new_samefs();

            for raw_event in hmon {
                App::dispatch_json_event(
                    &threads,
                    EventThread::new(self),
                    samefs.clone(),
                    fh.clone(),
                    raw_event,
                );
            }

            // Prohibit hmon from leaving scope (and closing the health mon fd) before the worker
            // threads have finished whatever they're doing.
            threads.join();
        } else {
            let hmon = CStructMonitor::try_new(fp, &self.path, self.everything)
                .with_context(|| M_("Opening health monitor file"))?;
            let samefs = hmon.new_samefs();

            for raw_event in hmon {
                App::dispatch_cstruct_event(
                    &threads,
                    EventThread::new(self),
                    samefs.clone(),
                    fh.clone(),
                    raw_event,
                );
            }

            // Prohibit hmon from leaving scope (and closing the health mon fd) before the worker
            // threads have finished whatever they're doing.
            threads.join();
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
            path: Arc::new(cli.0.get_one::<PathBuf>("path").unwrap().to_path_buf()),
            json: cli.0.get_flag("json"),
            repair: cli.0.get_flag("repair"),
            check: cli.0.get_flag("check"),
            autofsck: cli.0.get_flag("autofsck"),
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

    let mut app: App = args.into();
    match app.main() {
        Ok(f) => f,
        Err(e) => {
            eprintln!("{}: {:#}", app.mountpoint(), e);
            ExitCode::FAILURE
        }
    }
}
