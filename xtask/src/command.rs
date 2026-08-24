//! Child-command orchestration for quality checks.

use std::path::Path;
use std::process::Command;

pub(crate) struct Step {
    label: &'static str,
    program: &'static str,
    arguments: &'static [&'static str],
    environment: Option<(&'static str, &'static str)>,
}

impl Step {
    /// Creates a Cargo-backed quality step.
    pub(crate) const fn cargo(label: &'static str, arguments: &'static [&'static str]) -> Self {
        Self {
            label,
            program: "cargo",
            arguments,
            environment: None,
        }
    }

    /// Creates a Cargo-backed quality step with one explicit environment value.
    pub(crate) const fn cargo_with_env(
        label: &'static str,
        arguments: &'static [&'static str],
        key: &'static str,
        value: &'static str,
    ) -> Self {
        Self {
            label,
            program: "cargo",
            arguments,
            environment: Some((key, value)),
        }
    }
}

/// Runs the quality steps sequentially and prints a compact result ledger.
///
/// # Errors
///
/// Returns an error when a command cannot start or exits unsuccessfully.
pub(crate) fn run_steps(root: &Path, steps: &[Step]) -> Result<(), String> {
    for (index, step) in steps.iter().enumerate() {
        print!(
            "[{number:02}] {label:.<38}",
            number = index + 1,
            label = step.label
        );
        run_step(root, step)?;
        println!(" PASS");
    }
    println!("\nREADY");
    Ok(())
}

/// Runs one child process in the repository root.
///
/// # Errors
///
/// Returns an error when the process cannot start or exits unsuccessfully.
fn run_step(root: &Path, step: &Step) -> Result<(), String> {
    let mut command = Command::new(step.program);
    command.current_dir(root).args(step.arguments);
    if let Some((key, value)) = step.environment {
        command.env(key, value);
    }
    let status = command
        .status()
        .map_err(|error| format!("could not run {}: {error}", step.label))?;
    if status.success() {
        Ok(())
    } else {
        Err(format!("{} failed with {status}", step.label))
    }
}
