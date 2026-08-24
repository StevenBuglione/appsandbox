# ADR 001: Strangler rewrite

Status: accepted.

Build `appsandbox-rs` beside the C implementation. Keep C runnable as the oracle until the existing
Python suite and real workloads prove parity. This contains migration risk and permits subsystem-by-
subsystem replacement without changing the public contract.
