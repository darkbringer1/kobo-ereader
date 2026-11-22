#!/usr/bin/env python3
"""Fetch only the reviewed TRMNL Kobo files, pinned by commit and per-file SHA256."""
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def main():
    metadata = json.loads((ROOT / 'device/trmnl/upstream-files.json').read_text())
    folder = ROOT / 'artifacts/trmnl-review'
    def fetch(item):
        destination = folder / item['path']
        if destination.exists():
            content = destination.read_bytes()
        else:
            url = f'https://raw.githubusercontent.com/usetrmnl/trmnl-kobo/{metadata["commit"]}/{item["path"]}'
            request = urllib.request.Request(url, headers={'User-Agent': 'KoboOpenDashboard/0.1'})
            with urllib.request.urlopen(request, timeout=40) as response:
                content = response.read()
        if hashlib.sha256(content).hexdigest() != item['sha256'] or len(content) != item['size']:
            raise RuntimeError(f'Upstream checksum mismatch: {item["path"]}')
        if not destination.exists():
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(content)
    with ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(fetch, metadata['files']))
    (folder / 'files.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(f'Verified {len(metadata["files"])} upstream files at {metadata["commit"]}')


if __name__ == '__main__':
    main()
