#!/usr/bin/env python3
"""Safely deploy KoboDash custom lightweight firmware to a USB-mounted Kobo eReader."""
import argparse
import hashlib
import io
import os
import shutil
import stat
import sys
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FW_DIR = ROOT / "firmware"
DEFAULT_MOUNT = Path("/Volumes/KOBOeReader")

FONT_SOURCES = {
    "ui-regular.ttf": Path("/System/Library/Fonts/Supplemental/Arial.ttf"),
    "ui-bold.ttf": Path("/System/Library/Fonts/Supplemental/Arial Bold.ttf"),
    "clock.ttf": Path("/System/Library/Fonts/Supplemental/DIN Alternate Bold.ttf"),
    # 7 crisp book reading font families (only active family loaded in RAM!)
    "book-regular.ttf": Path("/System/Library/Fonts/Supplemental/Georgia.ttf"),
    "book-bold.ttf": Path("/System/Library/Fonts/Supplemental/Georgia Bold.ttf"),
    "book-italic.ttf": Path("/System/Library/Fonts/Supplemental/Georgia Italic.ttf"),
    "book-charter.ttc": Path("/System/Library/Fonts/Supplemental/Charter.ttc"),
    "book-palatino.ttc": Path("/System/Library/Fonts/Palatino.ttc"),
    "book-baskerville.ttc": Path("/System/Library/Fonts/Supplemental/Baskerville.ttc"),
    "book-times-reg.ttf": Path("/System/Library/Fonts/Supplemental/Times New Roman.ttf"),
    "book-times-bold.ttf": Path("/System/Library/Fonts/Supplemental/Times New Roman Bold.ttf"),
    "book-times-italic.ttf": Path("/System/Library/Fonts/Supplemental/Times New Roman Italic.ttf"),
    "book-verdana-reg.ttf": Path("/System/Library/Fonts/Supplemental/Verdana.ttf"),
    "book-verdana-bold.ttf": Path("/System/Library/Fonts/Supplemental/Verdana Bold.ttf"),
    "book-verdana-italic.ttf": Path("/System/Library/Fonts/Supplemental/Verdana Italic.ttf"),
    "book-trebuchet-reg.ttf": Path("/System/Library/Fonts/Supplemental/Trebuchet MS.ttf"),
    "book-trebuchet-bold.ttf": Path("/System/Library/Fonts/Supplemental/Trebuchet MS Bold.ttf"),
    "book-trebuchet-italic.ttf": Path("/System/Library/Fonts/Supplemental/Trebuchet MS Italic.ttf"),
}


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def copy_clean(src: Path, dst: Path, executable: bool = False) -> None:
    """Copy raw bytes without macOS extended attributes (prevents AppleDouble ._* clutter)."""
    dst.parent.mkdir(parents=True, exist_ok=True)
    data = src.read_bytes()
    dst.write_bytes(data)
    if executable:
        dst.chmod(0o755)
    # Remove any stray AppleDouble file macOS might have created
    ad = dst.parent / f"._{dst.name}"
    if ad.exists():
        try:
            ad.unlink()
        except OSError:
            pass
    if sha256_file(src) != sha256_file(dst):
        raise RuntimeError(f"Checksum mismatch after copying {src} -> {dst}")


def verify_arm_elf(binary: Path) -> None:
    hdr = binary.read_bytes()[:20]
    if len(hdr) < 20 or hdr[:4] != b"\x7fELF" or hdr[4] != 1 or hdr[18:20] != b"\x28\x00":
        raise RuntimeError(f"{binary} is not a 32-bit little-endian ARM ELF binary")


