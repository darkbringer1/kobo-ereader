#!/usr/bin/env python3
"""Fetch mbedTLS 3.6.3 source for reproducible static TLS 1.2/1.3 builds."""
from __future__ import annotations

import hashlib
import shutil
import tarfile
import urllib.request
from pathlib import Path

URL = "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.3/mbedtls-3.6.3.tar.bz2"
ROOT = Path(__file__).resolve().parent.parent
DST = ROOT / "firmware" / "third_party" / "mbedtls"


def main() -> int:
    if (DST / "library" / "ssl_tls.c").is_file():
        print(f"mbedTLS already present at {DST}")
        return 0
    DST.parent.mkdir(parents=True, exist_ok=True)
    archive = DST.parent / "mbedtls-3.6.3.tar.bz2"
    print(f"Downloading {URL}...")
    urllib.request.urlretrieve(URL, archive)
    h = hashlib.sha256(archive.read_bytes()).hexdigest()
    print(f"Downloaded {archive.name} (sha256={h[:16]}...)")
    with tarfile.open(archive, "r:bz2") as tf:
        tf.extractall(DST.parent)
    extracted = DST.parent / "mbedtls-3.6.3"
    if DST.exists():
        shutil.rmtree(DST)
    extracted.rename(DST)
    archive.unlink(missing_ok=True)
    print(f"Extracted mbedTLS 3.6.3 to {DST}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
