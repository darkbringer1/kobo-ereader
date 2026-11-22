#!/usr/bin/env python3
"""Build a minimal local add-on package from pinned upstream archives. No device I/O."""
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import zipfile

from fetch import SOURCES

ROOT = Path(__file__).resolve().parents[1]
DOWNLOADS = ROOT / 'artifacts/downloads'
EXPECTED = {name: checksum for name, _, checksum in SOURCES}


def main():
    for name, expected in EXPECTED.items():
        if hashlib.sha256((DOWNLOADS / name).read_bytes()).hexdigest() != expected:
            raise RuntimeError(f'Missing or changed upstream archive: {name}')
    key = ROOT / 'artifacts/keys/kobo_dashboard'
    key.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    if not key.exists():
        subprocess.run(['ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-C',
                        'kobo-dashboard-local', '-f', str(key)], check=True)
    if not key.with_suffix('.pub').read_text().startswith('ssh-ed25519 '):
        raise RuntimeError('Expected an Ed25519 public key')
    subprocess.run([sys.executable, str(ROOT / 'scripts/render.py'), '--output',
                    str(ROOT / 'output')], check=True)
    files = {}

    def add(name, data, mode=0o644):
        if name in files:
            raise RuntimeError(f'Duplicate package member: {name}')
        files[name] = (data, mode)

    with tarfile.open(DOWNLOADS / 'NickelMenu-v0.6.0-KoboRoot.tgz') as archive:
        members = [m for m in archive.getmembers() if m.isfile()]
        expected = {'usr/local/Kobo/imageformats/libnm.so', 'mnt/onboard/.adds/nm/doc'}
        if {m.name.removeprefix('./') for m in members} != expected:
            raise RuntimeError('NickelMenu archive has unexpected files')
        for member in members:
            add(member.name.removeprefix('./'), archive.extractfile(member).read(), member.mode)
    base = 'mnt/onboard/.adds/dashboard/'
    with zipfile.ZipFile(DOWNLOADS / 'FBInk-v1.25.0.zip') as outer:
        with tarfile.open(fileobj=io.BytesIO(outer.read('KoboRoot.tgz'))) as archive:
            for source, dest in [('./usr/local/fbink/bin/fbink', 'bin/fbink'),
                                 ('./usr/local/fbink/lib/libfbink.so.1.0.0', 'lib/libfbink.so.1')]:
                add(base + dest, archive.extractfile(source).read(), 0o755)
        for name in ['README.md', 'LICENSE', 'CREDITS']:
            add(base + 'licenses/FBInk-' + name, outer.read(name))
    with tarfile.open(DOWNLOADS / 'kobo-stuff-1.6.N-r18901.tar.xz') as outer:
        add(base + 'licenses/KoboStuff-README', outer.extractfile('KoboStuff/README').read())
        with tarfile.open(fileobj=io.BytesIO(outer.extractfile('KoboStuff/KoboRoot.tgz').read())) as archive:
            for source, dest in [('./usr/local/niluje/usbnet/bin/dropbearmulti', 'bin/dropbearmulti'),
                                 ('./usr/local/niluje/usbnet/bin/busybox', 'bin/busybox'),
                                 ('./usr/local/niluje/usbnet/lib/libz.so.1', 'lib/libz.so.1')]:
                add(base + dest, archive.extractfile(source).read(), 0o755)
    for source in sorted((ROOT / 'device').glob('*.sh')):
        add(base + source.name, source.read_bytes(), 0o755)
    add(base + 'authorized_keys', key.with_suffix('.pub').read_bytes())
    add(base + 'hello-dashboard.png', (ROOT / 'output/hello-dashboard.png').read_bytes())
    add('mnt/onboard/.adds/nm/dashboard', (ROOT / 'device/nickelmenu.conf').read_bytes())
    receipts = json.loads((DOWNLOADS / 'sources.json').read_text())
    add(base + 'licenses/upstream-sources.json', json.dumps(receipts, indent=2).encode() + b'\n')
    # The package is allowed to add only this NickelMenu library outside user storage.
    rootfs = [name for name in files if not name.startswith('mnt/onboard/')]
    if rootfs != ['usr/local/Kobo/imageformats/libnm.so']:
        raise RuntimeError(f'Unexpected root filesystem changes: {rootfs}')
    output = ROOT / 'output/device-install'
    output.mkdir(parents=True, exist_ok=True)
    package = output / 'KoboRoot.tgz'
    with tarfile.open(package, 'w:gz', format=tarfile.USTAR_FORMAT) as archive:
        for name, (data, mode) in sorted(files.items()):
            member = tarfile.TarInfo('./' + name)
            member.size, member.mode, member.uid, member.gid = len(data), mode, 0, 0
            archive.addfile(member, io.BytesIO(data))
    manifest = {'model_id': '310', 'firmware': '4.26.16704', 'requires_manual_install': True,
                'upstream_archives': EXPECTED,
                'package_sha256': hashlib.sha256(package.read_bytes()).hexdigest(),
                'rootfs_files': rootfs,
                'files': [{'path': name, 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
                          for name, (data, _) in sorted(files.items())]}
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    shutil.copyfile(ROOT / 'output/Hello Dashboard.cbz', output / 'Hello Dashboard.cbz')
    print(f'Prepared {package} ({package.stat().st_size:,} bytes)')
    print('No Kobo files changed. Private SSH key remains in artifacts/keys/.')


if __name__ == '__main__':
    main()