def main() -> int:
    ap = argparse.ArgumentParser(description="Deploy KoboDash firmware to USB-mounted Kobo")
    ap.add_argument("--mount", type=Path, default=DEFAULT_MOUNT, help="Kobo USB mount path")
    ap.add_argument("--reset-config", action="store_true", help="Overwrite existing config.ini")
    ap.add_argument("--remove-trmnl", action="store_true", help="Remove legacy .adds/TRMNL and its NickelMenu entry")
    ap.add_argument("--install-autoboot", action="store_true",
                    help="Stage clean 2-file .kobo/KoboRoot.tgz (udev hook) so Kobo auto-boots into KoboDash")
    ap.add_argument("--server-url", type=str, default=None,
                    help="Set server_url inside Kobo config.ini without resetting other settings")
    ap.add_argument("--server-token", type=str, default=None,
                    help="Set server_token inside Kobo config.ini without resetting other settings")
    args = ap.parse_args()

    mount: Path = args.mount
    if not (mount / ".kobo").is_dir() or not (mount / ".adds" / "nm").is_dir():
        print(f"ERROR: Kobo with NickelMenu not found at {mount}", file=sys.stderr)
        return 1

    binary = FW_DIR / "build" / "kobodash"
    if not binary.is_file():
        print(f"ERROR: {binary} not built yet. Run `make -C firmware kobo` first.", file=sys.stderr)
        return 1
    verify_arm_elf(binary)

    target = mount / ".adds" / "kobodash"
    for sub in ("bin", "lib", "fonts", "dict", "certs", "cache", "logs", "screensavers"):
        (target / sub).mkdir(parents=True, exist_ok=True)

    # 1. Native ARM binary + standalone libfbink.so.1 & libz.so.1 + TLS root CA bundle
    copy_clean(binary, target / "bin" / "kobodash", executable=True)
    fbink_so = ROOT / "artifacts" / "inspect" / "libfbink.so.1.0.0"
    if fbink_so.is_file():
        copy_clean(fbink_so, target / "lib" / "libfbink.so.1", executable=True)
    zlib_so = ROOT / "artifacts" / "inspect" / "libz.so.1"
    if zlib_so.is_file():
        copy_clean(zlib_so, target / "lib" / "libz.so.1", executable=True)
    ca_pem = FW_DIR / "device" / "cacert.pem"
    if ca_pem.is_file():
        ca_dst = target / "certs" / "cacert.pem"
        if not ca_dst.exists() or sha256_file(ca_pem) != sha256_file(ca_dst):
            copy_clean(ca_pem, ca_dst)
            print(f"Deployed TLS CA bundle: {ca_dst}")

    # 1b. Pre-indexed offline dictionaries (EN WordNet + EN<->DE FreeDict)
    deployed_dicts = 0
    for dname in ("en.kdict", "en-de.kdict", "de-en.kdict", "de.kdict"):
        dict_src = ROOT / "output" / dname
        if dict_src.is_file():
            dict_dst = target / "dict" / dname
            if not dict_dst.exists() or sha256_file(dict_src) != sha256_file(dict_dst):
                copy_clean(dict_src, dict_dst)
                print(f"Deployed dictionary ({dict_src.stat().st_size / (1024*1024):.1f} MB): {dict_dst}")
            else:
                print(f"Dictionary up to date: {dict_dst}")
            deployed_dicts += 1
    if deployed_dicts == 0:
        print("WARNING: no .kdict files found in output/; run scripts/build_dict.py")

    # 2. Device shell wrappers
    for name in ("launch.sh", "runner.sh", "autoboot.sh", "selftest.sh",
                 "wifi-ensure.sh", "wifi-ctl.sh", "ssh-toggle.sh"):
        copy_clean(FW_DIR / "device" / name, target / name, executable=True)

    # Clean up any temporary probe binaries from earlier diagnostics
    for stale in ("probe_musl", "probe_gnu_norelro", "probe_gnu_fbink"):
        sp = target / "bin" / stale
        if sp.exists():
            sp.unlink()

    # 2b. Ensure ExcludeSyncFolders is present in Kobo eReader.conf so Nickel ignores .adds/
    kobo_conf = mount / ".kobo" / "Kobo" / "Kobo eReader.conf"
    if kobo_conf.is_file():
        excl_line = r'ExcludeSyncFolders=(\\.(?!kobo|adobe).+|([^.][^/]*/)+\\..+)'
        raw_text = kobo_conf.read_text("utf-8")
        nl = "\r\n" if "\r\n" in raw_text else "\n"
        lines = raw_text.splitlines()
        section = None
        feat_idx = None
        has_excl = False
        for idx, line in enumerate(lines):
            if line.startswith("["):
                section = line
                if section == "[FeatureSettings]":
                    feat_idx = idx
            elif section == "[FeatureSettings]" and line.startswith("ExcludeSyncFolders="):
                has_excl = True
        if not has_excl:
            if feat_idx is None:
                lines.extend(["", "[FeatureSettings]", excl_line])
            else:
                lines.insert(feat_idx + 1, excl_line)
            kobo_conf.write_bytes((nl.join(lines) + nl).encode("utf-8"))
            print(f"Restored ExcludeSyncFolders in {kobo_conf}")

    # 3. Default config (preserve existing user config unless --reset-config)
    cfg_dst = target / "config.ini"
    if args.reset_config or not cfg_dst.exists():
        copy_clean(FW_DIR / "device" / "config.default.ini", cfg_dst)
        print(f"Wrote default config: {cfg_dst}")
    else:
        print(f"Preserved existing config: {cfg_dst}")

    if args.server_url is not None or args.server_token is not None:
        lines = cfg_dst.read_text("utf-8").splitlines()
        out_lines = []
        saw_url = False
        saw_tok = False
        for line in lines:
            s = line.strip()
            if args.server_url is not None and s.startswith("server_url"):
                out_lines.append(f"server_url = {args.server_url}")
                saw_url = True
            elif args.server_token is not None and s.startswith("server_token"):
                out_lines.append(f"server_token = {args.server_token}")
                saw_tok = True
            else:
                out_lines.append(line)
        if args.server_url is not None and not saw_url:
            out_lines.append(f"server_url = {args.server_url}")
        if args.server_token is not None and not saw_tok:
            out_lines.append(f"server_token = {args.server_token}")
        cfg_dst.write_bytes(("\n".join(out_lines) + "\n").encode("utf-8"))
        print("Updated server connection in config.ini")

    # 4. Crisp TTF/TTC fonts (skip unchanged files for fast deploys; not committed to git)
    for dst_name, src_font in FONT_SOURCES.items():
        if src_font.is_file():
            dst_font = target / "fonts" / dst_name
            if not dst_font.exists() or sha256_file(src_font) != sha256_file(dst_font):
                copy_clean(src_font, dst_font)
        else:
            print(f"WARNING: font not found: {src_font} (will fall back to Georgia)")

    # 5. Clean NickelMenu entry (separate file; does not modify existing dashboard entries)
    nm_dst = mount / ".adds" / "nm" / "kobodash"
    copy_clean(FW_DIR / "device" / "kobodash-nm.conf", nm_dst)

    # 5b. Optional brick-proof 2-file KoboRoot.tgz udev auto-boot hook (never touches rcS!)
    if args.install_autoboot:
        rules_data = (FW_DIR / "device" / "99-kobodash.rules").read_bytes()
        hook_data = (FW_DIR / "device" / "boot-hook.sh").read_bytes()
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w:gz", format=tarfile.USTAR_FORMAT) as tf:
            dti = tarfile.TarInfo(name="usr/local/kobodash")
            dti.type = tarfile.DIRTYPE
            dti.mode = 0o755
            dti.uid = 0
            dti.gid = 0
            dti.uname = "root"
            dti.gname = "root"
            dti.mtime = 1763800000
            tf.addfile(dti)
            for rel, payload, mode in (
                ("etc/udev/rules.d/99-kobodash.rules", rules_data, 0o644),
                ("usr/local/kobodash/boot-hook.sh", hook_data, 0o755),
            ):
                ti = tarfile.TarInfo(name=rel)
                ti.size = len(payload)
                ti.mode = mode
                ti.uid = 0
                ti.gid = 0
                ti.uname = "root"
                ti.gname = "root"
                ti.mtime = 1763800000
                tf.addfile(ti, io.BytesIO(payload))
        tgz_bytes = buf.getvalue()
        # Verify archive members before writing to Kobo
        with tarfile.open(fileobj=io.BytesIO(tgz_bytes), mode="r:gz") as check_tf:
            names = [m.name for m in check_tf.getmembers()]
            assert names == ["usr/local/kobodash",
                             "etc/udev/rules.d/99-kobodash.rules",
                             "usr/local/kobodash/boot-hook.sh"], names
        kr_dst = mount / ".kobo" / "KoboRoot.tgz"
        kr_dst.write_bytes(tgz_bytes)
        kr_ad = kr_dst.parent / "._KoboRoot.tgz"
        if kr_ad.exists():
            kr_ad.unlink()
        if hashlib.sha256(kr_dst.read_bytes()).hexdigest() != hashlib.sha256(tgz_bytes).hexdigest():
            raise RuntimeError("KoboRoot.tgz checksum mismatch!")
        # Clear any stale .boot_pending marker so first auto-boot runs cleanly
        pend = target / ".boot_pending"
        if pend.exists():
            pend.unlink()
        print(f"Staged 2-file auto-boot udev package ({len(tgz_bytes)} bytes) at {kr_dst}")

    # 6. Optional clean removal of legacy TRMNL add-on
    if args.remove_trmnl:
        for p in (mount / ".adds" / "nm" / "TRMNL-dashboard",
                  mount / ".adds" / "nm" / "._TRMNL-dashboard",
                  mount / ".adds" / "._TRMNL"):
            if p.exists():
                p.unlink()
                print(f"Removed {p}")
        trmnl_dir = mount / ".adds" / "TRMNL"
        if trmnl_dir.is_dir():
            shutil.rmtree(trmnl_dir)
            print(f"Removed {trmnl_dir}")

    # Clean up any macOS dot-underscore metadata files we may have created
    for ad in target.rglob("._*"):
        try:
            ad.unlink()
        except OSError:
            pass
    nm_ad = mount / ".adds" / "nm" / "._kobodash"
    if nm_ad.exists():
        try:
            nm_ad.unlink()
        except OSError:
            pass

    print(f"Deployed KoboDash ARM binary ({binary.stat().st_size} bytes) to {target}")
    print(f"Registered NickelMenu entry at {nm_dst}")
    print("All SHA-256 checksums verified.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
