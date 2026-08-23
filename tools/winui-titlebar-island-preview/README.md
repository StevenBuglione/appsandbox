# Native Win32 + WinUI title-bar island gate

This project proves that the framework's compact WinUI `TitleBar` can live in
the same ordinary Win32 HWND class used by the native presentation host. It is
not a second application window and it does not render controls in the guest.

Restore the packages listed in `packages.config`, then build:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe' `
  tools\winui-titlebar-island-preview\NativeTitleBarIslandPreview.vcxproj `
  /m /t:Build /p:Configuration=Release /p:Platform=x64 /nologo
```

The preview deliberately remains framework-dependent so the Windows App SDK
runtime is shared instead of copying its full payload beside each application.
It hosts the XAML Island in the native client, leaves the system caption
buttons to Windows, forwards messages through `ContentPreTranslateMessage`, and
uses AppWindow drag rectangles for native move behavior.
