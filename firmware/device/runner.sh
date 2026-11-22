#!/bin/sh
# Standalone supervisor for KoboDash on Kobo Touch A (N905 / PLATFORM=freescale).
# Safely stops Nickel, runs KoboDash out of /tmp, and cleanly restores Nickel on exit.
PATH="/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/bin"
export PATH

BASE=/mnt/onboard/.adds/kobodash
LOCK=/tmp/kobodash.lock
LOG=/tmp/kobodash.log

echo "=== KoboDash starting $(date) ==="

# Capture key environment variables from Nickel before stopping it
NICKEL_PID="$(pidof nickel 2>/dev/null | awk '{print $1}')"
if [ -n "$NICKEL_PID" ] && [ -r "/proc/$NICKEL_PID/environ" ]; then
    for var in DBUS_SESSION_BUS_ADDRESS NICKEL_HOME WIFI_MODULE LANG INTERFACE PLATFORM PRODUCT \
               QT_GSTREAMER_PLAYBIN_AUDIOSINK QT_GSTREAMER_PLAYBIN_AUDIOSINK_DEVICE_PARAMETER; do
        val="$(tr '\0' '\n' < "/proc/$NICKEL_PID/environ" | grep "^${var}=" | head -n 1)"
        if [ -n "$val" ]; then
            export "$val"
        fi
    done
fi

export PLATFORM="${PLATFORM:-freescale}"
export INTERFACE="${INTERFACE:-wlan0}"
export WIFI_MODULE="${WIFI_MODULE:-ar6000}"
export NICKEL_HOME="${NICKEL_HOME:-/mnt/onboard/.kobo}"
export LANG="${LANG:-en_US.UTF-8}"

# Pre-flight full self-test BEFORE stopping Nickel!
# Verifies binary loading, libfbink/libz, fonts, dictionaries, sysinfo, library, and evdev.
# If anything fails or crashes here, abort immediately and leave Nickel completely untouched.
export LD_LIBRARY_PATH="/mnt/onboard/.adds/kobodash/lib:/mnt/onboard/.adds/dashboard/lib"
/tmp/kobodash --base-dir "$BASE" --self-test >/tmp/kobodash-preflight.out 2>&1
PF_RC=$?
if [ "$PF_RC" -ne 0 ]; then
    echo "PREFLIGHT FAILED (exit $PF_RC): $(cat /tmp/kobodash-preflight.out 2>/dev/null || true)"
    echo "Aborting safely without stopping Nickel."
    if [ -d "$BASE" ]; then
        mkdir -p "$BASE/logs"
        tail -n 200 "$LOG" > "$BASE/logs/kobodash.log" 2>/dev/null || true
    fi
    rm -f /tmp/kobodash /tmp/kobodash-runner.sh /tmp/kobodash-preflight.out
    rmdir "$LOCK" 2>/dev/null || true
    exit 1
fi
echo "Pre-flight OK: $(cat /tmp/kobodash-preflight.out 2>/dev/null || true)"
rm -f /tmp/kobodash-preflight.out

# Flush disks before stopping Nickel to protect KoboReader.sqlite
sync

# Stop sickel-launcher.sh AND rcS boot animation (on-animator.sh / pickel) FIRST
# - Stopping sickel-launcher prevents the watchdog from respawning while Nickel is stopped
# - Stopping on-animator.sh / pickel prevents the 3-dot Kobo boot animation from overwriting KoboDash during auto-boot!
killall -q -TERM on-animator.sh pickel 2>/dev/null || true
for p in /proc/[0-9]*; do
    [ -r "$p/cmdline" ] || continue
    cmd="$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null || true)"
    case "$cmd" in
        *sickel-launcher*|*on-animator*)
            kill -TERM "${p#/proc/}" 2>/dev/null || true
            ;;
    esac
done
usleep 150000 2>/dev/null || true

# Stop Nickel stack (leave wpa_supplicant, dhcpcd, and fmon untouched!)
for proc in sickel nickel hindenburg fickel strickel fontickel adobehost foxitpdf on-animator.sh pickel; do
    killall -q -TERM "$proc" 2>/dev/null || true
done

