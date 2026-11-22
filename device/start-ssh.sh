#!/bin/sh
# Start only when selected in NickelMenu, on the active Wi-Fi IPv4 address.
set -eu
umask 077
BASE=/mnt/onboard/.adds/dashboard
export LD_LIBRARY_PATH="$BASE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
PIDFILE=/var/run/kobo-dashboard-ssh.pid
KEYDIR=/usr/local/niluje/usbnet/etc
AUTH="$KEYDIR/authorized_keys"
HOSTKEY="$KEYDIR/dashboard_ed25519_host_key"
/bin/sh "$BASE/repair-ssh.sh"
test -s "$BASE/authorized_keys"
# NiLuJe's Dropbear has this key path compiled in. Refuse to replace another setup.
if [ -e "$AUTH" ] || [ -L "$AUTH" ]; then
    if [ -L "$AUTH" ] || ! cmp -s "$BASE/authorized_keys" "$AUTH"; then
        echo 'Existing SSH key configuration differs; leaving it untouched.'
        exit 1
    fi
fi
IFACE=$(route -n | awk '$1 == "0.0.0.0" { print $8; exit }')
case "$IFACE" in
    eth*|wlan*) ;;
    *) echo 'Enable Wi-Fi and connect to your trusted LAN first.'; exit 1 ;;
esac
IP=$(ifconfig "$IFACE" | awk '/inet addr:/ { sub("addr:", "", $2); print $2; exit }')
if [ -z "$IP" ]; then echo 'No Wi-Fi IPv4 address found.'; exit 1; fi
if [ -f "$PIDFILE" ]; then
    PID=$(cat "$PIDFILE")
    case "$PID" in ''|*[!0-9]*) echo 'Invalid SSH PID file; inspect it before restarting.'; exit 1 ;; esac
    if kill -0 "$PID" 2>/dev/null; then
        echo "Dashboard SSH already running at $IP port 2222; shell dependency checked."
        exit 0
    fi
    rm "$PIDFILE"
fi
# Check the loader before creating any rootfs files.
/lib/ld-linux-armhf.so.3 --list "$BASE/bin/dropbearmulti" >/dev/null
mkdir -p "$KEYDIR"
if [ ! -e "$AUTH" ]; then
    cp "$BASE/authorized_keys" "$AUTH"
    chmod 600 "$AUTH"
fi
if [ ! -e "$HOSTKEY" ]; then
    "$BASE/bin/dropbearmulti" dropbearkey -t ed25519 -f "$HOSTKEY" >/dev/null 2>&1
fi
if ! awk '$2 == "/dev/pts" && $3 == "devpts" { found = 1 } END { exit !found }' /proc/mounts; then
    mkdir -p /dev/pts
    mount -t devpts devpts /dev/pts
fi
# -s: keys only, -j/-k: disable forwarding, -I: expire idle connections.
# Never use this build's -n option: it disables password checking.
"$BASE/bin/dropbearmulti" dropbear -s -j -k -I 600 -K 30 \
    -p "$IP:2222" -P "$PIDFILE" -r "$HOSTKEY"
echo "SSH ready at $IP port 2222 (key authentication only)."
"$BASE/bin/dropbearmulti" dropbearkey -y -f "$HOSTKEY" | tail -n 1
echo 'This server does not start automatically after reboot.'
