//! Enforced crate dependency and source-code boundaries.

use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

const RULE_COUNT: usize = 14;

struct SourceRule {
    id: &'static str,
    crate_name: &'static str,
    forbidden: &'static [&'static str],
}

const SOURCE_RULES: &[SourceRule] = &[
    SourceRule {
        id: "ASB-ARCH-002/003/004/013/014",
        crate_name: "asb-domain",
        forbidden: &[
            "asb_application",
            "axum::",
            "serde::",
            "serde_json",
            "tokio::",
            "windows::",
            "windows_sys::",
            "HttpDto",
            "RawHwnd",
            "HcsHandle",
        ],
    },
    SourceRule {
        id: "ASB-ARCH-005/006/007/012/013/014",
        crate_name: "asb-application",
        forbidden: &[
            "asb_display_win",
            "asb_windows",
            "axum::",
            "HttpDto",
            "RawHwnd",
            "HcsHandle",
        ],
    },
];

const DEPENDENCY_RULES: &[(&str, &[&str])] = &[
    (
        "asb-domain",
        &[
            "asb-application",
            "asb-windows",
            "asb-display-win",
            "axum",
            "tokio",
            "serde",
            "serde_json",
            "windows",
            "windows-sys",
        ],
    ),
    (
        "asb-application",
        &["asb-windows", "asb-display-win", "asb-headless", "axum"],
    ),
];

/// Enforces all declared architecture rules against metadata and source.
///
/// # Errors
///
/// Returns an error when metadata or source cannot be read or a rule is violated.
pub(crate) fn run(root: &Path) -> Result<(), String> {
    verify_metadata(root)?;
    let mut violations = dependency_violations(root)?;
    violations.extend(source_violations(root)?);
    violations.extend(unsafe_violations(root)?);
    if violations.is_empty() {
        println!("architecture: {RULE_COUNT} rules PASS");
        Ok(())
    } else {
        Err(format!(
            "architecture violations:\n{}",
            violations.join("\n")
        ))
    }
}

/// Uses Cargo metadata to reject invalid manifests and dependency cycles.
///
/// # Errors
///
/// Returns an error when Cargo cannot run or reports invalid metadata.
fn verify_metadata(root: &Path) -> Result<(), String> {
    let status = Command::new("cargo")
        .args(["metadata", "--format-version", "1", "--locked", "--no-deps"])
        .current_dir(root)
        .stdout(Stdio::null())
        .status()
        .map_err(|error| format!("ASB-ARCH-010 could not run cargo metadata: {error}"))?;
    if status.success() {
        Ok(())
    } else {
        Err(format!("ASB-ARCH-010 cargo metadata failed with {status}"))
    }
}

/// Finds forbidden dependency declarations in protected crate manifests.
///
/// # Errors
///
/// Returns an error when a protected manifest cannot be read.
fn dependency_violations(root: &Path) -> Result<Vec<String>, String> {
    let mut violations = Vec::new();
    for (crate_name, forbidden) in DEPENDENCY_RULES {
        let manifest = root.join("crates").join(crate_name).join("Cargo.toml");
        let text = read(&manifest)?;
        violations.extend(manifest_violations(crate_name, &text, forbidden));
    }
    Ok(violations)
}

/// Compares one manifest with the dependencies forbidden for its crate.
fn manifest_violations(crate_name: &str, text: &str, forbidden: &[&str]) -> Vec<String> {
    forbidden
        .iter()
        .filter(|dependency| manifest_declares(text, dependency))
        .map(|dependency| {
            format!("ASB-ARCH dependency: {crate_name} cannot depend on {dependency}")
        })
        .collect()
}

/// Reports whether a manifest declares a specific dependency key.
fn manifest_declares(text: &str, dependency: &str) -> bool {
    text.lines().any(|line| {
        let trimmed = line.trim_start();
        trimmed.starts_with(&format!("{dependency} ="))
            || trimmed.starts_with(&format!("{dependency}."))
    })
}

/// Finds forbidden implementation tokens in pure and application crates.
///
/// # Errors
///
/// Returns an error when protected source files cannot be enumerated or read.
fn source_violations(root: &Path) -> Result<Vec<String>, String> {
    let mut violations = Vec::new();
    for rule in SOURCE_RULES {
        let source = root.join("crates").join(rule.crate_name).join("src");
        for file in rust_files(&source)? {
            let text = read(&file)?;
            violations.extend(forbidden_tokens(rule, &file, &text));
        }
    }
    Ok(violations)
}

