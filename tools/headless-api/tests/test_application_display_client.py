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
        size_handler = source[source.index("case WM_SIZE:") : source.index("case WM_ENTERSIZEMOVE:")]
        self.assertNotIn("d3d_resize_swap_chain(d)", size_handler)


if __name__ == "__main__":
    unittest.main()
