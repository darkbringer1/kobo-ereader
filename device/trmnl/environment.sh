#!/bin/sh
TRMNL_DIR=/mnt/onboard/.adds/TRMNL
export TRMNL_DIR
export PATH="$TRMNL_DIR/deps/bin:$TRMNL_DIR/bin/fbink:/usr/local/Kobo:/sbin:/usr/sbin:/bin:/usr/bin"
export LD_LIBRARY_PATH="$TRMNL_DIR/deps/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LC_ALL=C
export PLATFORM=freescale
export INTERFACE=wlan0
cd "$TRMNL_DIR" || exit 1
if ! jq -e '.TrmnlId != "" and .TrmnlToken != "" and (.TrmnlApiUrl | startswith("http"))' config.json >/dev/null; then
    echo 'TRMNL server is not provisioned yet; leaving the Kobo UI running.'
    exit 1
fi
