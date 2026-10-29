import json
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'dev/tools'))
from package_ble_console import build_archive, validate_archive


class BlePackage(unittest.TestCase):
    def test_complete_source_archive_and_tamper_rejection(self):
        version = (ROOT / 'VERSION').read_text().strip()
        commit = 'a' * 40
        with tempfile.TemporaryDirectory() as folder:
            archive_path = Path(folder) / 'console.zip'
            build_archive(archive_path, version, commit, False)
            with zipfile.ZipFile(archive_path) as archive:
                data = {name: archive.read(name) for name in archive.namelist()}
                self.assertIn('tools/ble_dashboard.py', data)
                self.assertIn('tools/qr_dialog.py', data)
                self.assertNotIn('config.json', data)
            data['tools/console_model.py'] += b'\n# altered\n'
            with zipfile.ZipFile(archive_path, 'w') as archive:
                for name, content in data.items(): archive.writestr(name, content)
            with self.assertRaisesRegex(AssertionError, 'digest'):
                validate_archive(archive_path, version, commit)

    def test_rejects_dirty_provenance_and_unexpected_file(self):
        version = (ROOT / 'VERSION').read_text().strip()
        with tempfile.TemporaryDirectory() as folder:
            archive_path = Path(folder) / 'console.zip'
            build_archive(archive_path, version, 'b' * 40, True)
            with self.assertRaisesRegex(AssertionError, 'provenance'):
                validate_archive(archive_path, version, 'b' * 40)
            with zipfile.ZipFile(archive_path, 'a') as archive:
                archive.writestr('config.json', json.dumps({'fixture': True}))
            with self.assertRaisesRegex(AssertionError, 'entry'):
                validate_archive(archive_path, version, 'b' * 40, True)
