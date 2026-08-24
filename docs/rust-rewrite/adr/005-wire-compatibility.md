# ADR 005: Wire compatibility first

Status: accepted.

The existing HTTP, guest-agent, input, and frame formats remain stable until the Rust implementation
passes the old Python SDK and old/new guest combinations. Typed internal models may improve safety,
but boundary adapters preserve bytes, status codes, failure concepts, and passive readiness.
