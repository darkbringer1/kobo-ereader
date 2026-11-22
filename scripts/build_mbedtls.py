#!/usr/bin/env python3
"""Compile minimal client-only mbedTLS 3.6.x static library for Kobo ARM or host."""
from __future__ import annotations

import concurrent.futures
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MBED_DIR = ROOT / "firmware" / "third_party" / "mbedtls"
LIB_DIR = MBED_DIR / "library"
INC_MBED = MBED_DIR / "include"
INC_KD = ROOT / "firmware" / "include"


def ensure_sources() -> None:
    if not (LIB_DIR / "ssl_tls.c").is_file():
        subprocess.run([sys.executable, str(ROOT / "scripts" / "fetch_mbedtls.py")], check=True)


def build_lib(mode: str) -> Path:
    ensure_sources()
    out_dir = ROOT / "firmware" / "build" / f"mbedtls-{mode}"
    obj_dir = out_dir / "obj"
    obj_dir.mkdir(parents=True, exist_ok=True)
    lib_path = out_dir / "libmbedtls.a"

    if mode == "kobo":
        cc = ["zig", "cc", "--target=arm-linux-gnueabihf.2.19", "-mcpu=cortex_a8"]
        ar = ["zig", "ar"]
    else:
        cc = [os.environ.get("HOST_CC", "cc")]
        ar = ["ar"]

    cfg_hdr = INC_KD / "kd_mbedtls_config.h"
    cfg_mtime = cfg_hdr.stat().st_mtime

    cflags = [
        "-O2",
        "-std=c99",
        "-ffunction-sections",
        "-fdata-sections",
        f"-I{INC_KD}",
        f"-I{INC_MBED}",
        f"-I{LIB_DIR}",
        '-DMBEDTLS_CONFIG_FILE="kd_mbedtls_config.h"',
        "-Wno-unused-function",
        "-Wno-unused-parameter",
    ]

    srcs = sorted(LIB_DIR.glob("*.c"))
    objs: list[Path] = []
    to_compile: list[tuple[Path, Path]] = []

    for src in srcs:
        obj = obj_dir / (src.stem + ".o")
        objs.append(obj)
        need = not obj.exists() or obj.stat().st_mtime < max(src.stat().st_mtime, cfg_mtime)
        if need:
            to_compile.append((src, obj))

    if to_compile:
        print(f"Compiling {len(to_compile)} mbedTLS objects ({mode})...")

        def compile_one(pair: tuple[Path, Path]) -> None:
            src, obj = pair
            cmd = [*cc, *cflags, "-c", str(src), "-o", str(obj)]
            res = subprocess.run(cmd, capture_output=True, text=True)
            if res.returncode != 0:
                raise RuntimeError(f"Failed compiling {src.name}:\n{res.stderr}")

        with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
            list(ex.map(compile_one, to_compile))

    if to_compile or not lib_path.exists():
        if lib_path.exists():
            lib_path.unlink()
        subprocess.run([*ar, "rcs", str(lib_path), *(str(o) for o in objs)], check=True)
        print(f"Built {lib_path} ({lib_path.stat().st_size // 1024} KB)")

    # Ensure CA bundle exists for deployment
    ca_dst = ROOT / "firmware" / "device" / "cacert.pem"
    if not ca_dst.exists():
        for cand in ("/etc/ssl/cert.pem", "/etc/ssl/certs/ca-certificates.crt"):
            if Path(cand).is_file():
                shutil.copy2(cand, ca_dst)
                print(f"Copied root CA bundle from {cand} -> {ca_dst}")
                break

    return lib_path


def main() -> int:
    if len(sys.argv) != 2 or sys.argv[1] not in ("kobo", "host"):
        print("Usage: build_mbedtls.py <kobo|host>", file=sys.stderr)
        return 1
    build_lib(sys.argv[1])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
