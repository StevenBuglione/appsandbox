from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]


class VmTransportIdentityContractTests(unittest.TestCase):
    def test_windows_api_exposes_only_the_running_vm_runtime_identity(self) -> None:
        source = (ROOT / "src" / "app_win" / "headless.c").read_text(
            encoding="utf-8"
        )

        self.assertIn('wcscmp(sub, L"transport") == 0', source)
        self.assertIn('"transport_not_ready"', source)
        self.assertIn('"vm_not_running"', source)
        self.assertIn("hcs_find_runtime_id(v->name, &runtime_id)", source)
        self.assertRegex(
            source,
            re.compile(
                r'\\"transport\\":\\"hyperv-socket\\",\\"vmRuntimeId\\":\\"',
                re.MULTILINE,
            ),
        )

    def test_transport_identity_is_not_added_to_the_general_vm_status(self) -> None:
        source = (ROOT / "src" / "app_win" / "headless.c").read_text(
            encoding="utf-8"
        )
        status_start = source.index("static int append_vm_json")
        status_end = source.index("static BOOL runtime_id_is_zero", status_start)

        self.assertNotIn("runtimeId", source[status_start:status_end])
        self.assertNotIn("vmRuntimeId", source[status_start:status_end])

    def test_python_client_keeps_transport_lookup_explicit(self) -> None:
        source = (ROOT / "tools" / "headless-api" / "asb.py").read_text(
            encoding="utf-8"
        )

        self.assertIn('def transport(self, name):', source)
        self.assertIn('"/vms/%s/transport" % name', source)


if __name__ == "__main__":
    unittest.main()
