import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("framework_header_patches",
    ROOT / "tools/patches/framework_header_patches.py")
PATCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PATCH)


class RequiredHeaderPatchTests(unittest.TestCase):
    def test_stock_headers_upgrade_idempotently(self):
        for source, patch in [(PATCH.USB_ORIG, PATCH.patch_usb_header),
                              (PATCH.WS_ANCHOR, PATCH.patch_webserver_header)]:
            updated = patch(source)
            self.assertNotEqual(updated, source)
            self.assertEqual(patch(updated), updated)

    def test_changed_or_partial_header_patches_fail_closed(self):
        for source, patch in [
            (PATCH.USB_ORIG.replace("2048", "4096"), PATCH.patch_usb_header),
            (PATCH.USB_ORIG + PATCH.USB_PATCHED, PATCH.patch_usb_header),
            (PATCH.USB_PATCHED * 2, PATCH.patch_usb_header),
            (PATCH.WS_ANCHOR + "\n bool hasUpload() const;", PATCH.patch_webserver_header),
            (PATCH.WS_ANCHOR * 2, PATCH.patch_webserver_header),
            ("class WebServer {};", PATCH.patch_webserver_header)]:
            with self.subTest(source=source):
                with self.assertRaises(RuntimeError):
                    patch(source)


if __name__ == "__main__":
    unittest.main()
