//! Project-owned quality command entry point.

#![forbid(unsafe_code)]

mod architecture;
mod command;
mod policy;

use std::env;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("xtask: {error}");
            ExitCode::FAILURE
        }
    }
}

/// Dispatches the selected project command.
///
/// # Errors
///
/// Returns an error when the command is unknown or a selected check fails.
fn run() -> Result<(), String> {
    let command = env::args().nth(1).unwrap_or_else(|| "help".to_owned());
    let root = workspace_root()?;
    match command.as_str() {
        "architecture" => architecture::run(&root),
        "policy" => policy::run(&root),
        "ready" => ready(&root),
        "help" | "--help" | "-h" => {
            print_help();
            Ok(())
        }
        other => Err(format!("unknown command `{other}`; use `cargo xtask help`")),
    }
}

/// Resolves the repository root from the `xtask` manifest directory.
///
/// # Errors
///
/// Returns an error if the manifest directory does not have a parent.
fn workspace_root() -> Result<PathBuf, String> {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .map(Path::to_path_buf)
        .ok_or_else(|| "xtask manifest has no workspace parent".to_owned())
}

/// Prints the supported project commands.
fn print_help() {
    println!("cargo xtask architecture  enforce dependency and source boundaries");
    println!("cargo xtask policy        validate protected quality policy");
    println!("cargo xtask ready         run the authoritative local quality gate");
}

/// Runs every required local completion check in policy order.
///
/// # Errors
///
/// Returns the first failed quality step.
fn ready(root: &Path) -> Result<(), String> {
    let steps = [
        command::Step::cargo("formatting", &["fmt", "--all", "--", "--check"]),
        command::Step::cargo(
            "cargo check",
            &["check", "--workspace", "--all-targets", "--locked"],
        ),
        command::Step::cargo(
            "clippy",
            &[
                "clippy",
                "--workspace",
                "--all-targets",
                "--all-features",
                "--locked",
                "--",
                "-D",
                "warnings",
            ],
        ),
        command::Step::cargo("architecture", &["xtask", "architecture"]),
        command::Step::cargo("policy integrity", &["xtask", "policy"]),
        command::Step::cargo("dependency hygiene", &["shear", "--deny-warnings"]),
        command::Step::cargo("dependency policy", &["deny", "check"]),
        command::Step::cargo(
            "unit and contract tests",
            &["nextest", "run", "--profile", "ci", "--locked"],
        ),
        command::Step::cargo("doctests", &["test", "--workspace", "--doc", "--locked"]),
        command::Step::cargo_with_env(
            "rustdoc",
            &["doc", "--workspace", "--no-deps", "--locked"],
            "RUSTDOCFLAGS",
            "-Dwarnings",
        ),
        command::Step::cargo(
            "coverage",
            &[
                "llvm-cov",
                "--workspace",
                "--exclude",
                "xtask",
                "--exclude",
                "asb-runtime",
                "--all-features",
                "--no-report",
            ],
        ),
    ];
    command::run_steps(root, &steps)
}
