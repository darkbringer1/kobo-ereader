#!/usr/bin/env python3
"""Validate prepared add-ons and backups; write USB files only with --write. Never eject."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import tarfile

ROOT = Path(__file__).resolve().parents[1]
CONFIG = Path('.kobo/Kobo/Kobo eReader.conf')
EXCLUSION = r'ExcludeSyncFolders=(\\.(?!kobo|adobe).+|([^.][^/]*/)+\\..+)'


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def configure(data):
    # Preserve all unrelated settings and the original newline convention.
    text = data.decode('utf-8')
    newline = '\r\n' if '\r\n' in text else '\n'
    lines = text.splitlines()
    section = None
    feature = None
    for i, line in enumerate(lines):
        if line.startswith('['):
            section = line
            if section == '[FeatureSettings]':
                feature = i
        elif section == '[FeatureSettings]' and line.startswith('ExcludeSyncFolders='):
            if line != EXCLUSION:
                raise RuntimeError('Existing ExcludeSyncFolders differs; review and merge it before installing')
            return data
    if feature is None:
        lines.extend(['', '[FeatureSettings]', EXCLUSION])
    else:
        lines.insert(feature + 1, EXCLUSION)
    return (newline.join(lines) + newline).encode('utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--volume', type=Path, default=Path('/Volumes/KOBOeReader'))
    parser.add_argument('--backup', type=Path, help='Backup directory containing manifest.json; defaults to latest complete backup')
    parser.add_argument('--write', action='store_true', help='Stage files for installation on next safe eject/disconnect')
    args = parser.parse_args()
    volume = args.volume.resolve()
    if not os.path.ismount(volume) or not (volume / '.kobo/version').is_file():
        parser.error('A mounted Kobo user volume is required')
    version = (volume / '.kobo/version').read_text().strip().split(',')
    bundle = ROOT / 'output/device-install'
    manifest = json.loads((bundle / 'manifest.json').read_text())
    if version[2] != manifest['firmware'] or version[-1][-3:] != manifest['model_id']:
        parser.error('Device identity or firmware differs from the inspected device')
    backup = args.backup
    if backup is None:
        complete = sorted((ROOT / 'backups').glob('*/manifest.json'))
        if not complete:
            parser.error('Create and verify a user-volume backup first')
        backup = complete[-1].parent
    record = json.loads((backup / 'manifest.json').read_text())
    saved = {r['path']: r for r in record['files']}
    # Verify settings, device identity, databases, and prior launcher files before changes.
    critical = ['.kobo/version', str(CONFIG), '.kobo/KoboReader.sqlite',
                '.kobo/BookReader.sqlite', '.kobo/on_start.sh', '.kobo/nightmode.sh',
                '.kobo/nightmode.ini', '.kobo/fmon/fmon', '.kobo/fmon/fmon_nightmode.sh']
    for name in critical:
        original = volume / name
        if not original.exists():
            if name in saved:
                parser.error(f'Previously backed-up file disappeared: {name}')
            continue
        if name not in saved or digest(original) != saved[name]['sha256']:
            parser.error(f'Backup does not match current device file: {name}; create a fresh backup')
        if digest(backup / 'onboard' / name) != saved[name]['sha256']:
            parser.error(f'Backup file failed verification: {name}')
    package = bundle / 'KoboRoot.tgz'
    if digest(package) != manifest['package_sha256']:
        parser.error('Prepared package checksum mismatch; rebuild it')
    expected = {item['path']: item for item in manifest['files']}
    with tarfile.open(package) as archive:
        members = archive.getmembers()
        if len(members) != len(expected):
            parser.error('Unexpected archive member count')
        seen = set()
        for member in members:
            name = member.name.removeprefix('./')
            if name not in expected or name in seen or not member.isfile():
                parser.error(f'Unexpected archive entry: {name}')
            seen.add(name)
            content = archive.extractfile(member).read()
            if hashlib.sha256(content).hexdigest() != expected[name]['sha256']:
                parser.error(f'Package member checksum mismatch: {name}')
    for relative in ['.kobo/KoboRoot.tgz', '.adds/dashboard', '.adds/nm/dashboard',
                     '.adds/nm/doc', 'Hello Dashboard.cbz']:
        if (volume / relative).exists() or (volume / relative).is_symlink():
            parser.error(f'Would overwrite existing path: {relative}; inspect before proceeding')
    original_config = (volume / CONFIG).read_bytes()
    new_config = configure(original_config)
    required = package.stat().st_size + 8 * 1024 * 1024
    if shutil.disk_usage(volume).free < required:
        parser.error('Insufficient free user storage')
    print(f'Confirmed Touch ID {manifest["model_id"]}, firmware {manifest["firmware"]}')
    print(f'Backup checked: {backup}')
    print('Changes on USB: add hidden-folder exclusion, Hello Dashboard.cbz, .kobo/KoboRoot.tgz')
    print('At installation: add NickelMenu library and .adds/dashboard tools; no boot partition changes')
    if not args.write:
        print('DRY RUN PASSED. Use --write when ready. No Kobo files changed.')
        return
    os.umask(0o077)
    logdir = ROOT / 'artifacts/staging' / datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    logdir.mkdir(parents=True)
    (logdir / 'Kobo eReader.conf.before').write_bytes(original_config)
    pending = volume / '.kobo/KoboRoot.tgz'
    book = volume / 'Hello Dashboard.cbz'
    created_book = False
    created_pending = False
    try:
        (volume / CONFIG).write_bytes(new_config)
        with book.open('xb') as stream:
            created_book = True
            stream.write((bundle / book.name).read_bytes())
        if digest(book) != digest(bundle / book.name):
            raise RuntimeError('CBZ transfer checksum mismatch')
        # Installer is written last, after its dependencies and settings are ready.
        with pending.open('xb') as stream:
            created_pending = True
            stream.write(package.read_bytes())
        if digest(pending) != manifest['package_sha256']:
            raise RuntimeError('Installer transfer checksum mismatch')
        os.sync()
    except Exception:
        if created_pending:
            pending.unlink(missing_ok=True)
        if created_book:
            book.unlink(missing_ok=True)
        (volume / CONFIG).write_bytes(original_config)
        raise
    (logdir / 'receipt.json').write_text(json.dumps({'backup': str(backup),
        'package_sha256': manifest['package_sha256'], 'volume': str(volume)}, indent=2) + '\n')
    print('STAGED and verified. Device is still mounted. Safe eject/disconnect triggers installation.')
    print(f'Pre-install settings retained in {logdir}')


if __name__ == '__main__':
    main()
