#!/usr/bin/env python3
"""Read macOS USB/disk/network inventory and Kobo identity; write only a local report."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--volume', type=Path, default=Path('/Volumes/KOBOeReader'))
    args = parser.parse_args()
    report = {'time': datetime.now(timezone.utc).isoformat(), 'commands': []}
    commands = [['ioreg', '-p', 'IOUSB', '-w', '0'], ['diskutil', 'list', 'external', 'physical'],
                ['networksetup', '-listallhardwareports']]
    for command in commands:
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
        report['commands'].append({'argv': command, 'exit_code': result.returncode,
                                   'stdout': result.stdout, 'stderr': result.stderr})
    report['mounted_volumes'] = sorted(p.name for p in Path('/Volumes').iterdir())
    version = args.volume / '.kobo/version'
    if version.is_file():
        fields = version.read_text().strip().split(',')
        report['kobo'] = {'serial': '<redacted>', 'kernel_reported': fields[1],
                          'firmware': fields[2], 'model_id': fields[-1]}
        report['root_entries'] = sorted(p.name for p in args.volume.iterdir())
    else:
        report['kobo'] = None
    output = Path(__file__).resolve().parents[1] / 'artifacts'
    output.mkdir(exist_ok=True)
    path = output / ('inspection-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '.json')
    path.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'kobo': report['kobo'], 'mounted_volumes': report['mounted_volumes'],
                      'report': str(path)}, indent=2))


if __name__ == '__main__':
    main()
