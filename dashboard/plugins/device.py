"""Read only battery and signal values over the existing key-only SSH channel."""
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
COMMAND = r'''printf 'BATTERY='; cat /sys/class/power_supply/mc13892_bat/capacity
printf 'STATUS='; cat /sys/class/power_supply/mc13892_bat/status
printf 'RSSI='; iwconfig wlan0 2>/dev/null | sed -n 's/.*Signal level[:=] *\(-*[0-9]*\).*/\1/p'
'''


def parse(output):
    values = dict(line.split('=', 1) for line in output.splitlines() if '=' in line)
    battery = int(values['BATTERY'])
    if not 0 <= battery <= 100:
        raise ValueError('Invalid battery percentage')
    raw = values.get('RSSI', '')
    rssi = int(raw) if re.fullmatch(r'-\d+', raw) and -120 <= int(raw) < 0 else None
    return {'battery': battery, 'status': values.get('STATUS', 'Unknown'), 'rssi': rssi,
            'measured_at': datetime.now(timezone.utc).isoformat(), 'cached': False}


def collect(host, cache, offline=False):
    cache = Path(cache)
    try:
        if offline or not host:
            raise ValueError('No live device requested')
        command = ['ssh', '-T', '-p', '2222', '-i', str(ROOT / 'artifacts/keys/kobo_dashboard'),
                   '-o', 'IdentitiesOnly=yes', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8',
                   '-o', f'UserKnownHostsFile={ROOT / "artifacts/keys/known_hosts"}',
                   '-o', 'StrictHostKeyChecking=yes', f'root@{host}', COMMAND]
        result = subprocess.run(command, capture_output=True, text=True, timeout=20, check=True)
        values = parse(result.stdout)
        values['host'] = host
        cache.parent.mkdir(parents=True, exist_ok=True)
        temporary = cache.with_suffix('.new')
        temporary.write_text(json.dumps(values, indent=2) + '\n')
        temporary.replace(cache)
        return values
    except (OSError, ValueError, KeyError, subprocess.SubprocessError):
        if cache.exists():
            values = json.loads(cache.read_text())
            if not host or host == values.get('host'):
                return dict(values, cached=True)
        return {'battery': None, 'rssi': None, 'cached': True}
