# Native title bar design QA

## Visual target

- Reference: `D:\Temp\codex-clipboard-7c22c1d5-84ce-4d18-a6b9-5f397f154d41.png`
- Reference viewport: 316 x 37 physical pixels
- Native comparison: `.build/winui-titlebar-reference-comparison-final.png`
- Full startup experience: `.build/winui-startup-experience-final.png`

## Matched details

- Control order is sidebar, back, forward, File, Edit, View, Help.
- The first menu label begins at the same x-position as the reference.
- The bar is 36 device-independent pixels high at 100% scale.
- No application name, logo, or preview label is repeated in the title bar.
- Minimize, maximize, close, resize, Snap, taskbar identity, and accessibility
  remain Windows-owned.
- The compact strip is a WinUI `TitleBar` hosted in the existing Win32 HWND,
  not a guest-rendered imitation or a second top-level window.

## Intentional difference

The reference uses an approximately `#202020` surface. The verified prototype
uses the application's actual `#101217` internal background for the title bar,
caption-button area, and application canvas. This difference implements the
owner's explicit direction that the native bar match the internal app.

## Interaction and resize proof

- UI Automation found the three system caption buttons and all seven framework
  controls.
- `SidebarToggleButton` completed a native-to-host round trip twice.
- 160 rapid native resizes completed in 2.707 seconds; the preview remained
  responsive and grew by about 1.4 MiB during the sample.
- Final 1100 x 720 capture showed a fully covered client without a black strip.
- Native C++ and managed WinUI preview projects build with warnings as errors.

## Final result

Passed. The compact title bar matches the selected reference structure and the
application theme, while the startup canvas remains friendly and animated.
