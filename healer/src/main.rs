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
            .get_matches())
    }
}

/// Contains all the global program state but allows more flexibility.
#[derive(Debug)]
struct App {
    debug: bool,
    log: bool,
    everything: bool,
    path: PathBuf,
}

impl App {
    /// Return mountpoint as string, for printing messages
    fn mountpoint(&self) -> String {
        self.path.display().to_string()
    }

    /// Main app method
    fn main(&self) -> Result<ExitCode> {
        let _fp = File::open(&self.path)?;

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
        }
    }
}

fn main() -> ExitCode {
    xfsprogs::init_localization();

    let args = Cli::new();
    if args.0.get_flag("version") {
        println!("{} {}", M_("xfs_healer version"), xfsprogs::VERSION);
        return ExitCode::SUCCESS;
    }

    if args.0.get_flag("debug") {
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
