#!/usr/bin/env python3
"""Prepare a pinned TRMNL Kobo client with private dependencies and a bounded preview."""
import hashlib
import io
import json
from pathlib import Path
import shutil
import tarfile

ROOT = Path(__file__).resolve().parents[1]
COMMIT = '731c402eb3a0b3014d36277d8ef8b35f671a2be5'


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError(f'Upstream patch anchor changed: {old[:60]}')
    return text.replace(old, new, 1)


def harden_preview(loop):
    """Fail the bounded preview on transport/render errors; keep radio untouched."""
    loop = replace_once(loop, 'function ErrorOnCurl(){', '''function ErrorOnCurl(){
    if [ "${TRMNL_TEST_ONCE:-0}" = "1" ]; then
        echo "Preview download failed ($curl_status)" >&2
        exit 1
    fi''')
    loop = replace_once(loop,
        './bin/fbink/fbink -g file=/tmp/trmnl.$trmnl_image_format,valign=CENTER,halign=CENTER,h=-2,w=0 -c -f',
        './bin/fbink/fbink -g file=/tmp/trmnl.$trmnl_image_format,valign=CENTER,halign=CENTER,h=-2,w=0 -c -f || exit 1')
    return loop


def main():
    source = ROOT / 'artifacts/trmnl-review'
    metadata = json.loads((ROOT / 'device/trmnl/upstream-files.json').read_text())
    if metadata['commit'] != COMMIT:
        raise RuntimeError('Unexpected TRMNL source revision')
    for item in metadata['files']:
        if hashlib.sha256((source / item['path']).read_bytes()).hexdigest() != item['sha256']:
            raise RuntimeError(f'Changed upstream file: {item["path"]}')
    output = ROOT / 'output/trmnl-install/TRMNL'
    if output.exists():
        raise RuntimeError('Prepared TRMNL folder already exists; retain its config and inspect before rebuilding')
    shutil.copytree(source / 'src/TRMNL', output)
    shutil.copyfile(source / 'LICENSE', output / 'LICENSE')
    (output / 'UPSTREAM.json').write_text(json.dumps(metadata, indent=2) + '\n')
    deps = output / 'deps'
    (deps / 'bin').mkdir(parents=True)
    (deps / 'lib').mkdir()
    with tarfile.open(ROOT / 'artifacts/downloads/kobo-stuff-1.6.N-r18901.tar.xz') as outer:
        inner = outer.extractfile('KoboStuff/KoboRoot.tgz').read()
        with tarfile.open(fileobj=io.BytesIO(inner)) as archive:
            for name in ['curl', 'jq']:
                (deps / 'bin' / name).write_bytes(archive.extractfile('./mnt/onboard/.niluje/usbnet/bin/' + name).read())
            (deps / 'bin/busybox').write_bytes(archive.extractfile('./usr/local/niluje/usbnet/bin/busybox').read())
            for name in ['libcurl.so.4', 'libssl.so.1.1', 'libcrypto.so.1.1', 'libzstd.so.1', 'libz.so.1']:
                (deps / 'lib' / name).write_bytes(archive.extractfile('./mnt/onboard/.niluje/usbnet/lib/' + name).read())
    # Include explicit applet wrappers on FAT, where symlinks are unavailable.
    for applet in ['pkill', 'usleep', 'hwclock', 'md5sum', 'killall']:
        (deps / 'bin' / applet).write_text('#!/bin/sh\nexec /mnt/onboard/.adds/TRMNL/deps/bin/busybox ' + applet + ' "$@"\n')
    config = {'TrmnlId': '', 'TrmnlToken': '', 'TrmnlApiUrl': 'http://192.0.2.10:2300/api',
              'DebugToScreen': 0, 'LogToServer': 'NONE', 'LoopMaxIteration': 0,
              'ConnectedGracePeriod': 20, 'ImageFormat': 'png', 'IgnoreCurlErrors': True,
              'WpaNetworkId': -1}
    (output / 'config.json').write_text(json.dumps(config, indent=2) + '\n')
    (output / 'version.txt').write_text('1.6.10\n')
    # This version is the supported TRMNL API dialect, not a Kobo firmware image.
    loop = (output / 'trmnlloop.sh').read_text()
    loop = replace_once(loop, './scripts/ledToggle.sh 8 >>/tmp/debug.log 2>&1',
                        'if [ "${TRMNL_TEST_ONCE:-0}" != "1" ]; then\n./scripts/ledToggle.sh 8 >>/tmp/debug.log 2>&1')
    loop = replace_once(loop, 'sleep $trmnl_loop_connected_grace_period',
                        'sleep $trmnl_loop_connected_grace_period\nfi')
    loop = loop.replace('curl "${trmnl_apiurl}/display" -L',
                        'curl --fail --connect-timeout 10 --max-time 40 --retry 1 "${trmnl_apiurl}/display" -L')
    loop = loop.replace('curl -L -o /tmp/trmnl.$trmnl_image_format',
                        'curl --fail --connect-timeout 10 --max-time 40 --retry 1 -L -o /tmp/trmnl.$trmnl_image_format')
    loop = replace_once(loop, '-H "Battery-Voltage: $trmnl_fake_voltage"',
                        '-H "Battery-Voltage: $trmnl_fake_voltage" \\\n    -H "Percent-Charged: $batteryCapacity"')
    loop = replace_once(loop, '        # With png image is already in portrait', '''        if [ "${TRMNL_TEST_ONCE:-0}" = "1" ]; then
            STOPPED=""
            resume_preview() { for pid in $STOPPED; do kill -CONT "$pid" 2>/dev/null || true; done; }
            trap resume_preview 0
            trap 'exit 1' HUP INT TERM
            for pid in $(pidof nickel); do STOPPED="$STOPPED $pid"; kill -STOP "$pid"; done
        fi
        # With png image is already in portrait''')
    loop = replace_once(loop, '        # rotate back to portrait mode', '''        if [ "${TRMNL_TEST_ONCE:-0}" = "1" ]; then
            sleep "${TRMNL_PREVIEW_SECONDS:-12}"
            exit 0
        fi
        # rotate back to portrait mode''')
    (output / 'trmnlloop.sh').write_text(harden_preview(loop))
    for name in ['environment.sh', 'preview.sh', 'launch.sh']:
        shutil.copyfile(ROOT / 'device/trmnl' / name, output / name)
    shutil.copyfile(ROOT / 'device/trmnl/getrssi.sh', output / 'scripts/getrssi.sh')
    for path in output.rglob('*'):
        if path.is_file() and (path.suffix == '.sh' or '/bin/' in str(path) or path.name == 'luajit'):
            path.chmod(0o755)
    manifest = {'upstream_commit': COMMIT, 'protocol_version': '1.6.10',
                'configured': False, 'files': []}
    for path in sorted(output.rglob('*')):
        if path.is_file():
            manifest['files'].append({'path': str(path.relative_to(output)), 'size': path.stat().st_size,
                                      'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
    (output.parent / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Prepared {len(manifest["files"])} files, {sum(f["size"] for f in manifest["files"]):,} bytes')
    print('API credentials are blank; provision Terminus before launching.')


if __name__ == '__main__':
    main()
