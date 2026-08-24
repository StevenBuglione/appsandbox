# Qualification

## Preserved product baseline

- Host: Windows 11 x64
- Guest: Ubuntu 26.04 LTS, 8 vCPU, 16 GB RAM, 64 GB disk
- GPU: GPU-PV (qualified example: NVIDIA GeForce RTX 3070)
- Firefox, Widevine, Netflix, Prime Video, audio: PASS

This is a regression contract, not a claim about the Rust implementation yet.

## Evidence ledger

| Date | Target | Result |
| --- | --- | --- |
| 2026-08-23 | C Release x64 host/core/application | PASS |
| 2026-08-23 | C solution driver projects | BLOCKED: local WDK toolsets not installed |
| 2026-08-23 | Python no-VM host and validation oracle | PASS |
| 2026-08-23 | Python application-display client tests | PASS (7/7) |
| 2026-08-23 | Linux host `venus` connectivity | PASS |
| 2026-08-23 | Apple Silicon host `mac-mini` connectivity | PASS |

Rust claims are added only with command output and, for product behavior, real-VM evidence.
