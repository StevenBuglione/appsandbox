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
