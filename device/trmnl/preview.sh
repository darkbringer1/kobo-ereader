#!/bin/sh
set -eu
. /mnt/onboard/.adds/TRMNL/environment.sh
LOCK=/tmp/trmnl-preview.lock
mkdir "$LOCK" 2>/dev/null || exit 1
trap 'rmdir "$LOCK" 2>/dev/null || true' 0
mkdir -p logs
exec >>logs/preview.txt 2>&1
export trmnl_id="$(jq -r '.TrmnlId' config.json)"
export trmnl_token="$(jq -r '.TrmnlToken' config.json)"
export trmnl_apiurl="$(jq -r '.TrmnlApiUrl' config.json)"
export debug_to_screen=0 log_to_server=NONE
export trmnl_loop_ignore_curl_errors=true
export trmnl_image_format=png trmnl_firmware_version=1.6.10
export TRMNL_TEST_ONCE=1
# Preview skips radio/CPU changes, suspension, stock UI termination, and reboot.
# The loop pauses Nickel only for rendering, after both network transfers finish.
"$TRMNL_DIR/deps/bin/busybox" ash ./trmnlloop.sh