i=0
while pidof nickel >/dev/null 2>&1 || pidof sickel >/dev/null 2>&1; do
    i=$((i + 1))
    if [ "$i" -ge 16 ]; then
        killall -q -KILL sickel nickel 2>/dev/null || true
        break
    fi
    usleep 200000 2>/dev/null || sleep 1
done

# Force-kill any surviving sickel-launcher, on-animator.sh, or pickel (SIGKILL stops sleeping BusyBox subshells immediately)
killall -q -KILL on-animator.sh pickel 2>/dev/null || true
for p in /proc/[0-9]*; do
    [ -r "$p/cmdline" ] || continue
    cmd="$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null || true)"
    case "$cmd" in
        *sickel-launcher*|*on-animator*)
            echo "Stopping boot/watchdog process PID ${p#/proc/}: $cmd"
            kill -KILL "${p#/proc/}" 2>/dev/null || true
            ;;
    esac
done
killall -q -KILL on-animator.sh pickel 2>/dev/null || true
usleep 300000 2>/dev/null || true

rm -f /tmp/nickel-hardware-status

# Show immediate crisp boot splash while Wi-Fi & library initialize
export LD_LIBRARY_PATH="/mnt/onboard/.adds/kobodash/lib:/mnt/onboard/.adds/dashboard/lib"
/tmp/kobodash --base-dir "$BASE" --splash "Starting KoboDash..." "Connecting Wi-Fi & loading library" >/dev/null 2>&1 || true

# Ensure Wi-Fi is up (best effort, safe on Touch A)
/bin/sh "$BASE/wifi-ensure.sh" || true

# Run native KoboDash binary (fully self-contained under .adds/kobodash)
export LD_LIBRARY_PATH="/mnt/onboard/.adds/kobodash/lib:/mnt/onboard/.adds/dashboard/lib"
cd /
/tmp/kobodash --base-dir "$BASE"
RC=$?
echo "=== KoboDash exited with code $RC at $(date) ==="

# Persist recent log safely to onboard storage
if [ -d "$BASE" ]; then
    mkdir -p "$BASE/logs"
    tail -n 200 "$LOG" > "$BASE/logs/kobodash.log" 2>/dev/null || true
fi

rm -f /tmp/kobodash /tmp/kobodash-runner.sh
rmdir "$LOCK" 2>/dev/null || true

if [ "$RC" -eq 42 ]; then
    sync
    reboot
    exit 0
fi

if [ "$RC" -eq 43 ]; then
    sync
    poweroff
    exit 0
fi

# Cleanly restart Stock Kobo UI (Nickel), matching /etc/init.d/rcS verbatim.
# NOTE: Do NOT manually spawn sickel-launcher.sh — Nickel starts sickel itself once initialized!
echo "Restarting Stock Kobo UI (Nickel)..."
if [ -d "$BASE" ]; then
    mkdir -p "$BASE/logs"
    tail -n 200 "$LOG" > "$BASE/logs/kobodash.log" 2>/dev/null || true
fi

cd /
export LD_LIBRARY_PATH="/usr/local/Kobo"
export NICKEL_HOME="/mnt/onboard/.kobo"
export LANG="en_US.UTF-8"

if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ] && [ -x /bin/dbus-daemon ]; then
    DBUS_SESSION_BUS_ADDRESS="$(/bin/dbus-daemon --session --print-address --fork 2>/dev/null || true)"
    export DBUS_SESSION_BUS_ADDRESS
fi

# Unmount SD card cleanly if present so Nickel does not show an unrecognized FS popup
if [ -e /dev/mmcblk1p1 ] && grep -q ' /mnt/sd ' /proc/mounts 2>/dev/null; then
    umount /mnt/sd 2>/dev/null || true
fi

rm -f /tmp/nickel-hardware-status /mnt/onboard/.kobo/sickel_frozen
mkfifo /tmp/nickel-hardware-status
sync

if [ -x /usr/local/Kobo/hindenburg ]; then
    /usr/local/Kobo/hindenburg &
fi
LIBC_FATAL_STDERR_=1 /usr/local/Kobo/nickel -platform kobo -skipFontLoad &

exit 0
