#include "vm_native_titlebar.h"
#include "NativeTitleBarApp.xaml.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef GetCurrentTime

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>

#include <Microsoft.UI.Dispatching.Interop.h>
#include <winrt/Microsoft.UI.Content.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.UI.h>

using namespace winrt;
using namespace Microsoft::UI;
using namespace Microsoft::UI::Dispatching;
using namespace Microsoft::UI::Windowing;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Hosting;
using namespace Microsoft::UI::Xaml::Media;
using namespace Windows::Graphics;
using namespace Windows::UI;

namespace
{
constexpr int title_bar_height_dip = 36;
constexpr int button_width_dip = 32;
constexpr int desktop_menu_width_dip = 156;
/* A WinUI Application is a process singleton and is intentionally created
   once for the lifetime of a native AppHost process. AppSandbox's daemon is
   only the private qualification adapter; a second compact host must run in a
   fresh process, matching the production one-AppHost-per-process boundary. */
std::atomic<bool> native_app_runtime_consumed{false};

Color color_from_ref(COLORREF value)
{
    return Color{
        0xff,
        GetRValue(value),
        GetGValue(value),
        GetBValue(value),
    };
}

bool high_contrast_enabled()
{
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof(contrast);
    return SystemParametersInfoW(
               SPI_GETHIGHCONTRAST,
               sizeof(contrast),
               &contrast,
               0) != FALSE &&
           (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

SolidColorBrush brush(Color value)
{
    return SolidColorBrush{value};
}

COLORREF effective_background(const AsbWindowChromeOptions &options)
{
    if (high_contrast_enabled())
        return GetSysColor(COLOR_WINDOW);
    if (options.has_caption_color)
        return options.caption_color;
    return options.theme == ASB_TITLE_BAR_LIGHT
               ? RGB(243, 243, 243)
               : RGB(16, 18, 23);
}

COLORREF effective_foreground(const AsbWindowChromeOptions &options)
{
    if (high_contrast_enabled())
        return GetSysColor(COLOR_WINDOWTEXT);
    if (options.has_text_color)
        return options.text_color;
    return options.theme == ASB_TITLE_BAR_LIGHT
               ? RGB(31, 31, 31)
               : RGB(216, 216, 216);
}
}

struct VmNativeTitleBar
{
    HWND hwnd{};
    UINT action_message{};
    AsbWindowChromeOptions options{};
    UINT height_px{title_bar_height_dip};
    int controls_width_dip{};
    bool sidebar_visible{true};

    DispatcherQueueController dispatcher{nullptr};
    Application application{nullptr};
    DesktopWindowXamlSource source{nullptr};
    AppWindow app_window{nullptr};
    TitleBar title_bar{nullptr};
    StackPanel navigation{nullptr};
    MenuBar menu_bar{nullptr};
};

namespace
{
Button icon_button(
    wchar_t const *glyph,
    wchar_t const *accessible_name,
    bool enabled)
{
    Button button;
    button.Width(button_width_dip);
    button.Height(title_bar_height_dip);
    button.Padding(Thickness{});
    button.Margin(Thickness{});
    button.BorderThickness(Thickness{});
    button.CornerRadius(CornerRadius{});
    button.IsEnabled(enabled);
    button.IsTabStop(enabled);

    FontIcon icon;
    icon.FontFamily(FontFamily{L"Segoe Fluent Icons"});
    icon.FontSize(14);
    icon.Glyph(glyph);
    button.Content(icon);
    Automation::AutomationProperties::SetName(button, accessible_name);
    return button;
}

MenuBarItem menu(wchar_t const *title)
{
    MenuBarItem item;
    item.Title(title);
    return item;
}

MenuFlyoutItem menu_item(wchar_t const *text, bool enabled = true)
{
    MenuFlyoutItem item;
    item.Text(text);
    item.IsEnabled(enabled);
    return item;
}

void update_drag_region(VmNativeTitleBar *state)
{
    RECT client{};
    if (!state || !state->app_window ||
        !GetClientRect(state->hwnd, &client))
        return;

    auto native_title_bar = state->app_window.TitleBar();
    auto dpi = GetDpiForWindow(state->hwnd);
    auto left = MulDiv(state->controls_width_dip, dpi, 96);
    auto right = client.right -
                 static_cast<int>(native_title_bar.RightInset());
    if (right <= left) {
        native_title_bar.SetDragRectangles({});
        return;
    }
    native_title_bar.SetDragRectangles(
        {RectInt32{left, 0, right - left, static_cast<int>(state->height_px)}});
}

void apply_colors(VmNativeTitleBar *state)
{
    auto background = color_from_ref(effective_background(state->options));
    auto foreground = color_from_ref(effective_foreground(state->options));
    auto inactive = foreground;
    inactive.A = 0xa0;

    state->title_bar.Background(brush(background));
    state->title_bar.Foreground(brush(foreground));
    state->navigation.Background(brush(background));
    for (auto const &child : state->navigation.Children()) {
        if (auto button = child.try_as<Button>()) {
            button.Background(brush(background));
            button.Foreground(brush(foreground));
        }
    }
    if (state->menu_bar) {
        state->menu_bar.Background(brush(background));
        state->menu_bar.Foreground(brush(foreground));
        for (auto const &item : state->menu_bar.Items())
            item.Foreground(brush(foreground));
    }

    auto native_title_bar = state->app_window.TitleBar();
    native_title_bar.ButtonBackgroundColor(background);
    native_title_bar.ButtonInactiveBackgroundColor(background);
    native_title_bar.ButtonForegroundColor(foreground);
    native_title_bar.ButtonInactiveForegroundColor(inactive);
}

TitleBar create_title_bar(VmNativeTitleBar *state)
{
    TitleBar title_bar;
    title_bar.Height(title_bar_height_dip);

    StackPanel navigation;
    navigation.Orientation(Orientation::Horizontal);
    navigation.Spacing(0);
    state->navigation = navigation;

    if (state->options.sidebar_toggle_visible) {
        auto sidebar = icon_button(L"\xE8A0", L"Toggle sidebar", true);
        Automation::AutomationProperties::SetAutomationId(
            sidebar,
            L"SidebarToggleButton");
        sidebar.Click([state](Windows::Foundation::IInspectable const &,
                              RoutedEventArgs const &) {
            state->sidebar_visible = !state->sidebar_visible;
            PostMessageW(
                state->hwnd,
                state->action_message,
                ASB_NATIVE_TITLE_BAR_TOGGLE_SIDEBAR,
                state->sidebar_visible ? 1 : 0);
        });
        navigation.Children().Append(sidebar);
        state->controls_width_dip += button_width_dip;
    }

    if (state->options.navigation_visible) {
        auto back = icon_button(L"\xE72B", L"Back", false);
        Automation::AutomationProperties::SetAutomationId(back, L"BackButton");
        navigation.Children().Append(back);
        auto forward = icon_button(L"\xE72A", L"Forward", false);
        Automation::AutomationProperties::SetAutomationId(
            forward,
            L"ForwardButton");
        navigation.Children().Append(forward);
        state->controls_width_dip += button_width_dip * 2;
    }

    if (state->options.desktop_menu_visible) {
        MenuBar menu_bar;
        menu_bar.Height(title_bar_height_dip);
        menu_bar.Margin(Thickness{-10, 0, 0, 0});
        menu_bar.Padding(Thickness{});

        auto file = menu(L"File");
        file.Items().Append(menu_item(L"New Window", false));
        auto exit = menu_item(L"Exit");
        exit.Click([state](Windows::Foundation::IInspectable const &,
                           RoutedEventArgs const &) {
            PostMessageW(state->hwnd, WM_CLOSE, 0, 0);
        });
        file.Items().Append(exit);

        auto edit = menu(L"Edit");
        edit.Items().Append(menu_item(L"Undo", false));
        edit.Items().Append(menu_item(L"Redo", false));

        auto view = menu(L"View");
        auto sidebar_item = ToggleMenuFlyoutItem{};
        sidebar_item.Text(L"Sidebar");
        sidebar_item.IsChecked(state->sidebar_visible);
        sidebar_item.Click(
            [state, sidebar_item](Windows::Foundation::IInspectable const &,
                                  RoutedEventArgs const &) {
                state->sidebar_visible = !state->sidebar_visible;
                sidebar_item.IsChecked(state->sidebar_visible);
                PostMessageW(
                    state->hwnd,
                    state->action_message,
                    ASB_NATIVE_TITLE_BAR_TOGGLE_SIDEBAR,
                    state->sidebar_visible ? 1 : 0);
            });
        view.Items().Append(sidebar_item);

        auto help = menu(L"Help");
        help.Items().Append(menu_item(L"About Linguum", false));

        menu_bar.Items().Append(file);
        menu_bar.Items().Append(edit);
        menu_bar.Items().Append(view);
        menu_bar.Items().Append(help);
        navigation.Children().Append(menu_bar);
        state->menu_bar = menu_bar;
        state->controls_width_dip += desktop_menu_width_dip;
    }

    title_bar.LeftHeader(navigation);
    return title_bar;
}
}

extern "C" VmNativeTitleBar *vm_native_titlebar_create(
    HWND hwnd,
    const AsbWindowChromeOptions *options,
    UINT action_message)
{
    if (!hwnd || !options || options->layout != ASB_TITLE_BAR_COMPACT)
        return nullptr;
    if (native_app_runtime_consumed.exchange(true))
        return nullptr;

    try {
        auto state = std::make_unique<VmNativeTitleBar>();
        state->hwnd = hwnd;
        state->action_message = action_message;
        state->options = *options;

        state->dispatcher = DispatcherQueueController::CreateOnCurrentThread();
        /* Standard WinUI controls require an Application object for metadata
           and the XamlControlsResources declared in NativeTitleBarApp.xaml.
           The isolated proof already exercised this exact startup order. */
        state->application = make<
            AppSandbox::implementation::NativeTitleBarApp>();

        auto window_id = GetWindowIdFromWindow(hwnd);
        state->app_window = AppWindow::GetFromWindowId(window_id);
        auto native_title_bar = state->app_window.TitleBar();
        native_title_bar.ExtendsContentIntoTitleBar(true);
        native_title_bar.PreferredHeightOption(TitleBarHeightOption::Standard);

        state->source = DesktopWindowXamlSource{};
        state->source.Initialize(window_id);
        state->title_bar = create_title_bar(state.get());
        state->source.Content(state->title_bar);
        apply_colors(state.get());
        vm_native_titlebar_resize(state.get());
        return state.release();
    } catch (...) {
        return nullptr;
    }
}

extern "C" void vm_native_titlebar_destroy(VmNativeTitleBar *title_bar)
{
    if (!title_bar)
        return;
    try {
        if (IsWindow(title_bar->hwnd)) {
            vm_native_titlebar_close_island(title_bar);
        } else {
            /* Owner-window destruction already closed the site bridge. Calling
               Content or Close on that invalidated projection is a use-after-
               close inside the generated WinRT ABI. Drop references only. */
            title_bar->menu_bar = nullptr;
            title_bar->navigation = nullptr;
            title_bar->title_bar = nullptr;
            title_bar->source = nullptr;
        }
        if (title_bar->dispatcher)
            title_bar->dispatcher.ShutdownQueue();
        title_bar->app_window = nullptr;
        title_bar->application = nullptr;
    } catch (...) {
    }
    delete title_bar;
}

extern "C" void vm_native_titlebar_close_island(VmNativeTitleBar *title_bar)
{
    if (!title_bar)
        return;
    try {
        /* The owner HWND is being destroyed immediately after this detach, so
           do not transition AppWindow back to a system caption first. That
           transition installs another non-client update while the island is
           closing and its callback later races DestroyWindow. */
        if (title_bar->source) {
            title_bar->source.Content(nullptr);
            title_bar->source.Close();
        }
        title_bar->menu_bar = nullptr;
        title_bar->navigation = nullptr;
        title_bar->title_bar = nullptr;
        title_bar->source = nullptr;
        /* AppWindow installs native title-bar handling on the owner HWND.
           Retain that projection until after DestroyWindow has completed;
           releasing it here leaves the native teardown callback without its
           WinRT owner and crashes inside the generated ABI thunk. */
    } catch (...) {
    }
}

extern "C" void vm_native_titlebar_resize(VmNativeTitleBar *title_bar)
{
    RECT client{};
    if (!title_bar || !title_bar->source ||
        !GetClientRect(title_bar->hwnd, &client))
        return;
    auto dpi = GetDpiForWindow(title_bar->hwnd);
    title_bar->height_px =
        static_cast<UINT>((std::max)(1, MulDiv(title_bar_height_dip, dpi, 96)));
    title_bar->source.SiteBridge().MoveAndResize(
        {0, 0, client.right, static_cast<int>(title_bar->height_px)});
    update_drag_region(title_bar);
}

extern "C" void vm_native_titlebar_refresh(VmNativeTitleBar *title_bar)
{
    if (!title_bar)
        return;
    try {
        apply_colors(title_bar);
        vm_native_titlebar_resize(title_bar);
    } catch (...) {
    }
}

extern "C" BOOL vm_native_titlebar_pretranslate_message(
    VmNativeTitleBar *title_bar,
    MSG *message)
{
    if (!title_bar || !message)
        return FALSE;
    try {
        return ContentPreTranslateMessage(message) ? TRUE : FALSE;
    } catch (...) {
        return FALSE;
    }
}

extern "C" UINT vm_native_titlebar_height(const VmNativeTitleBar *title_bar)
{
    return title_bar ? title_bar->height_px : 0;
}
