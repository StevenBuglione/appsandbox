import os
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import asb


class ApplicationDisplayClientTests(unittest.TestCase):
    def test_default_display_request_remains_bodyless(self):
        client = asb.Client("http://127.0.0.1:1", "token")
        with patch.object(client, "_req", return_value=(200, {})) as request:
            client.open_display("vm")
        request.assert_called_once_with("POST", "/vms/vm/display", None)

    def test_application_options_are_forwarded_without_rewriting(self):
        client = asb.Client("http://127.0.0.1:1", "token")
        options = {
            "mode": "application",
            "title": "Linguum Runtime POC",
            "width": 1440,
            "height": 900,
            "backingWidth": 1900,
            "backingHeight": 1180,
            "showDebugTitle": False,
            "showDebugOverlay": False,
            "showOnOpen": False,
        }
        with patch.object(client, "_req", return_value=(200, {})) as request:
            client.open_display("vm", **options)
        request.assert_called_once_with("POST", "/vms/vm/display", options)

    def test_customizable_thinking_startup_is_forwarded_without_vm_terms(self):
        client = asb.Client("http://127.0.0.1:1", "token")
        options = {
            "mode": "application",
            "title": "My App",
            "startupPreset": "thinking",
            "startupReadyMode": "manual",
            "startupPosition": "bottom-left",
            "startupMotion": "orbit",
            "startupShell": "workspace",
            "startupSidebar": "expanded",
            "startupBackgroundColor": "#111318",
            "startupForegroundColor": "#f6f4fb",
            "startupAccentColor": "#c9b6ff",
            "startupOpeningMessage": "My App is waking up…",
            "titleBarTheme": "system",
            "titleBarCorner": "rounded",
        }
        with patch.object(client, "_req", return_value=(200, {})) as request:
            client.open_display("vm", **options)
        request.assert_called_once_with("POST", "/vms/vm/display", options)

    def test_startup_state_is_scoped_to_one_owned_display(self):
        client = asb.Client("http://127.0.0.1:1", "token")
        with patch.object(client, "_req", return_value=(202, {})) as request:
            client.set_display_startup_state("vm", "finishing")
            client.set_display_startup_state("vm", "ready", detailed=True)
        self.assertEqual(
            request.call_args_list,
            [
                unittest.mock.call(
                    "PUT", "/vms/vm/display", {"startupPhase": "finishing"}
                ),
                unittest.mock.call(
                    "PUT", "/vms/vm/display",
                    {"startupPhase": "ready", "startupDetailed": True},
                ),
            ],
        )

    def test_resize_is_scoped_to_one_open_display(self):
        client = asb.Client("http://127.0.0.1:1", "token")
        with patch.object(client, "_req", return_value=(202, {})) as request:
            client.resize_display("vm", 1320, 800)
        request.assert_called_once_with(
            "PUT", "/vms/vm/display", {"width": 1320, "height": 800}
        )

    def test_resize_phase_is_scoped_and_not_an_arbitrary_window_message(self):
        client = asb.Client("http://127.0.0.1:1", "token")
        with patch.object(client, "_req", return_value=(202, {})) as request:
            client.begin_display_resize("vm")
            client.end_display_resize("vm")
        self.assertEqual(
            request.call_args_list,
            [
                unittest.mock.call(
                    "PUT", "/vms/vm/display", {"phase": "begin"}
                ),
                unittest.mock.call("PUT", "/vms/vm/display", {"phase": "end"}),
            ],
        )

    def test_pointer_gestures_are_scoped_to_one_open_display(self):
        client = asb.Client("http://127.0.0.1:1", "token")
        with patch.object(client, "_req", return_value=(202, {})) as request:
            client.click_display("vm", 840, 96)
            client.drag_display("vm", 620, 480, 760, 480, 32)
        self.assertEqual(
            request.call_args_list,
            [
                unittest.mock.call(
                    "PUT", "/vms/vm/display",
                    {"input": "click", "x": 840, "y": 96},
                ),
                unittest.mock.call(
                    "PUT", "/vms/vm/display",
                    {"input": "drag", "x": 620, "y": 480,
                     "endX": 760, "endY": 480, "steps": 32},
                ),
            ],
        )

        header = (
            Path(__file__).resolve().parents[3]
            / "src"
            / "backend_win"
            / "vm_display_idd.h"
        ).read_text(encoding="utf-8")
        self.assertIn("vm_display_idd_pointer_click", header)
        self.assertIn("vm_display_idd_pointer_drag", header)

    def test_native_video_present_never_blocks_the_window_thread(self):
        source = (
            Path(__file__).resolve().parents[3]
            / "src"
            / "backend_win"
            / "vm_display_idd.c"
        ).read_text(encoding="utf-8")
        self.assertIn("DXGI_PRESENT_DO_NOT_WAIT", source)
        self.assertIn("DXGI_SWAP_EFFECT_FLIP_DISCARD", source)
        self.assertIn("DXGI_SCALING_STRETCH", source)
        self.assertIn("scd.BufferCount      = 2", source)
        self.assertIn("frame_message_pending", source)
        self.assertIn(
            "InterlockedCompareExchange(&d->frame_message_pending, 1, 0)", source
        )
        self.assertIn("idd_render_thread_proc", source)
        self.assertIn("WM_IDD_RESIZE_PHASE", source)
        self.assertIn("idd_begin_interactive_resize(d, hwnd)", source)
        self.assertIn("idd_end_interactive_resize(d, hwnd)", source)
        self.assertIn("d->app_mode && d->fixed_backing", source)
        self.assertIn("if (d->show_on_open)", source)
        self.assertIn("vp_w = (float)d->frame_width", source)
        self.assertIn("width = d->fixed_backing ? d->backing_width", source)
        self.assertIn("CreateEventW(NULL, FALSE, FALSE, NULL)", source)
        self.assertNotIn("IDT_PRESENT", source)

        render_child = source[
            source.index("static LRESULT CALLBACK idd_render_proc") :
            source.index("static LRESULT CALLBACK idd_ll_keyboard_proc")
        ]
        self.assertIn("PostMessageW(parent, WM_IDD_FRAME_READY", render_child)
        self.assertNotIn("SendMessageW(GetParent(hwnd), WM_IDD_FRAME_READY", render_child)

        window_handler = source[
            source.rindex("static LRESULT CALLBACK idd_wnd_proc(HWND hwnd") :
            source.index(" * Public API")
        ]
        for blocking_call in (
            "d3d_init(d)",
            "d3d_render_frame(d)",
            "d3d_resize_swap_chain(d",
            "d3d_cleanup(d)",
        ):
            self.assertNotIn(blocking_call, window_handler)

        size_handler = source[
            source.index("case WM_SIZE:") : source.index("case WM_ENTERSIZEMOVE:")
        ]
        self.assertIn("SWP_NOREDRAW", size_handler)
        self.assertIn("d->render_hwnd != hwnd", size_handler)
        self.assertIn("idd_request_render(d, TRUE)", size_handler)
        self.assertIn("idd_queue_desired_resize(d)", size_handler)
        self.assertNotIn(
            "guest modesetting remain deferred", size_handler
        )

        creation = source[
            source.index("A product application window renders") :
            source.index("The application gate has no App Sandbox debug-log window")
        ]
        self.assertIn("d->render_hwnd = d->hwnd", creation)

        destroy_handler = source[
            source.index("case WM_DESTROY:") : source.index("case WM_GETMINMAXINFO:")
        ]
        self.assertIn("if (d->render_hwnd == hwnd)", destroy_handler)
        self.assertIn("d->render_hwnd = NULL", destroy_handler)

        classes = source[
            source.index("static void ensure_idd_class") :
            source.index("static void idd_update_window_title")
        ]
        self.assertIn("wc.hbrBackground = NULL", classes)
        self.assertNotIn("CS_HREDRAW | CS_VREDRAW", classes)

        top_level_proc = source[source.index("static LRESULT CALLBACK idd_wnd_proc") :]
        self.assertIn("case WM_ERASEBKGND:", top_level_proc)
        self.assertIn("We handle all painting via D3D11", top_level_proc)

        focus_api = source[source.index("BOOL vm_display_idd_focus") :]
        self.assertIn("SendMessageTimeoutW", focus_api)
        self.assertNotIn("PostMessageW(display->hwnd, WM_IDD_FOCUS", focus_api)

        pointer_api = source[
            source.index("BOOL vm_display_idd_pointer_click") :
            source.index("Per-VM display settings")
        ]
        self.assertIn("Sleep(POINTER_SETTLE_MS)", pointer_api)
        self.assertIn("Sleep(POINTER_PRESS_MS)", pointer_api)

        exit_size_start = source.index("case WM_EXITSIZEMOVE:")
        exit_size_handler = source[
            exit_size_start : source.index("case WM_PAINT:", exit_size_start)
        ]
        self.assertIn("idd_end_interactive_resize(d, hwnd)", exit_size_handler)
        resize_phase_helpers = source[
            source.index("static void idd_begin_interactive_resize") :
            source.index("static void idd_note_applied_frame")
        ]
        self.assertIn("idd_request_render(d, TRUE)", resize_phase_helpers)
        self.assertIn("idd_queue_desired_resize(d)", resize_phase_helpers)
        self.assertIn(
            "d->app_mode && d->fixed_backing",
            source[source.index("static void window_to_vm_coords") :],
        )

        headless = (
            Path(__file__).resolve().parents[3]
            / "src"
            / "app_win"
            / "headless.c"
        ).read_text(encoding="utf-8")
        self.assertIn('json_get_int(body, L"backingWidth"', headless)
        self.assertIn('json_get_int(body, L"backingHeight"', headless)
        self.assertIn('json_get_bool(body, L"showOnOpen"', headless)
        self.assertIn(
            "display_options.backing_width < display_options.initial_width",
            headless,
        )

        render_worker_start = source.index(
            "static DWORD WINAPI idd_render_thread_proc(LPVOID param)\n{"
        )
        render_worker = source[
            render_worker_start : source.index("Guest cursor", render_worker_start)
        ]
        self.assertIn("d3d_init(d)", render_worker)
        self.assertIn("d3d_resize_swap_chain(d, desired_width, desired_height)", render_worker)
        self.assertIn("d3d_render_frame(d)", render_worker)
        self.assertIn("d3d_cleanup(d)", render_worker)

    def test_startup_scene_uses_the_existing_native_swap_chain(self):
        root = Path(__file__).resolve().parents[3]
        source = (root / "src" / "backend_win" / "vm_display_idd.c").read_text(
            encoding="utf-8"
        )
        scene = (root / "src" / "backend_win" / "vm_startup_scene.c").read_text(
            encoding="utf-8"
        )
        chrome = (root / "src" / "backend_win" / "vm_window_chrome.c").read_text(
            encoding="utf-8"
        )
        api = (root / "src" / "app_win" / "headless.c").read_text(
            encoding="utf-8"
        )

        self.assertIn("startup_srv", source)
        self.assertIn("d3d_update_startup_texture", source)
        self.assertIn("display_srv = startup_active ? d->startup_srv : d->frame_srv", source)
        self.assertIn("if (!d->startup_enabled)", source)
        self.assertIn("vm_startup_scene_is_animated", source)
        self.assertIn("startupVisible", api)
        self.assertIn("startupPhase", api)
        self.assertIn("SetProgressState", source)
        self.assertIn("TBPF_INDETERMINATE", source)
        self.assertIn("startup presentation is available only in application mode", api)
        self.assertIn("SPI_GETCLIENTAREAANIMATION", scene)
        self.assertIn("vm_startup_scene_refresh_system_settings", source)
        self.assertIn("startup_settings_changed", source)
        self.assertIn("First launch can take a little longer", scene)
        self.assertNotIn("CreateWindowEx", scene)

        self.assertIn("DWMWA_CAPTION_COLOR", chrome)
        self.assertIn("DWMWA_TEXT_COLOR", chrome)
        self.assertIn("DWMWA_BORDER_COLOR", chrome)
        self.assertIn("SPI_GETHIGHCONTRAST", chrome)
        self.assertNotIn("WM_NCCALCSIZE", source)
        self.assertIn("WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN", source)

        project = (root / "AppSandbox.vcxproj").read_text(encoding="utf-8")
        preview_project = (
            root / "tools" / "native-startup-preview" /
            "NativeStartupPreview.vcxproj"
        ).read_text(encoding="utf-8")
        self.assertIn("/utf-8", project)
        self.assertIn("/utf-8", preview_project)
        self.assertIn("TreatWarningAsError", preview_project)

    def test_winui_title_bar_is_hosted_in_the_native_window(self):
        root = Path(__file__).resolve().parents[3]
        source = (
            root / "tools" / "winui-titlebar-island-preview" / "main.cpp"
        ).read_text(encoding="utf-8")
        project = (
            root / "tools" / "winui-titlebar-island-preview" /
            "NativeTitleBarIslandPreview.vcxproj"
        ).read_text(encoding="utf-8")

        self.assertIn("DesktopWindowXamlSource", source)
        self.assertIn("TitleBar createTitleBar", source)
        self.assertIn("ContentPreTranslateMessage", source)
        self.assertIn('L"SidebarToggleButton"', source)
        self.assertIn('menuBar.Items().Append(file)', source)
        self.assertIn('menuBar.Items().Append(help)', source)
        self.assertIn("navigation.Children().Append(menuBar)", source)
        self.assertIn("titleBar.LeftHeader(navigation)", source)
        self.assertNotIn("titleBar.Content(menuBar)", source)
        self.assertGreaterEqual(source.count("color(16, 18, 23)"), 5)
        self.assertIn("WindowsAppSDKSelfContained>false", project)
        self.assertIn("Microsoft.WindowsAppSDK.WinUI", project)

    def test_compact_title_bar_is_wired_into_the_production_display(self):
        root = Path(__file__).resolve().parents[3]
        source = (root / "src" / "backend_win" / "vm_display_idd.c").read_text(
            encoding="utf-8"
        )
        title_bar = (
            root / "src" / "backend_win" / "vm_native_titlebar.cpp"
        ).read_text(encoding="utf-8")
        api = (root / "src" / "app_win" / "headless.c").read_text(
            encoding="utf-8"
        )
        project = (root / "AppSandbox.vcxproj").read_text(encoding="utf-8")

        self.assertIn("DesktopWindowXamlSource", title_bar)
        self.assertIn("ExtendsContentIntoTitleBar(true)", title_bar)
        self.assertIn("TitleBar create_title_bar", title_bar)
        self.assertIn("NativeTitleBarApp", title_bar)
        self.assertIn("native_app_runtime_consumed.exchange(true)", title_bar)
        self.assertIn("ContentPreTranslateMessage", title_bar)
        self.assertIn("SetDragRectangles", title_bar)
        self.assertIn('L"SidebarToggleButton"', title_bar)
        self.assertIn("PostMessageW(state->hwnd, WM_CLOSE", title_bar)

        self.assertIn("vm_native_titlebar_create(", source)
        self.assertIn("vm_native_titlebar_pretranslate_message(", source)
        self.assertIn("vm_native_titlebar_resize(", source)
        self.assertIn("vm_native_titlebar_close_island(", source)
        self.assertIn("idd_get_content_size", source)
        self.assertIn("idd_resize_window_for_content", source)
        self.assertIn("d->render_height - d->title_bar_height", source)
        self.assertIn("wy -= (int)top_inset", source)
        self.assertIn("RGB(16, 18, 23)", source)
        self.assertIn("title_bar_hosted", source)

        self.assertIn('json_get_string(body, L"titleBarLayout"', api)
        self.assertIn('wchar_t title_bar_theme[16] = L"system"', api)
        self.assertIn('wchar_t title_bar_layout[24] = L"caption-only"', api)
        self.assertIn(
            "display_options.window_chrome.theme = ASB_TITLE_BAR_DARK", api
        )
        self.assertIn('L"compact"', api)
        self.assertIn('L"caption-only"', api)
        self.assertIn('L"visible"', api)
        self.assertIn('L"hidden"', api)
        self.assertIn('L"desktop"', api)
        self.assertIn(r'\"titleBarHosted\":%s', api)
        self.assertIn(r'\"contentWidth\":%u', api)

        self.assertIn("vm_native_titlebar.cpp", project)
        self.assertIn("Microsoft.WindowsAppSDK.WinUI", project)
        self.assertIn("<CompileAs>CompileAsCpp</CompileAs>", project)
        self.assertIn("<WindowsPackageType>None</WindowsPackageType>", project)
        self.assertIn("NativeTitleBarApp.xaml", project)
        app_xaml = (
            root / "src" / "backend_win" / "NativeTitleBarApp.xaml"
        ).read_text(encoding="utf-8")
        self.assertIn("XamlControlsResources", app_xaml)

        # One production HWND owns the frame, swap chain, startup surface, and
        # XAML Island. The integration must not regress to custom non-client
        # emulation, a second top-level splash, or a timer-driven resize loop.
        self.assertIn("d->render_hwnd = d->hwnd", source)
        self.assertNotIn("WM_NCCALCSIZE", source)
        self.assertNotIn("SetTimer(hwnd, IDT_PRESENT", source)

        window_proc = source[source.rindex("static LRESULT CALLBACK idd_wnd_proc(") :]
        close_handler = window_proc[
            window_proc.index("case WM_CLOSE:") : window_proc.index("case WM_DESTROY:")
        ]
        self.assertNotIn("vm_native_titlebar_destroy", close_handler)
        self.assertNotIn("vm_native_titlebar_close_island", close_handler)
        self.assertIn("DestroyWindow(hwnd)", close_handler)
        self.assertNotIn("case WM_NCDESTROY:", source)

    def test_title_bar_does_not_replace_the_qualified_resize_state_machine(self):
        root = Path(__file__).resolve().parents[3]
        source = (root / "src" / "backend_win" / "vm_display_idd.c").read_text(
            encoding="utf-8"
        )

        self.assertIn("idd_render_thread_proc", source)
        self.assertIn("DXGI_PRESENT_DO_NOT_WAIT", source)
        self.assertIn("d->app_mode && d->fixed_backing", source)
        self.assertIn("idd_begin_interactive_resize(d, hwnd)", source)
        self.assertIn("idd_end_interactive_resize(d, hwnd)", source)
        self.assertIn("idd_queue_desired_resize(d)", source)
        self.assertIn("SWP_NOREDRAW", source)
        self.assertIn("if (d->show_on_open)", source)
        self.assertNotIn("IDT_PRESENT", source)

    def test_display_state_exposes_monotonic_native_presentation_progress(self):
        root = Path(__file__).resolve().parents[3]
        source = (root / "src" / "backend_win" / "vm_display_idd.c").read_text(
            encoding="utf-8"
        )
        header = (root / "src" / "backend_win" / "vm_display_idd.h").read_text(
            encoding="utf-8"
        )
        api = (root / "src" / "app_win" / "headless.c").read_text(encoding="utf-8")
        self.assertIn("AsbDisplayRuntimeState", header)
        self.assertIn("vm_display_idd_get_runtime_state", header)
        self.assertIn("InterlockedIncrement64(&d->received_frame_generation)", source)
        self.assertIn("InterlockedIncrement64(&d->present_count)", source)
        self.assertIn("presented_frame_generation", source)
        self.assertIn("presented_render_width", source)
        self.assertIn("presented_render_height", source)
        self.assertNotIn(
            "state->render_width = (UINT)InterlockedCompareExchange(\n"
            "            &display->desired_render_width",
            source,
        )
        self.assertIn("receivedFrames", api)
        self.assertIn("presentedFrames", api)
        self.assertIn("presentCount", api)


if __name__ == "__main__":
    unittest.main()
