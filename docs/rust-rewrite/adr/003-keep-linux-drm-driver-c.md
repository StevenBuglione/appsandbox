# ADR 003: Keep the Linux DRM driver in C

Status: accepted.

The qualified `tools/linux/asb_drm/` module remains C. Rust user space treats its validated
`<width>x<height>@60` sysfs interface as an external adapter boundary. Rewriting a working kernel
module would add tooling and deployment risk without improving host architecture.
