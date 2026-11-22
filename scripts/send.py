#!/usr/bin/env python3
"""Transfer a dashboard over SSH without requiring an SFTP/SCP server on the Kobo."""
import argparse
from pathlib import Path
import subprocess

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True, help='Wi-Fi IP shown by Start dashboard SSH')
    parser.add_argument('--port', type=int, default=2222)
    parser.add_argument('--image', type=Path, default=ROOT / 'output/hello-dashboard.png')
    parser.add_argument('--key', type=Path, default=ROOT / 'artifacts/keys/kobo_dashboard')
    args = parser.parse_args()
    if args.host.startswith('-') or any(c.isspace() for c in args.host):
        parser.error('Expected a hostname or IPv4 address')
    with Image.open(args.image) as im:
        im.load()
        if im.size != (600, 800) or im.mode not in ('1', 'L'):
            parser.error('This prototype expects a 600x800 grayscale image')
    command = ['ssh', '-T', '-p', str(args.port), '-i', str(args.key),
               '-o', 'IdentitiesOnly=yes', '-o', f'UserKnownHostsFile={ROOT / "artifacts/keys/known_hosts"}',
               f'root@{args.host}']
    # The command and remote paths are fixed; the host is a separate SSH argument.
    remote = ('cat > /mnt/onboard/.adds/dashboard/hello-dashboard.png.new && '
              'mv /mnt/onboard/.adds/dashboard/hello-dashboard.png.new '
              '/mnt/onboard/.adds/dashboard/hello-dashboard.png')
    with args.image.open('rb') as stream:
        subprocess.run(command + [remote], stdin=stream, check=True)
    subprocess.run(command + ['/bin/sh /mnt/onboard/.adds/dashboard/show.sh'], check=True)
    print('Transfer and framebuffer commands completed. Confirm the physical screen visually.')


if __name__ == '__main__':
    main()
