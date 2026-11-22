#!/usr/bin/env python3
"""Manage this project's isolated self-hosted Terminus stack; retain data on stop."""
import argparse
from pathlib import Path
import secrets
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / 'server'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['setup', 'start', 'stop', 'status', 'logs'])
    parser.add_argument('--host', help='Mac LAN address advertised to the Kobo')
    args = parser.parse_args()
    environment = SERVER / '.env'
    if not environment.exists():
        host = args.host
        if not host:
            result = subprocess.run(['ipconfig', 'getifaddr', 'en0'], text=True, capture_output=True, check=True)
            host = result.stdout.strip()
        if not host or any(c not in '0123456789.' for c in host):
            parser.error('Specify the Mac IPv4 address with --host')
        content = (f'API_URI=http://{host}:2300\nSERVER_BIND_ADDRESS=0.0.0.0\n'
                   f'APP_SECRET={secrets.token_hex(48)}\n'
                   f'DATABASE_PASSWORD={secrets.token_hex(24)}\n'
                   f'KEYVALUE_PASSWORD={secrets.token_hex(24)}\n')
        environment.touch(mode=0o600, exist_ok=False)
        environment.write_text(content)
        print('Created private server/.env; credentials are preserved across restarts.', flush=True)
    command = ['docker', 'compose', '-p', 'kobo-terminus', '--env-file', str(environment),
               '-f', str(SERVER / 'compose.yml')]
    if args.action == 'setup':
        subprocess.run(command + ['config', '--quiet'], check=True)
        print('Compose configuration validated; no services started.')
    elif args.action == 'start':
        subprocess.run(command + ['up', '-d', '--pull', 'missing'], check=True)
    elif args.action == 'stop':
        subprocess.run(command + ['stop'], check=True)
    elif args.action == 'status':
        subprocess.run(command + ['ps'], check=True)
    else:
        result = subprocess.run(command + ['logs', '--no-color', '--tail', '60'], capture_output=True, text=True)
        output = result.stdout + result.stderr
        for line in environment.read_text().splitlines():
            key, _, value = line.partition('=')
            if ('SECRET' in key or 'PASSWORD' in key) and value:
                output = output.replace(value, '<redacted>')
        print(output)


if __name__ == '__main__':
    main()
