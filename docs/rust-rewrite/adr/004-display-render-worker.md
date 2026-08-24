# ADR 004: Dedicated display workers

Status: accepted.

The Win32 thread owns the window and message pump; a render worker owns D3D/DXGI resources; a frame
receiver owns the frame channel; a resize coordinator coalesces guest requests. `WM_SIZE` only
publishes state and never performs network, guest modeset, or D3D work synchronously.
