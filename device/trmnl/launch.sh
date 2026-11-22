#!/bin/sh
set -eu
. /mnt/onboard/.adds/TRMNL/environment.sh
if [ ! -f /mnt/onboard/.adds/TRMNL/dashboard-enabled ]; then
    echo 'Continuous TRMNL mode is not enabled; complete the preview first.'
    exit 1
fi
mkdir -p logs
exec >>logs/dashboard.txt 2>&1
exec "$TRMNL_DIR/deps/bin/busybox" ash ./trmnl.sh
