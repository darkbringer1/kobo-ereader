#!/usr/bin/env python3
"""Collect four small plugins and render the Berlin example on the host."""
import argparse
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from dashboard.plugins import clock, custom, device, weather
from dashboard.renderer import render


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', help='Kobo Wi-Fi IP for current battery and signal measurements')
    parser.add_argument('--config', type=Path, default=ROOT / 'dashboard/config.json')
    parser.add_argument('--offline', action='store_true', help='Use cached weather and device data, marked on screen')
    parser.add_argument('--output', type=Path, default=ROOT / 'output/berlin-dashboard.png')
    args = parser.parse_args()
    config = json.loads(args.config.read_text())
    cache = ROOT / 'artifacts/dashboard-cache'
    values = {'clock': clock.collect(config), 'custom': custom.collect(config),
              'weather': weather.collect(config, cache / 'weather.json', args.offline),
              'device': device.collect(args.host, cache / 'device.json', args.offline)}
    render(values, config, args.output)
    args.output.with_suffix('.json').write_text(json.dumps(values, indent=2) + '\n')
    print(f'Rendered {args.output}: 600x800, grayscale, 16 levels')
    print(f'Weather: {"cached/unavailable" if values["weather"]["cached"] else "current"}; '
          f'device: {"cached/unavailable" if values["device"]["cached"] else "current"}')


if __name__ == '__main__':
    main()
