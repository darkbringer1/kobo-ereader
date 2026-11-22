#!/usr/bin/env python3
"""Update only the existing access scripts over USB, backing up every replaced file."""
import argparse
from datetime import datetime, timezone
import hashlib
import io
import json
import os
from pathlib import Path
import tarfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--volume', type=Path, default=Path('/Volumes/KOBOeReader'))
    parser.add_argument('--write', action='store_true')
    parser.add_argument('--with-trmnl', action='store_true', help='Also stage the prepared TRMNL client without enabling continuous mode')
    args = parser.parse_args()
    volume = args.volume.resolve()
    if not os.path.ismount(volume):
        parser.error('Kobo must be mounted in USB mode')
    version = (volume / '.kobo/version').read_text().strip().split(',')
    if version[2] != '4.26.16704' or version[-1][-3:] != '310':
        parser.error('Device differs from the inspected Kobo')
    if not (volume / '.adds/dashboard/bin/dropbearmulti').is_file():
        parser.error('Original dashboard installation is required')
    files = {}
    for name in ['start-ssh.sh', 'repair-ssh.sh', 'probe.sh']:
        files['.adds/dashboard/' + name] = (ROOT / 'device' / name).read_bytes()
    files['.adds/nm/dashboard'] = (ROOT / 'device/nickelmenu.conf').read_bytes()
    if args.with_trmnl:
        if (volume / '.adds/TRMNL').exists():
            parser.error('TRMNL already exists; preserve its configuration and inspect before updating')
        source = ROOT / 'output/trmnl-install/TRMNL'
        manifest = json.loads((source.parent / 'manifest.json').read_text())
        for item in manifest['files']:
            path = source / item['path']
            data = path.read_bytes()
            if hashlib.sha256(data).hexdigest() != item['sha256']:
                parser.error(f'Prepared TRMNL file changed: {item["path"]}')
            files['.adds/TRMNL/' + item['path']] = data
        files['.adds/nm/TRMNL-dashboard'] = (ROOT / 'device/trmnl/nickelmenu.conf').read_bytes()
    with tarfile.open(ROOT / 'artifacts/downloads/kobo-stuff-1.6.N-r18901.tar.xz') as outer:
        inner = outer.extractfile('KoboStuff/KoboRoot.tgz').read()
        with tarfile.open(fileobj=io.BytesIO(inner)) as archive:
            files['.adds/dashboard/bin/busybox'] = archive.extractfile('./usr/local/niluje/usbnet/bin/busybox').read()
    print('Access update: scripts, WiFi controls, and missing upstream BusyBox shell.')
    print('No firmware installer or boot files will be copied.')
    if args.with_trmnl:
        print('TRMNL client will be staged; server credentials and continuous mode are not yet enabled.')
    if not args.write:
        print('DRY RUN PASSED; use --write to transfer.')
        return
    os.umask(0o077)
    backup = ROOT / 'backups/updates' / datetime.now(timezone.utc).strftime('access-%Y%m%dT%H%M%S.%fZ')
    backup.mkdir(parents=True)
    originals = {}
    for name in files:
        destination = volume / name
        if destination.is_symlink():
            parser.error(f'Refusing to replace a symlink: {name}')
        old = destination.read_bytes() if destination.exists() else None
        originals[name] = old
        if old is not None:
            saved = backup / name
            saved.parent.mkdir(parents=True, exist_ok=True)
            saved.write_bytes(old)
            if saved.read_bytes() != old:
                raise RuntimeError('Access backup failed verification')
    written = []
    try:
        # Update menu last so it cannot refer to files that have not been copied.
        for name in sorted(files, key=lambda name: name.startswith('.adds/nm/')):
            destination = volume / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            temporary = destination.with_name(destination.name + '.access-new')
            with temporary.open('xb') as stream:
                stream.write(files[name])
            if temporary.read_bytes() != files[name]:
                raise RuntimeError(f'Transfer failed verification: {name}')
            os.replace(temporary, destination)
            written.append(name)
        os.sync()
    except Exception:
        for name in reversed(written):
            destination = volume / name
            if originals[name] is None:
                destination.unlink(missing_ok=True)
            else:
                destination.write_bytes(originals[name])
        raise
    receipt = [{'path': name, 'sha256': hashlib.sha256(data).hexdigest()}
               for name, data in files.items()]
    (backup / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(f'Transferred and verified {len(files)} files. Backups: {backup}')
    print('Safe eject and unplug, then select Development WiFi on and Start dashboard SSH.')


if __name__ == '__main__':
    main()
