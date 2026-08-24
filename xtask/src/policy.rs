//! Protected quality-policy validation.

use std::fs;
use std::path::Path;

const REQUIRED_POLICY: &[&str] = &[
    "schema-version = 1",
    "architecture-rules = 14",
    "maximum-function-lines = 60",
    "maximum-function-arguments = 5",
    "maximum-nesting = 3",
    "coverage-minimum-lines = 0",
    "xtask/src/architecture.rs",
    ".github/workflows/rust-quality.yml",
];

/// Verifies that protected quality invariants remain present.
///
/// # Errors
///
/// Returns an error when the policy cannot be read or a protected setting is absent.
pub(crate) fn run(root: &Path) -> Result<(), String> {
    let path = root.join("quality.toml");
    let text = fs::read_to_string(&path)
        .map_err(|error| format!("cannot read protected policy {}: {error}", path.display()))?;
    let missing: Vec<_> = REQUIRED_POLICY
        .iter()
        .filter(|required| !text.contains(*required))
        .copied()
        .collect();
    if missing.is_empty() {
        println!("quality policy: protected invariants PASS");
        Ok(())
    } else {
        Err(format!(
            "quality policy is missing protected settings: {}",
            missing.join(", ")
        ))
    }
}
