# WinUI startup preview

This standalone visual gate shows the default framework startup experience
without starting a VM or replacing the active AppSandbox daemon. It uses a
real WinUI window and Windows-owned caption buttons.

Build from the repository root:

```powershell
dotnet build tools\winui-startup-preview\WinUIStartupPreview.csproj `
  -c Release -p:Platform=x64 --nologo
```

The default compact title bar contains sidebar, back, forward, File, Edit,
View, and Help. The View menu toggles the startup sidebar and the optional
loading details. The friendly status line changes periodically while the orbit
uses compositor-driven animation.

The title bar and caption-button area use the same `#101217` background as the
application canvas. `design-qa.md` records the reference comparison and native
interaction evidence.
