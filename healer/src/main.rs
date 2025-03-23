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
use std::sync::Arc;
use threadpool::ThreadPool;
use xfs_healer::fsprops;
use xfs_healer::fsprops::XfsAutofsck;
use xfs_healer::healthmon::cstruct::CStructMonitor;
use xfs_healer::healthmon::event::XfsHealthEvent;
use xfs_healer::healthmon::json::JsonMonitor;
use xfs_healer::healthmon::json::JsonEventWrapper;
use xfs_healer::repair;
use xfs_healer::weakhandle::WeakHandle;
use xfs_healer::xfs_fs::xfs_fsop_geom;
use xfs_healer::xfs_fs::xfs_health_monitor_event;
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

    /// Use the JSON interface?
    #[arg(long)]
    json: bool,

    /// Repair broken metadata unconditionally
    #[arg(long)]
    repair: bool,

    /// Check to see if monitoring is supported
    #[arg(long)]
    check: bool,

    /// Decide what to do using the autofsck fs property
    #[arg(long)]
    autofsck: bool,

    /// XFS filesystem mountpoint to monitor
    path: PathBuf,
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

impl App {
    /// Return mountpoint as string, for printing messages
    fn mountpoint(&self) -> String {
        self.path.display().to_string()
    }

    /// Handle a health event that has been decoded into real objects
    fn process_event(
        et: EventThread,
        fh: Arc<WeakHandle>,
        cooked: Result<Box<dyn XfsHealthEvent>>,
    ) {
        match cooked {
            Err(e) => {
                eprintln!("{}: {}", fh.mountpoint(), e)
            }
            Ok(event) => {
                if et.log || event.must_log() {
                    let (maybe_path, message) = event.format(&fh);
                    match maybe_path {
                        Some(x) => println!("{}: {}", x.display(), message),
                        None => println!("{}: {}", fh.mountpoint(), message),
                    };
                }
                if et.repair {
                    for mut repair in event.schedule_repairs(et.everything) {
                        repair.perform(&fh)
                    }
                }
            }
        }
    }

    // fugly helpers to reduce the scope of the variables moved into the closure
    fn dispatch_json_event(
        threads: &ThreadPool,
        et: EventThread,
        fh: Arc<WeakHandle>,
        raw_event: JsonEventWrapper,
    ) {
        threads.execute(move || {
            App::process_event(et, fh, raw_event.cook());
        })
    }

    fn dispatch_cstruct_event(
        threads: &ThreadPool,
        et: EventThread,
        fh: Arc<WeakHandle>,
        raw_event: xfs_health_monitor_event,
    ) {
        threads.execute(move || {
            App::process_event(et, fh, raw_event.cook());
        })
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
        let fp = File::open(&*self.path)?;

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

        let fh = Arc::new(WeakHandle::try_new(&fp, self.path.clone(), fsgeom)?);

        // XXX debug code
        let mud = fh.instance_unit_name("xfs_scrub@.service")?;
        println!("GAWK |{:?}|", mud);

        // Creates a threadpool with nr_cpus workers.
        let threads = threadpool::Builder::new().build();

        if self.json {
            let hmon = JsonMonitor::try_new(fp, &self.path, self.everything, self.debug)?;

            for raw_event in hmon {
                App::dispatch_json_event(&threads, EventThread::new(self), fh.clone(), raw_event);
            }
        } else {
            let hmon = CStructMonitor::try_new(fp, &self.path, self.everything)?;

            for raw_event in hmon {
                App::dispatch_cstruct_event(
                    &threads,
                    EventThread::new(self),
                    fh.clone(),
                    raw_event,
                );
            }
        }
        threads.join();

        Ok(ExitCode::SUCCESS)
    }
}

impl From<Cli> for App {
    fn from(cli: Cli) -> Self {
        App {
            debug: cli.debug,
            log: cli.log,
            everything: cli.everything,
            json: cli.json,
            repair: cli.repair,
            check: cli.check,
            autofsck: cli.autofsck,
            path: Arc::new(cli.path),
        }
    }
}

fn main() -> ExitCode {
    // Checked separately from the parsed args so that users don't have to specify a path.
    if std::env::args().any(|arg| { arg == "-V" }) {
        println!("xfs_healer {}", xfsprogs::VERSION);
        return ExitCode::SUCCESS;
    }

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
