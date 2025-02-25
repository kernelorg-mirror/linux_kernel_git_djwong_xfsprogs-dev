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
use xfs_healer::healthmon::json::XfsHealthMonitor as JsonMonitor;
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
    path: PathBuf,
}

impl App {
    /// Return mountpoint as string, for printing messages
    fn mountpoint(&self) -> String {
        self.path.display().to_string()
    }

    /// Main app method
    fn main(&self) -> Result<ExitCode> {
        if self.version {
            println!("xfs_healer {}", xfsprogs::VERSION);
            return Ok(ExitCode::SUCCESS);
        }

        let fp = File::open(&self.path)?;
        let hmon = JsonMonitor::try_new(fp, &self.path, self.everything, self.debug)?;

        for raw_event in hmon {
            if self.log {
                println!("{}", raw_event);
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
