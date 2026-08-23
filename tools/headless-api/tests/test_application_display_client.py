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

    def test_native_video_present_never_blocks_the_window_thread(self):
        source = (
            Path(__file__).resolve().parents[3]
            / "src"
            / "backend_win"
            / "vm_display_idd.c"
        ).read_text(encoding="utf-8")
        self.assertIn("DXGI_PRESENT_DO_NOT_WAIT", source)
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
        self.assertIn("idd_request_render(d, FALSE)", size_handler)

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


if __name__ == "__main__":
    unittest.main()
