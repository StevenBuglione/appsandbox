#include <windows.h>

#include "vm_startup_scene.h"
#include "vm_window_chrome.h"

#define PREVIEW_CLASS L"LinguumNativeStartupPreview"
#define PREVIEW_TIMER 1

typedef struct PreviewState {
    VmStartupScene *scene;
    HICON window_icon;
    AsbStartupPhase phase;
    BOOL detailed;
    AsbWindowChromeOptions chrome;
} PreviewState;

static AsbStartupPhase parse_phase(const wchar_t *command_line)
{
    if (wcsstr(command_line, L"--phase failed"))
        return ASB_STARTUP_FAILED;
    if (wcsstr(command_line, L"--phase finishing"))
        return ASB_STARTUP_FINISHING;
    if (wcsstr(command_line, L"--phase opening"))
        return ASB_STARTUP_OPENING;
    return ASB_STARTUP_PREPARING;
}

static void paint_scene(HWND hwnd, PreviewState *state)
{
    PAINTSTRUCT paint;
    RECT client;
    VmStartupSceneFrame frame;
    BITMAPINFO bitmap;
    HDC dc = BeginPaint(hwnd, &paint);

    GetClientRect(hwnd, &client);
    ZeroMemory(&frame, sizeof(frame));
    if (state && state->scene && client.right > 0 && client.bottom > 0 &&
        vm_startup_scene_render(
            state->scene, (UINT)client.right, (UINT)client.bottom,
            GetDpiForWindow(hwnd), state->phase, state->detailed,
            GetTickCount64(), &frame)) {
        ZeroMemory(&bitmap, sizeof(bitmap));
        bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap.bmiHeader.biWidth = (LONG)frame.width;
        bitmap.bmiHeader.biHeight = -(LONG)frame.height;
        bitmap.bmiHeader.biPlanes = 1;
        bitmap.bmiHeader.biBitCount = 32;
        bitmap.bmiHeader.biCompression = BI_RGB;
        SetDIBitsToDevice(
            dc, 0, 0, frame.width, frame.height, 0, 0, 0, frame.height,
            frame.pixels, &bitmap, DIB_RGB_COLORS);
    } else {
        FillRect(dc, &client, (HBRUSH)GetStockObject(BLACK_BRUSH));
    }
    EndPaint(hwnd, &paint);
}

