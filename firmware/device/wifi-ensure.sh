#!/bin/sh
# Safe Touch A (PLATFORM=freescale, ar6000, wlan0) Wi-Fi bring-up helper.
# Uses wifi-ctl.sh which disables AR6000 power-saving and auto-power-cycles
# ar6000 + sdio_wifi_pwr if the radio wedged after an earlier connection.
IFACE="${INTERFACE:-wlan0}"
BASE=/mnt/onboard/.adds/kobodash

if ifconfig "$IFACE" 2>/dev/null | grep -q "inet addr:"; then
    if command -v iwconfig >/dev/null 2>&1; then
        iwconfig "$IFACE" power off 2>/dev/null || true
    fi
    exit 0
fi

if [ -r "$BASE/wifi-ctl.sh" ]; then
    /bin/sh "$BASE/wifi-ctl.sh" on >/dev/null 2>&1 || true
fi
exit 0
