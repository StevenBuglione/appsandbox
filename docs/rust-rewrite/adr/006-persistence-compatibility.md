# ADR 006: Persistence compatibility

Status: accepted.

Rust loads existing VM, snapshot, branch, template, artifact-cache, and discovery metadata. Writes
use atomic replace and preserve process-owned cleanup. Format evolution requires explicit versioning
and migration; infrastructure details do not enter domain APIs.
