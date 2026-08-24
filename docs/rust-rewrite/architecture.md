# Architecture

The rewrite is a strangler migration. Rust is built as `appsandbox-rs` beside the proven C host;
the C binary is not replaced until black-box parity and workload qualification are complete.

Dependency direction is enforced by `cargo xtask architecture`:

```text
runtime -> headless / windows / display-win -> application -> domain
                    protocol is a sibling wire-contract crate
```

The domain is pure. The application layer owns narrow capability ports. Native adapters own
platform handles and translate infrastructure failures. The runtime is the only composition root.
HCS and D3D resources will remain on dedicated owner threads, and asynchronous work carries stable
IDs and generation numbers instead of references into mutable registries.

The Linux `asb_drm` module, WDK drivers, Mesa, QEMU, and signed artifacts remain native external
components. Their qualified protocols are adapter boundaries, not rewrite targets.
