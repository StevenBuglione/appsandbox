# Unsafe inventory

No Rust unsafe code exists in the foundation crates.

Approved future containment areas are:

| Subsystem | Location | Reason | Safe boundary | Required tests |
| --- | --- | --- | --- | --- |
| HCS/HCN and Win32 FFI | `crates/asb-windows/src/native/` | Native Windows APIs | Owner-thread commands and RAII handles | lifecycle, cancellation, drop order |
| HWND/D3D/DXGI | `crates/asb-display-win/src/native/` | Window and graphics APIs | window/render worker messages | open/close/reconnect/resize stress |
| Linux uinput ioctl | `crates/asb-linux-input/src/native/` | Kernel input ABI | validated packet-to-event writer | golden packets and property tests |

Every introduction of unsafe code must update this table with the precise file, invariant, wrapper,
and tests. `cargo xtask architecture` rejects unsafe code outside the approved directories.
