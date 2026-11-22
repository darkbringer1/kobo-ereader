"""Exercise USB staging safeguards against a temporary fake device, never real hardware."""
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location('stage', Path(__file__).resolve().parents[1] / 'scripts/stage.py')
stage = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(stage)


class StagingSafetyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.volume = self.root / 'device'
        self.backup = self.root / 'backups/complete'
        originals = {'.kobo/version': b'TEST,2.6.35.3,4.26.16704,0,0,000000000310\n',
                     '.kobo/Kobo/Kobo eReader.conf': b'[General]\r\nExisting=preserved\r\n',
                     '.kobo/KoboReader.sqlite': b'placeholder database bytes',
                     '.kobo/nightmode.sh': b'original launcher bytes'}
        records = []
        for name, data in originals.items():
            for directory in [self.volume, self.backup / 'onboard']:
                destination = directory / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(data)
            records.append({'path': name, 'sha256': hashlib.sha256(data).hexdigest()})
        (self.backup / 'manifest.json').write_text(json.dumps({'files': records}))
        self.bundle = self.root / 'output/device-install'
        self.bundle.mkdir(parents=True)
        name, data = 'usr/local/Kobo/imageformats/libnm.so', b'placeholder library bytes'
        package = self.bundle / 'KoboRoot.tgz'
        with tarfile.open(package, 'w:gz') as archive:
            info = tarfile.TarInfo('./' + name)
            info.size = len(data)
            archive.addfile(info, io.BytesIO(data))
        (self.bundle / 'manifest.json').write_text(json.dumps({
            'firmware': '4.26.16704', 'model_id': '310',
            'package_sha256': stage.digest(package),
            'files': [{'path': name, 'sha256': hashlib.sha256(data).hexdigest()}]}))
        (self.bundle / 'Hello Dashboard.cbz').write_bytes(b'placeholder preview bytes')
        self.before = self.snapshot()

    def snapshot(self):
        return {str(p.relative_to(self.volume)): p.read_bytes()
                for p in self.volume.rglob('*') if p.is_file()}

    def run_stage(self, write=False):
        args = ['stage.py', '--volume', str(self.volume), '--backup', str(self.backup)]
        if write:
            args.append('--write')
        with patch.object(stage, 'ROOT', self.root), patch('sys.argv', args), \
                patch.object(stage.os.path, 'ismount', return_value=True), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            stage.main()

    def test_dry_run_never_changes_device_files(self):
        self.run_stage()
        self.assertEqual(self.snapshot(), self.before)

    def test_staging_preserves_books_settings_and_existing_launcher(self):
        self.run_stage(write=True)
        after = self.snapshot()
        self.assertEqual(after['.kobo/nightmode.sh'], self.before['.kobo/nightmode.sh'])
        self.assertEqual(after['.kobo/KoboReader.sqlite'], self.before['.kobo/KoboReader.sqlite'])
        self.assertIn(b'Existing=preserved\r\n', after[str(stage.CONFIG)])
        self.assertIn(stage.EXCLUSION.encode(), after[str(stage.CONFIG)])
        self.assertEqual(after['.kobo/KoboRoot.tgz'], (self.bundle / 'KoboRoot.tgz').read_bytes())

    def test_stale_backup_stops_without_changes(self):
        (self.volume / '.kobo/KoboReader.sqlite').write_bytes(b'new reading progress')
        before = self.snapshot()
        with self.assertRaises(SystemExit):
            self.run_stage(write=True)
        self.assertEqual(self.snapshot(), before)

    def test_existing_installer_is_never_overwritten(self):
        pending = self.volume / '.kobo/KoboRoot.tgz'
        pending.write_bytes(b'another installer')
        before = self.snapshot()
        with self.assertRaises(SystemExit):
            self.run_stage(write=True)
        self.assertEqual(self.snapshot(), before)

    def test_wrong_firmware_stops_without_changes(self):
        (self.volume / '.kobo/version').write_text('TEST,kernel,5.0.0,000000000310')
        before = self.snapshot()
        with self.assertRaises(SystemExit):
            self.run_stage(write=True)
        self.assertEqual(self.snapshot(), before)

    def test_io_failure_rolls_back_config_and_new_installer(self):
        with patch.object(stage.os, 'sync', side_effect=OSError('simulated sync failure')):
            with self.assertRaises(OSError):
                self.run_stage(write=True)
        self.assertEqual(self.snapshot(), self.before)

    def test_custom_hidden_folder_setting_is_not_replaced(self):
        original = b'[FeatureSettings]\nExcludeSyncFolders=custom\nOther=preserved\n'
        with self.assertRaises(RuntimeError):
            stage.configure(original)


if __name__ == '__main__':
    unittest.main()
