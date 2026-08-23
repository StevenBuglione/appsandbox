#include "pch.h"
#include "App.xaml.h"

#include <dwmapi.h>
#include <memory>

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
    constexpr wchar_t WindowClassName[] = L"LinguumWinUITitleBarIslandPreview";
    constexpr int TitleBarHeightDip = 36;

    Color color(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha = 0xFF)
    {
        return Color{ alpha, red, green, blue };
    }

    struct WindowState
    {
        DesktopWindowXamlSource source{ nullptr };
        AppWindow appWindow{ nullptr };
        bool sidebarVisible{ true };
        int titleBarHeightPx{ TitleBarHeightDip };
    };

    SolidColorBrush brush(Color value)
    {
        return SolidColorBrush{ value };
    }

    Button iconButton(wchar_t const* glyph, wchar_t const* accessibleName, bool enabled)
    {
        Button button;
        button.Width(32);
        button.Height(TitleBarHeightDip);
        button.Padding(Thickness{});
        button.Margin(Thickness{});
        button.BorderThickness(Thickness{});
        button.CornerRadius(CornerRadius{});
        button.Background(brush(color(16, 18, 23)));
        button.Foreground(brush(color(154, 154, 154)));
        button.IsEnabled(enabled);

        FontIcon icon;
        icon.FontFamily(FontFamily{ L"Segoe Fluent Icons" });
        icon.FontSize(14);
        icon.Glyph(glyph);
        button.Content(icon);
        Automation::AutomationProperties::SetName(button, accessibleName);
        return button;
    }

    MenuBarItem menu(wchar_t const* title)
    {
        MenuBarItem item;
        item.Title(title);
        item.Foreground(brush(color(154, 154, 154)));
        return item;
    }

    MenuFlyoutItem menuItem(wchar_t const* text, bool enabled = true)
    {
        MenuFlyoutItem item;
        item.Text(text);
        item.IsEnabled(enabled);
        return item;
    }

    void updateDragRegion(HWND hwnd, WindowState& state)
    {
        RECT client{};
        if (!GetClientRect(hwnd, &client) || !state.appWindow)
        {
            return;
        }

        auto titleBar = state.appWindow.TitleBar();
        auto right = client.right - static_cast<int>(titleBar.RightInset()) - 1;
        if (right < 316)
        {
            right = 316;
        }
        if (right <= 316)
        {
            titleBar.SetDragRectangles({});
            return;
        }
        titleBar.SetDragRectangles(
            { RectInt32{ 316, 0, right - 316, state.titleBarHeightPx } });
    }

    void resizeIsland(HWND hwnd, WindowState& state)
    {
        RECT client{};
        if (!GetClientRect(hwnd, &client) || !state.source)
        {
            return;
        }

        auto dpi = GetDpiForWindow(hwnd);
        state.titleBarHeightPx = MulDiv(TitleBarHeightDip, dpi, 96);
        state.source.SiteBridge().MoveAndResize(
            { 0, 0, client.right, state.titleBarHeightPx });
        updateDragRegion(hwnd, state);
    }

    TitleBar createTitleBar(HWND hwnd, WindowState& state)
    {
        TitleBar titleBar;
        titleBar.Height(TitleBarHeightDip);
        titleBar.Background(brush(color(16, 18, 23)));
        StackPanel navigation;
        navigation.Orientation(Orientation::Horizontal);
        navigation.Spacing(0);

        auto sidebar = iconButton(L"\xE8A0", L"Toggle sidebar", true);
        Automation::AutomationProperties::SetAutomationId(
            sidebar, L"SidebarToggleButton");
        sidebar.Click([hwnd, &state](Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
        {
            state.sidebarVisible = !state.sidebarVisible;
            InvalidateRect(hwnd, nullptr, FALSE);
        });
        navigation.Children().Append(sidebar);
        navigation.Children().Append(iconButton(L"\xE72B", L"Back", false));
        navigation.Children().Append(iconButton(L"\xE72A", L"Forward", false));
        MenuBar menuBar;
        menuBar.Height(TitleBarHeightDip);
        menuBar.Margin(Thickness{ -10, 0, 0, 0 });
        menuBar.Padding(Thickness{});
        menuBar.Background(brush(color(16, 18, 23)));

        auto file = menu(L"File");
        file.Items().Append(menuItem(L"New Window", false));
        auto exit = menuItem(L"Exit");
        exit.Click([hwnd](Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
        {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        });
        file.Items().Append(exit);

        auto edit = menu(L"Edit");
        edit.Items().Append(menuItem(L"Undo", false));
        edit.Items().Append(menuItem(L"Redo", false));

        auto view = menu(L"View");
        auto sidebarItem = ToggleMenuFlyoutItem{};
        sidebarItem.Text(L"Sidebar");
        sidebarItem.IsChecked(true);
        sidebarItem.Click([hwnd, &state, sidebarItem](Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
        {
            state.sidebarVisible = !state.sidebarVisible;
            sidebarItem.IsChecked(state.sidebarVisible);
            InvalidateRect(hwnd, nullptr, FALSE);
        });
        view.Items().Append(sidebarItem);

        auto help = menu(L"Help");
        help.Items().Append(menuItem(L"About Linguum"));

        menuBar.Items().Append(file);
        menuBar.Items().Append(edit);
        menuBar.Items().Append(view);
        menuBar.Items().Append(help);
        navigation.Children().Append(menuBar);
        titleBar.LeftHeader(navigation);
        return titleBar;
    }

    void paintPreview(HWND hwnd, WindowState const& state)
    {
        PAINTSTRUCT paint{};
        auto dc = BeginPaint(hwnd, &paint);
        RECT client{};
        GetClientRect(hwnd, &client);

        HBRUSH background = CreateSolidBrush(RGB(16, 18, 23));
        FillRect(dc, &client, background);
        DeleteObject(background);

        if (state.sidebarVisible)
        {
            RECT sidebar{ 0, state.titleBarHeightPx, 236, client.bottom };
            HBRUSH sidebarBrush = CreateSolidBrush(RGB(24, 26, 32));
            FillRect(dc, &sidebar, sidebarBrush);
            DeleteObject(sidebarBrush);
        }

        EndPaint(hwnd, &paint);
    }

    LRESULT CALLBACK windowProcedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        auto state = reinterpret_cast<WindowState*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        switch (message)
        {
        case WM_CREATE:
        {
            auto owned = std::make_unique<WindowState>();
            state = owned.get();
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

            auto windowId = GetWindowIdFromWindow(hwnd);
            state->appWindow = AppWindow::GetFromWindowId(windowId);
            auto nativeTitleBar = state->appWindow.TitleBar();
            nativeTitleBar.ExtendsContentIntoTitleBar(true);
            nativeTitleBar.PreferredHeightOption(TitleBarHeightOption::Standard);
            nativeTitleBar.ButtonBackgroundColor(color(16, 18, 23));
            nativeTitleBar.ButtonInactiveBackgroundColor(color(16, 18, 23));
            nativeTitleBar.ButtonForegroundColor(color(154, 154, 154));
            nativeTitleBar.ButtonInactiveForegroundColor(color(112, 112, 112));

            state->source = DesktopWindowXamlSource{};
            state->source.Initialize(windowId);
            state->source.Content(createTitleBar(hwnd, *state));
            resizeIsland(hwnd, *state);
            owned.release();
            return 0;
        }
        case WM_SIZE:
            if (state)
            {
                resizeIsland(hwnd, *state);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_DPICHANGED:
            if (state)
            {
                auto suggested = reinterpret_cast<RECT const*>(lparam);
                SetWindowPos(
                    hwnd,
                    nullptr,
                    suggested->left,
                    suggested->top,
                    suggested->right - suggested->left,
                    suggested->bottom - suggested->top,
                    SWP_NOZORDER | SWP_NOACTIVATE);
                resizeIsland(hwnd, *state);
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            if (state)
            {
                paintPreview(hwnd, *state);
                return 0;
            }
            break;
        case WM_NCDESTROY:
            if (state)
            {
                state->source.Close();
                delete state;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            }
            PostQuitMessage(0);
            return 0;
        }

        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    try
    {
        init_apartment(apartment_type::single_threaded);
        auto dispatcher = DispatcherQueueController::CreateOnCurrentThread();
        auto app = make<LinguumNativeTitleBarIsland::implementation::App>();

        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = windowProcedure;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = WindowClassName;
        check_bool(RegisterClassExW(&windowClass) != 0);

        auto hwnd = CreateWindowExW(
            WS_EX_APPWINDOW,
            WindowClassName,
            L"Linguum",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            1100,
            720,
            nullptr,
            nullptr,
            instance,
            nullptr);
        check_bool(hwnd != nullptr);
        COLORREF borderColor = RGB(16, 18, 23);
        check_hresult(DwmSetWindowAttribute(
            hwnd, DWMWA_BORDER_COLOR, &borderColor, sizeof(borderColor)));
        ShowWindow(hwnd, showCommand);
        UpdateWindow(hwnd);

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            if (ContentPreTranslateMessage(&message))
            {
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        dispatcher.ShutdownQueue();
        return static_cast<int>(message.wParam);
    }
    catch (hresult_error const& error)
    {
        return error.code().value;
    }
}
