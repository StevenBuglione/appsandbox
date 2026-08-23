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
            "showDebugTitle": False,
            "showDebugOverlay": False,
        }
        with patch.object(client, "_req", return_value=(200, {})) as request:
            client.open_display("vm", **options)
        request.assert_called_once_with("POST", "/vms/vm/display", options)

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
        self.assertIn("idd_request_render(d, FALSE)", size_handler)

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
