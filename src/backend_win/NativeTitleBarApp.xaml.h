#pragma once

#include "src/backend_win/NativeTitleBarApp.xaml.g.h"
#include <winrt/Microsoft.UI.Xaml.Hosting.h>

namespace winrt::AppSandbox::implementation
{
struct NativeTitleBarApp : AppT<NativeTitleBarApp>
{
    NativeTitleBarApp()
        : xaml_manager(
              winrt::Microsoft::UI::Xaml::Hosting::WindowsXamlManager::
                  InitializeForCurrentThread())
    {
        InitializeComponent();
    }

    void OnLaunched(
        winrt::Microsoft::UI::Xaml::LaunchActivatedEventArgs const &);

private:
    winrt::Microsoft::UI::Xaml::Hosting::WindowsXamlManager
        xaml_manager{nullptr};
};
}
