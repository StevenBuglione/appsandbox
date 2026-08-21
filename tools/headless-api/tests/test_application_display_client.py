import os
import sys
import unittest
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


if __name__ == "__main__":
    unittest.main()