static LRESULT CALLBACK preview_window_proc(HWND hwnd, UINT message,
                                            WPARAM wparam, LPARAM lparam)
{
    PreviewState *state =
        (PreviewState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (message == WM_NCCREATE) {
        CREATESTRUCTW *create = (CREATESTRUCTW *)lparam;
        state = (PreviewState *)create->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)state);
    }

    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint_scene(hwnd, state);
        return 0;
    case WM_SIZE:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_TIMER:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_KEYDOWN:
        if (!state) break;
        if (wparam == VK_F12) {
            state->detailed = !state->detailed;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (wparam >= L'1' && wparam <= L'5') {
            state->phase = (AsbStartupPhase)(wparam - L'0');
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (wparam == VK_ESCAPE) {
            DestroyWindow(hwnd);
            return 0;
        }
        if (wparam == L'S' && vm_startup_scene_toggle_sidebar(state->scene)) {
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        break;
    case WM_DPICHANGED:
    {
        const RECT *suggested = (const RECT *)lparam;
        SetWindowPos(hwnd, NULL, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        if (state)
            vm_window_chrome_apply(hwnd, &state->chrome,
                                   GetActiveWindow() == hwnd);
        return 0;
    }
    case WM_THEMECHANGED:
    case WM_DWMCOLORIZATIONCOLORCHANGED:
        if (state)
            vm_window_chrome_apply(hwnd, &state->chrome,
                                   GetActiveWindow() == hwnd);
        return 0;
    case WM_SETTINGCHANGE:
        if (state) {
            vm_startup_scene_refresh_system_settings(state->scene);
            if (vm_startup_scene_is_animated(state->scene))
                SetTimer(hwnd, PREVIEW_TIMER, 120, NULL);
            else
                KillTimer(hwnd, PREVIEW_TIMER);
            vm_window_chrome_apply(hwnd, &state->chrome,
                                   GetActiveWindow() == hwnd);
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_ACTIVATE:
        if (state)
            vm_window_chrome_apply(hwnd, &state->chrome,
                                   LOWORD(wparam) != WA_INACTIVE);
        break;
    case WM_DESTROY:
        KillTimer(hwnd, PREVIEW_TIMER);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous_instance,
                    PWSTR command_line, int show_command)
{
    WNDCLASSEXW window_class;
    AsbStartupOptions startup;
    PreviewState state;
    DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    DWORD extended_style = WS_EX_APPWINDOW;
    RECT bounds = { 0, 0, 1000, 640 };
    HWND window;
    MSG message;

    (void)previous_instance;
    (void)show_command;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    ZeroMemory(&state, sizeof(state));
    ZeroMemory(&startup, sizeof(startup));
    startup.enabled = TRUE;
    startup.position = wcsstr(command_line, L"--center")
        ? ASB_STARTUP_CENTER : ASB_STARTUP_BOTTOM_LEFT;
    startup.motion = wcsstr(command_line, L"--no-motion")
        ? ASB_STARTUP_MOTION_NONE : ASB_STARTUP_MOTION_ORBIT;
    startup.shell = wcsstr(command_line, L"--canvas")
        ? ASB_STARTUP_SHELL_CANVAS : ASB_STARTUP_SHELL_WORKSPACE;
    startup.sidebar = wcsstr(command_line, L"--collapsed")
        ? ASB_STARTUP_SIDEBAR_COLLAPSED : ASB_STARTUP_SIDEBAR_EXPANDED;
    startup.background_color = RGB(17, 19, 24);
    startup.foreground_color = RGB(246, 244, 251);
    startup.accent_color = RGB(201, 182, 255);
    startup.delayed_message_after_ms = 3000;
    startup.app_name = L"Linguum";
    startup.opening_message = L"Linguum is waking up…";
    startup.preparing_message = L"Getting things ready…";
    startup.finishing_message = L"Almost there…";
    startup.delayed_message = L"First launch can take a little longer.";
    startup.failed_message = L"Something got in the way.";
    startup.failure_detail = L"The browser service did not become ready";
    startup.mark_path =
        L"tools\\native-startup-preview\\assets\\linguum-orbit-mark.ico";
    state.scene = vm_startup_scene_create(&startup);
    if (!state.scene) return 2;
    state.phase = parse_phase(command_line);
    state.detailed = wcsstr(command_line, L"--details") != NULL;
    state.chrome.theme = ASB_TITLE_BAR_SYSTEM;
    state.chrome.corner_preference = ASB_WINDOW_CORNER_ROUNDED;
    state.chrome.has_caption_color = TRUE;
    state.chrome.has_text_color = TRUE;
    state.chrome.has_border_color = TRUE;
    state.chrome.caption_color = startup.background_color;
    state.chrome.text_color = startup.foreground_color;
    state.chrome.border_color = startup.accent_color;

    ZeroMemory(&window_class, sizeof(window_class));
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = preview_window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(NULL, IDC_ARROW);
    window_class.lpszClassName = PREVIEW_CLASS;
    if (!RegisterClassExW(&window_class)) {
        vm_startup_scene_destroy(state.scene);
        return 3;
    }

    AdjustWindowRectExForDpi(
        &bounds, style, FALSE, extended_style, GetDpiForSystem());
    window = CreateWindowExW(
        extended_style, PREVIEW_CLASS, L"Linguum — Native startup preview",
        style, 160, 100, bounds.right - bounds.left, bounds.bottom - bounds.top,
        NULL, NULL, instance, &state);
    if (!window) {
        vm_startup_scene_destroy(state.scene);
        return 4;
    }
    state.window_icon = (HICON)LoadImageW(
        NULL, startup.mark_path, IMAGE_ICON, 256, 256, LR_LOADFROMFILE);
    if (state.window_icon) {
        SendMessageW(window, WM_SETICON, ICON_BIG,
                     (LPARAM)state.window_icon);
        SendMessageW(window, WM_SETICON, ICON_SMALL,
                     (LPARAM)state.window_icon);
    }
    vm_window_chrome_apply(window, &state.chrome, TRUE);
    if (vm_startup_scene_is_animated(state.scene))
        SetTimer(window, PREVIEW_TIMER, 120, NULL);
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    vm_startup_scene_destroy(state.scene);
    if (state.window_icon) DestroyIcon(state.window_icon);
    return (int)message.wParam;
}
