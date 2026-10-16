import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('layout', ROOT / 'tools/check_flash_layout.py')
layout = importlib.util.module_from_spec(spec)
spec.loader.exec_module(layout)


class FlashLayoutTest(unittest.TestCase):
    def test_current_layout_matches_arduino_upload(self):
        layout.validate_layout(ROOT / 'partitions.csv', '0x10000', [('0xe000', 'boot_app0.bin')])

    def test_rejects_app_uploaded_to_wrong_address(self):
        with self.assertRaisesRegex(ValueError, 'App upload address'):
            layout.validate_layout(ROOT / 'partitions.csv', '0x30000', [('0xe000', 'boot_app0.bin')])

    def test_rejects_ota_initializer_in_nvs(self):
        with self.assertRaisesRegex(ValueError, 'OTA initializer'):
            layout.validate_layout(ROOT / 'partitions.csv', '0x10000', [('0x3b0000', 'boot_app0.bin')])

    def test_rejects_overlapping_nvs(self):
        text = (ROOT / 'partitions.csv').read_text().replace('0x3B0000', '0x3A0000')
        with tempfile.TemporaryDirectory() as folder:
            table = Path(folder) / 'partitions.csv'
            table.write_text(text)
            with self.assertRaisesRegex(ValueError, 'overlapping'):
                layout.validate_layout(table, '0x10000', [('0xe000', 'boot_app0.bin')])


if __name__ == '__main__':
    unittest.main()
