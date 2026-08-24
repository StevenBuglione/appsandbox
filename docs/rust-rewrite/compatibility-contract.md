# Compatibility contract

The exact source being characterized is `ccb1874477ca79a785a0169052050d3aa49bd190`.
The Python SDK in `tools/headless-api/asb.py`, its tests, and the existing C host are the initial
black-box oracle.

## Invariants

- Preserve `/v1/version`, `/v1/host`, `/v1/vms`, lifecycle, SSH, snapshot, branch, template,
  display, input, resize, event, and daemon-shutdown wire behavior.
- Listen only on loopback, discover a per-run bearer token through `host.json`, preserve the
  single-owner daemon, and keep status polling as level-triggered truth.
- Display readiness is passive: it never opens the single-consumer framebuffer channel.
- Preserve `display_resize:<width>x<height>@60` and typed failure parity.
- Preserve `IRDY` and `IRV2`; `IRV2` receives the current framebuffer geometry.
- Map a clamped coordinate in `0..extent-1` to `0..32767`; `extent <= 1` maps to zero.
- Accept display sizes from 64x64 through 7680x4320, including odd widths without CVT rounding.
- Preserve exact, temporary Mutter mode application, current transform/layout, and supported scale.
- Preserve prebuilt Linux VHDX import through owned storage and the normal HCS/device stack.
- Preserve on-disk VM, snapshot, branch, template, artifact-cache, and process-ownership semantics.

## Characterization evidence

On 2026-08-23 the untouched M4 C host/core and Windows application built in Release x64. The full
solution additionally reported missing local WDK driver toolsets; the already-proven drivers are
out of rewrite scope. Against the running C daemon, `test_host_and_validation.py` passed all host,
capability, and input-validation checks, and `test_application_display_client.py` passed 7/7.

Protocol fixtures under `docs/rust-rewrite/fixtures/` freeze representative compatibility bytes.