/// Formats every forbidden source token found in one file.
fn forbidden_tokens(rule: &SourceRule, file: &Path, text: &str) -> Vec<String> {
    rule.forbidden
        .iter()
        .filter(|token| text.contains(*token))
        .map(|token| {
            format!(
                "{} {} contains forbidden token `{token}` in {}",
                rule.id,
                rule.crate_name,
                file.display()
            )
        })
        .collect()
}

/// Finds unsafe Rust outside the explicitly approved native adapter directories.
///
/// # Errors
///
/// Returns an error when workspace source files cannot be enumerated or read.
fn unsafe_violations(root: &Path) -> Result<Vec<String>, String> {
    let crates = root.join("crates");
    let mut violations = Vec::new();
    for file in rust_files(&crates)? {
        let normalized = file.to_string_lossy().replace('\\', "/");
        let approved = normalized.contains("/asb-windows/src/native/")
            || normalized.contains("/asb-display-win/src/native/")
            || normalized.contains("/asb-linux-input/src/native/");
        if !approved && contains_unsafe_code(&read(&file)?) {
            violations.push(format!(
                "ASB-ARCH-009 unsafe code is not approved in {}",
                file.display()
            ));
        }
    }
    Ok(violations)
}

/// Detects executable unsafe blocks, functions, and implementations.
fn contains_unsafe_code(text: &str) -> bool {
    text.lines().map(str::trim).any(|line| {
        !line.starts_with("//")
            && (line.contains("unsafe {")
                || line.contains("unsafe fn")
                || line.contains("unsafe impl"))
    })
}

/// Recursively lists Rust source files below a directory.
///
/// # Errors
///
/// Returns an error when the directory tree cannot be read.
fn rust_files(directory: &Path) -> Result<Vec<PathBuf>, String> {
    let mut files = Vec::new();
    visit(directory, &mut files)?;
    Ok(files)
}

/// Adds Rust source paths below one directory to the result list.
///
/// # Errors
///
/// Returns an error when a directory entry cannot be read.
fn visit(directory: &Path, files: &mut Vec<PathBuf>) -> Result<(), String> {
    for entry in fs::read_dir(directory)
        .map_err(|error| format!("cannot read {}: {error}", directory.display()))?
    {
        let path = entry
            .map_err(|error| format!("cannot read directory entry: {error}"))?
            .path();
        if path.is_dir() {
            visit(&path, files)?;
        } else if path.extension().is_some_and(|extension| extension == "rs") {
            files.push(path);
        }
    }
    Ok(())
}

/// Reads one UTF-8 source or manifest file.
///
/// # Errors
///
/// Returns an error when the file is absent, unreadable, or not UTF-8.
fn read(path: &Path) -> Result<String, String> {
    fs::read_to_string(path).map_err(|error| format!("cannot read {}: {error}", path.display()))
}

#[cfg(test)]
mod tests {
    use super::{contains_unsafe_code, manifest_violations};

    /// Proves a forbidden domain-to-application edge is rejected.
    ///
    /// # Panics
    ///
    /// Panics if the checker fails to report exactly one violation.
    #[test]
    fn rejects_forbidden_dependency_edge() {
        let manifest = "[dependencies]\nasb-application = { path = \"../asb-application\" }\n";
        let violations = manifest_violations("asb-domain", manifest, &["asb-application"]);
        assert_eq!(violations.len(), 1);
    }

    /// Proves an unrelated dependency is accepted.
    ///
    /// # Panics
    ///
    /// Panics if the checker reports a false positive.
    #[test]
    fn accepts_unrelated_dependency_edge() {
        let manifest = "[dependencies]\nthiserror = \"2\"\n";
        let violations = manifest_violations("asb-domain", manifest, &["asb-application"]);
        assert!(violations.is_empty());
    }

    /// Proves documentation examples do not trigger the unsafe-code rule.
    ///
    /// # Panics
    ///
    /// Panics if code and documentation are not distinguished.
    #[test]
    fn distinguishes_unsafe_code_from_documentation() {
        assert!(!contains_unsafe_code(
            "//! unsafe code policy\n// unsafe { example }"
        ));
        assert!(contains_unsafe_code("unsafe { native_call(); }"));
    }
}
