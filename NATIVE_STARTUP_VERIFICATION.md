# Native startup experience verification

## Scope

This checkpoint adds a host-owned startup canvas, theme-matched Windows chrome,
and the qualified Windows App SDK compact framework controls to the production
application-display HWND. It does not create a splash window, render controls
in the guest, or wait for the guest before showing useful pixels.

## Automated verification

- Release x64 `AppSandbox.exe` rebuild: passed.
- Release x64 `NativeStartupPreview.exe` rebuild with level-four warnings and
  warnings-as-errors: passed.
- Application display client and native architecture tests: 13 passed.
- Native Win32 + WinUI XAML Island title-bar build: passed.
- Managed WinUI startup-experience build: passed with zero warnings.
- Diff whitespace validation: passed.
- UTF-8 source compilation is explicit so native friendly copy and thinking
  glyphs render correctly.

The focused tests cover the existing flip-model swap chain, startup texture
selection, partial texture update path, non-blocking render worker, native
window style, DWM attributes, High Contrast guard, reduced-motion refresh,
taskbar progress, bounded API fields, and application-mode-only enforcement.

## Visual and interaction verification

The isolated previews use the production `vm_startup_scene` painter and
`vm_window_chrome` policy without opening a VM or touching the running daemon.
Local evidence is under `.build/` (the original painter captures are in
`.build/native-startup-preview/`):

- `opening-utf8.png`: native caption and Unicode copy/glyphs.
- `resized-preparing.png`: bottom-left default after growth to 1420x900.
- `failed-details.png`: friendly failure with technical details enabled.
- `center-fixed.png`: centered, no-motion customization.
- `maximized.png`: maximized native window with a fully covered client.
- `rapid-resize-final-client.png`: client after 160 rapid native resizes.
- `winui-titlebar-reference-comparison-final.png`: the 316x37 reference above
  the native WinUI result.
- `winui-titlebar-island-final.png`: the compact WinUI strip hosted inside an
  ordinary Win32 top-level HWND.
- `winui-startup-experience-final.png`: the animated friendly startup canvas
  with the same compact title bar.

Measured results:

- `WS_OVERLAPPEDWINDOW` remains present; the preview has one top-level HWND.
- DWM reported immersive-dark mode enabled for the dark custom caption and
  rounded corner preference `2`.
- Native maximize and restore returned from 2560x1400 outer bounds to the exact
  original 1016x679 outer bounds.
- 160 rapid resize operations completed in 1.132 seconds; the process remained
  responsive and consumed 0.062 CPU seconds during that sample.
- The final 1384x821 client had the configured `#111318` color at every sampled
  edge and corner, with zero pure-black pixels.
- Bottom-left, center, animation-disabled, delayed copy, details, and failure
  variants were visually inspected. A center-placement defect found during
  inspection was corrected and reverified.
- UI Automation found Windows-owned minimize, maximize, and close plus sidebar,
  back, forward, File, Edit, View, and Help.
- The sidebar button completed two native round trips through the XAML Island.
- The 36-pixel strip contains no repeated application name or preview label.
- Its background and caption-button area use the same `#101217` surface as the
  internal application instead of copying the reference's lighter gray.
- The XAML Island survived 160 rapid resizes in 2.707 seconds, remained
  responsive, and increased its working set by about 1.4 MiB during the sample.
- Detailed visual comparison is recorded in
  `tools/winui-startup-preview/design-qa.md`.

## Integration boundary

The production Release x64 daemon was deliberately replaced by exact PID and
smoked against `linguum-framework-m2`. The resulting single HWND contained the
host-owned startup canvas and the compact WinUI title bar, with the genuine
Windows minimize, maximize, close, frame, resize, shadow, and rounded corners.

Production evidence:

- `production-titlebar-final.png`: compact controls and Windows caption buttons
  in the live VM application window.
- API geometry: 1320x800 content, 36-pixel title bar, and 1320x836 render
  surface on the initial sample.
- 160 coalesced resize operations completed in 3.409 seconds; every API result
  retained exact content geometry and the hosted title bar, and the final
  render converged to 1355x995 for a 1355x959 content area.
- Maximum resize-call latency was 35.122 ms. The daemon remained responsive;
  working-set growth during the live sample was about 11.43 MiB.
- Native maximize reached the 2560x1400 work area and restore returned to the
  exact prior 1371x1003 outer bounds.
- Closing the first native host returned success without terminating the
  daemon. The production AppHost boundary is one WinUI `Application` per
  process, matching Microsoft's island architecture; AppSandbox deliberately
  prevents a second compact host in the same qualification daemon process.

The private AppSandbox adapter is now sufficient for the M4.5 native-window
smoke. Public BaseWindow mapping remains in the runtime repository and exposes
no VM, transport, or HWND identifiers.
