#!/usr/bin/env python3
"""Configure the installed client using an ignored local Terminus receipt."""
import argparse
import json
from pathlib import Path
import subprocess
import tarfile
import io

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True)
    parser.add_argument('--port', type=int, default=2222)
    parser.add_argument('--api-url', default='http://192.0.2.10:2300/api')
    args = parser.parse_args()
    receipt = json.loads((ROOT / 'artifacts/server/kobo-device.json').read_text())
    prepared = ROOT / 'output/trmnl-install/TRMNL'
    config = json.loads((prepared / 'config.json').read_text())
    config.update(TrmnlId=receipt['mac_address'], TrmnlToken=receipt['api_key'],
                  TrmnlApiUrl=args.api_url)
    bundle = io.BytesIO()
    with tarfile.open(fileobj=bundle, mode='w') as archive:
        for name, content in [('config.json', (json.dumps(config, indent=2) + '\n').encode()),
                              ('trmnlloop.sh', (prepared / 'trmnlloop.sh').read_bytes()),
                              ('scripts/getrssi.sh', (ROOT / 'device/trmnl/getrssi.sh').read_bytes())]:
            info = tarfile.TarInfo(name + '.new')
            info.size = len(content)
            info.mode = 0o700 if name.endswith('.sh') else 0o600
            archive.addfile(info, io.BytesIO(content))
    # The secret travels on stdin, never in process arguments or terminal output.
    command = '''set -e
cd /mnt/onboard/.adds/TRMNL
test -f config.json
mkdir -p backups
stamp=$(date +%s)
cp config.json backups/config.$stamp.json
cp trmnlloop.sh backups/trmnlloop.$stamp.sh
cp scripts/getrssi.sh backups/getrssi.$stamp.sh
tar xf -
./deps/bin/busybox ash -n trmnlloop.sh.new
./deps/bin/jq -e '.TrmnlId != "" and .TrmnlToken != ""' config.json.new >/dev/null
mv trmnlloop.sh.new trmnlloop.sh
mv scripts/getrssi.sh.new scripts/getrssi.sh
mv config.json.new config.json
echo TRMNL_CONFIGURED
'''
    ssh = ['ssh', '-T', '-p', str(args.port), '-i', str(ROOT / 'artifacts/keys/kobo_dashboard'),
           '-o', 'IdentitiesOnly=yes', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10',
           '-o', 'ServerAliveInterval=5', '-o', 'ServerAliveCountMax=3',
           '-o', f'UserKnownHostsFile={ROOT / "artifacts/keys/known_hosts"}',
           '-o', 'StrictHostKeyChecking=yes', f'root@{args.host}', command]
    subprocess.run(ssh, input=bundle.getvalue(), check=True, timeout=120)


if __name__ == '__main__':
    main()
