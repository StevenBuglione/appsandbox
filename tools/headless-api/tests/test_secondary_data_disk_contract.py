"""Focused source/API contract for the optional caller-owned secondary VHDX."""

import importlib.util
import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]


def read(relative_path):
    return (ROOT / relative_path).read_text(encoding="utf-8")


class RecordingClient:
    def __init__(self, client_type):
        self.client = client_type("http://127.0.0.1:1", "test-token")
        self.request = None
        self.client._req = self.record

    def record(self, method, path, body=None, timeout=60):
        self.request = (method, path, body, timeout)
        return 202, {"ok": True}


class SecondaryDataDiskContractTest(unittest.TestCase):
    def test_python_client_forwards_exact_path_without_mutation(self):
        module_path = ROOT / "tools" / "headless-api" / "asb.py"
        spec = importlib.util.spec_from_file_location("asb_secondary_disk_test", module_path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        recording = RecordingClient(module.Client)
        path = r"D:\Linguum State\browser-state.vhdx"

        code, body = recording.client.create(
            name="state-test",
            osType="Linux",
            diskPath=r"D:\Runtime\scratch.vhdx",
            dataDiskPath=path,
            install=False,
            ramMb=4097,
        )

        self.assertEqual((code, body), (202, {"ok": True}))
        self.assertEqual(recording.request[0:2], ("POST", "/vms"))
        self.assertEqual(recording.request[2]["dataDiskPath"], path)
        self.assertEqual(recording.request[2]["ramMb"], 4096)

    def test_hcs_uses_one_fixed_virtual_disk_slot_and_fail_closed_grant(self):
        source = read("src/backend_win/hcs_vm.c")
        self.assertIn(
            'L",\\\"3\\\":{\\\"Type\\\":\\\"VirtualDisk\\\",\\\"Path\\\":\\\"%s\\\"}"',
            source,
        )
        self.assertEqual(source.count("prepare_secondary_data_disk(config)"), 2)
        self.assertIn("return HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION);", source)
        self.assertIn("return E_NOTIMPL;", source)
        self.assertIn("return grant_hr;", source)
        self.assertIn("FILE_ATTRIBUTE_REPARSE_POINT", source)
        self.assertIn("must remain outside the owned VM directory", source)
        self.assertEqual(
            source.count(
                "wcscpy_s(instance->data_disk_path, MAX_PATH, config->data_disk_path);"
            ),
            2,
        )

    def test_core_persists_but_never_owns_secondary_disk_contents(self):
        source = read("src/backend_win/asb_core.c")
        self.assertIn('fwprintf(f, L"DataDiskPath=%s\\n", g_vms[i].data_disk_path);', source)
        self.assertIn('wcsncmp(line, L"DataDiskPath=", 13)', source)
        self.assertIn("asb_vm_create_with_data_disk", source)
        self.assertIn("FILE_ATTRIBUTE_REPARSE_POINT", source)
        self.assertIn("Caller-owned secondary data disk must be outside the VM directory", source)
        for forbidden in (
            "CopyFileW(data_disk_path",
            "DeleteFileW(data_disk_path",
            "vhdx_create(data_disk_path",
            "vhdx_create_differencing(data_disk_path",
            "snapshot_init(data_disk_path",
        ):
            self.assertNotIn(forbidden, source)

    def test_headless_api_advertises_and_bounds_the_capability(self):
        source = read("src/app_win/headless.c")
        self.assertIn('"secondaryDataDisk\\\":true', source)
        self.assertIn('json_get_string(body, L"dataDiskPath", data_disk, MAX_PATH);', source)
        self.assertIn("asb_vm_create_with_data_disk(&cfg, data_disk)", source)
        self.assertIn("dataDiskPath is not supported for template creation", source)
        self.assertIn("dataDiskPath must differ from diskPath", source)


if __name__ == "__main__":
    unittest.main()
