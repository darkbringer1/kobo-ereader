#!/usr/bin/env python3
"""Render and publish the example to the existing local Terminus playlist."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def publish(host, offline=False):
    render = [sys.executable, str(ROOT / 'scripts/dashboard.py')]
    if host: render += ['--host', host]
    if offline: render += ['--offline']
    subprocess.run(render, check=True)
    receipt = json.loads((ROOT / 'artifacts/server/kobo-device.json').read_text())
    compose = ['docker', 'compose', '-p', 'kobo-terminus', '--env-file', str(ROOT / 'server/.env'),
               '-f', str(ROOT / 'server/compose.yml')]
    subprocess.run(compose + ['cp', str(ROOT / 'output/berlin-dashboard.png'), 'web:/tmp/kobo-dashboard.png'], check=True)
    subprocess.run(compose + ['cp', str(ROOT / 'server/publish_screen.rb'), 'web:/tmp/publish_screen.rb'], check=True)
    subprocess.run(compose + ['exec', '-T', '-e', f'KOBO_DEVICE_ID={receipt["device_id"]}',
                             'web', 'bundle', 'exec', 'ruby', '/tmp/publish_screen.rb'], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', help='Kobo IP for battery and Wi-Fi telemetry')
    parser.add_argument('--offline', action='store_true')
    parser.add_argument('--watch', action='store_true', help='Keep this host process running to republish every 15 minutes')
    args = parser.parse_args()
    interval = int(json.loads((ROOT / 'dashboard/config.json').read_text())['refresh_seconds'])
    if interval < 60:
        parser.error('Use a refresh interval of at least 60 seconds')
    while True:
        try:
            publish(args.host, args.offline)
        except (OSError, subprocess.SubprocessError) as error:
            if not args.watch: raise
            print(f'Update failed; preserving the last published image: {error}', file=sys.stderr, flush=True)
        if not args.watch: break
        print(f'Next host render in {interval} seconds. Ctrl-C stops the publisher.', flush=True)
        time.sleep(interval)


if __name__ == '__main__':
    main()
