#!/bin/sh
# Read-only hardware inspection; the report itself is written to user storage.
BASE=/mnt/onboard/.adds/dashboard
export LD_LIBRARY_PATH="$BASE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
mkdir -p "$BASE/logs"
REPORT="$BASE/logs/hardware.txt"
{
    date
    uname -a
    printf '\n--- CPU ---\n'
    cat /proc/cpuinfo
    printf '\n--- Memory ---\n'
    cat /proc/meminfo
    printf '\n--- Storage / mounts ---\n'
    df -h
    cat /proc/mounts
    printf '\n--- Platform / revision evidence ---\n'
    printf 'PLATFORM=%s\n' "${PLATFORM:-unset}"
    ls -ld /drivers/* /dev/fb* /dev/mmcblk* /dev/mtd* 2>/dev/null
    printf '\n--- Kernel modules / WiFi drivers ---\n'
    cat /proc/modules
    ls /drivers/freescale/wifi /drivers/ntx508/wifi 2>/dev/null
    printf '\n--- Stock startup ---\n'
    cat /etc/init.d/rcS
    cat /etc/init.d/on-animator.sh 2>/dev/null
    printf '\n--- libc ---\n'
    /lib/libc.so.6 2>&1
    printf '\n--- Network ---\n'
    ifconfig -a
    cat /proc/net/route
    printf '\n--- Framebuffer (read only) ---\n'
    for name in name virtual_size bits_per_pixel rotate stride modes; do
        path="/sys/class/graphics/fb0/$name"
        if [ -f "$path" ]; then printf '%s: ' "$name"; cat "$path"; fi
    done
    "$BASE/bin/fbink" -e
    printf '\n--- Power supply ---\n'
    for path in /sys/class/power_supply/*/uevent; do
        if [ -f "$path" ]; then printf '%s\n' "$path"; cat "$path"; fi
    done
    printf '\n--- Available sleep states (read only) ---\n'
    cat /sys/power/state 2>/dev/null
    printf '\n--- Existing processes / listening sockets ---\n'
    ps w
    netstat -ltn 2>/dev/null
    printf '\n--- Runtime library checks ---\n'
    /lib/ld-linux-armhf.so.3 --list "$BASE/bin/fbink"
    /lib/ld-linux-armhf.so.3 --list "$BASE/bin/dropbearmulti"
    printf '\n--- SSH shell diagnostics ---\n'
    awk -F: '$1 == "root" { print "root uid=" $3 " gid=" $4 " home=" $6 " shell=" $7 }' /etc/passwd
    ls -l /usr/local/niluje/usbnet/bin/ash /usr/local/niluje/usbnet/bin/dashboard-busybox 2>/dev/null
    if [ -x /usr/local/niluje/usbnet/bin/ash ]; then
        /usr/local/niluje/usbnet/bin/ash -c 'echo ASH_EXECUTION_OK'
    fi
    printf '\n--- Recent SSH messages ---\n'
    logread 2>/dev/null | grep -i dropbear | tail -n 30
    printf '\n--- End of report ---\n'
} >"$REPORT" 2>&1
printf 'Report saved: .adds/dashboard/logs/hardware.txt\n'
