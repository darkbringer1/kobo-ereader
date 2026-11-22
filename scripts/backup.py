#!/usr/bin/env python3
"""Copy Kobo user files to a private local backup, verifying every file's SHA-256."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--volume', type=Path, default=Path('/Volumes/KOBOeReader'))
    parser.add_argument('--destination', type=Path, default=Path('backups'))
    args = parser.parse_args()
    source = args.volume.resolve()
    if not os.path.ismount(source) or not (source / '.kobo/version').is_file():
        parser.error('Source must be a mounted Kobo user volume with .kobo/version')
    destination = args.destination.resolve()
    if destination == source or source in destination.parents:
        parser.error('Backup destination must be outside the Kobo')
    os.umask(0o077)
    root = destination / datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    target = root / 'onboard'
    target.mkdir(parents=True)
    # macOS/Windows indexing and trash directories are not Kobo software or books.
    excluded = {'.Spotlight-V100', '.Trashes', '.fseventsd', 'System Volume Information'}
    records = []
    total = 0
    for folder, dirs, files in os.walk(source):
        base = Path(folder)
        if base == source:
            dirs[:] = sorted(d for d in dirs if d not in excluded)
        for directory in dirs:
            if (base / directory).is_symlink():
                raise RuntimeError(f'Unexpected symlink: {base / directory}')
        (target / base.relative_to(source)).mkdir(parents=True, exist_ok=True)
        for name in sorted(files):
            item = base / name
            if not item.is_file() or item.is_symlink():
                raise RuntimeError(f'Unexpected non-regular file: {item}')
            relative = item.relative_to(source)
            output = target / relative
            checksum = hashlib.sha256()
            with item.open('rb') as src, output.open('xb') as dst:
                while chunk := src.read(1024 * 1024):
                    checksum.update(chunk)
                    dst.write(chunk)
            shutil.copystat(item, output)
            if digest(output) != checksum.hexdigest():
                raise RuntimeError(f'Backup verification failed: {relative}')
            size = output.stat().st_size
            records.append({'path': str(relative), 'size': size, 'sha256': checksum.hexdigest()})
            total += size
            if len(records) % 100 == 0:
                print(f'Verified {len(records)} files / {total / 1e6:.1f} MB', flush=True)
    manifest = {'source': str(source), 'scope': 'user volume only; no OS or boot partitions',
                'excluded_root_directories': sorted(excluded), 'bytes': total, 'files': records}
    (root / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'COMPLETE: {root} ({len(records)} verified files, {total / 1e6:.1f} MB)', flush=True)


if __name__ == '__main__':
    main()
