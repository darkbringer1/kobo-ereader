#!/usr/bin/env python3
"""Download and verify the exact upstream archives inspected for this prototype."""
import hashlib
import json
from pathlib import Path
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
SOURCES = [
    ('FBInk-v1.25.0.zip',
     'https://www.mobileread.com/forums/attachment.php?attachmentid=198199&d=1670270458',
     'ab2f65ef8faa9cdfe5d48241bc2eb99253e4d2afd02ba483b4703c8abc38b87e'),
    ('NickelMenu-v0.6.0-KoboRoot.tgz',
     'https://github.com/pgaskin/NickelMenu/releases/download/v0.6.0/KoboRoot.tgz',
     '322ff9aa863860e8f5f7e0b55cae561c54bf95983b9bce1d19819d1225d064af'),
    ('kobo-stuff-1.6.N-r18901.tar.xz',
     'https://storage.gra.cloud.ovh.net/v1/AUTH_2ac4bfee353948ec8ea7fd1710574097/mr-public/Kobo/kobo-stuff-1.6.N-r18901.tar.xz',
     '289e6959a71d1cd9dc3e5f770a03280b23b54e77404ae19147ad3dd702905d1f'),
]


def main():
    folder = ROOT / 'artifacts/downloads'
    folder.mkdir(parents=True, exist_ok=True)
    receipts = []
    for name, url, expected in SOURCES:
        destination = folder / name
        if destination.exists():
            data = destination.read_bytes()
        else:
            request = urllib.request.Request(url, headers={'User-Agent': 'KoboDashboardPrototype/0.1'})
            with urllib.request.urlopen(request, timeout=60) as response:
                data = response.read()
        actual = hashlib.sha256(data).hexdigest()
        if actual != expected:
            raise RuntimeError(f'Upstream checksum mismatch: {name}; inspect before proceeding')
        if not destination.exists():
            destination.write_bytes(data)
        receipts.append({'file': name, 'url': url, 'size': len(data), 'sha256': actual})
        print(f'Verified {name}')
    (folder / 'sources.json').write_text(json.dumps(receipts, indent=2) + '\n')


if __name__ == '__main__':
    main()
