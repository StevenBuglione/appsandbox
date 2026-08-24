# AppSandbox Rust rewrite

This directory records implementation-specific decisions for the side-by-side Rust rewrite.
The canonical Linguum framework governance remains in `StevenBuglione/linguum-runtime` as
required by [`LINGUUM_FRAMEWORK.md`](../../LINGUUM_FRAMEWORK.md).

## Source lock

- AppSandbox source: `ccb1874477ca79a785a0169052050d3aa49bd190`
- Upstream-derived base: `72c06267a51d90bbde7d2edcb9e4e32468f21b07`
- Rust rewrite branch: `codex/rust-rewrite`
- Branch created: 2026-08-23

The existing C implementation remains the behavioral oracle until the Rust executable passes
the existing Python headless suite and real-VM qualification.

## Commands

```text
cargo xtask architecture
cargo xtask ready
```

`cargo xtask ready` is the authoritative local quality gate. CI calls the same command.
The code-empty Phase 1 foundation runs LLVM instrumentation without a numeric coverage percentage;
the protected 80% line gate is enabled with the first executable domain/protocol code in Phase 2.
