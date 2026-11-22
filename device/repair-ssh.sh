#!/bin/sh
# NiLuJe's Dropbear forces this BusyBox ash path; do not change /etc/passwd.
set -eu
BASE=/mnt/onboard/.adds/dashboard
BIN=/usr/local/niluje/usbnet/bin
SOURCE="$BASE/bin/busybox"
DEST="$BIN/dashboard-busybox"
SHELL_PATH="$BIN/ash"
export LD_LIBRARY_PATH="$BASE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
test -x "$SOURCE"
"$SOURCE" ash -c 'exit 0'
mkdir -p "$BIN"
if [ ! -e "$SHELL_PATH" ] && [ ! -L "$SHELL_PATH" ]; then
    if [ -e "$DEST" ]; then
        cmp -s "$SOURCE" "$DEST" || { echo 'Existing dashboard shell differs; not replacing it.'; exit 1; }
    else
        cp "$SOURCE" "$DEST"
        chmod 755 "$DEST"
    fi
    ln -s dashboard-busybox "$SHELL_PATH"
fi
"$SHELL_PATH" -c 'printf "SSH shell dependency ready.\n"'
