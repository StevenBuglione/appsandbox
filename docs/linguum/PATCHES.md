# Linguum patch stack

The `linguum` branch contains only the dynamic Windows/Linux display-sizing POC.

## LINGUUM-001 — Dynamic Windows viewer sizing

- Resolution-independent frame receive allocation
- Dynamic CPU framebuffer and D3D texture sizing
- Arbitrary native viewer sizes without the 1080p tracking maximum
- Stable asynchronous viewer startup under status polling

## LINGUUM-002 — Host-to-guest resize command

- Coalesced physical-client sizing from the Win32 viewer
- Worker-based `display_resize:<width>x<height>@60` agent requests

## LINGUUM-003 — Runtime `asb_drm` preferred-mode resizing

- Atomic `mode` sysfs attribute
- EDID, vblank, and KMS hotplug updates

## LINGUUM-004 — Linux agent resize application

- Validated `display_resize` command dispatch
- Runtime mode write through the existing Linux agent

## LINGUUM-005 — Mutter session mode stabilization

- One-shot user-session helper using Mutter's existing DisplayConfig API
- Temporary exact-mode application after the virtual KMS hotplug
- Exact arbitrary-width virtual modes without CVT's eight-pixel rounding
- Per-monitor DPI changes and full-display client tracking limits

## LINGUUM-006 — Dynamic Linux pointer calibration

- Versioned framebuffer-size hints on the existing input channel
- Exact absolute-pointer mapping across arbitrary viewer sizes
- Backward-compatible host normalization for previously installed Linux agents
- Serialized input packets during reconnect and resize transitions
- Focused edge, midpoint, and frame-size validation tests
