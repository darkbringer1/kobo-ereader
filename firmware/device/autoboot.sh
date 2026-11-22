#!/bin/sh
# Brick-proof, self-test-gated auto-boot supervisor for KoboDash on Kobo Touch A.
# Runs AT MOST ONCE per cold boot (never fires when returning to Stock Kobo UI).
# BusyBox 1.17 ash safe (zero nested double quotes inside $(...)).
PATH="/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/bin"
export PATH

BASE=/mnt/onboard/.adds/kobodash
LOG_FILE="$BASE/logs/autoboot.log"
PENDING="$BASE/.boot_pending"
DISABLE="$BASE/disable-autoboot"
CFG="$BASE/config.ini"

log_msg() {
    mkdir -p "$BASE/logs" 2>/dev/null || true
    printf '%s: %s\n' "$(date 2>/dev/null || echo boot)" "$1" >> "$LOG_FILE" 2>/dev/null || true
}

# Layer 1: Once-per-boot RAM guard (cleared only on full reboot; preserved when returning to Stock UI!)
if ! mkdir /tmp/kobodash-autoboot.done 2>/dev/null; then
    exit 0
fi

# Layer 2: Wait for Kobo boot to finish mounting /mnt/onboard, dbus, and nickel
tries=0
while [ "$tries" -lt 40 ]; do
    if [ -r "$BASE/bin/kobodash" ] && pidof nickel >/dev/null 2>&1 && pidof dbus-daemon >/dev/null 2>&1; then
        break
    fi
    sleep 1
    tries=$((tries + 1))
done

if [ ! -r "$BASE/bin/kobodash" ] || [ ! -r "$BASE/launch.sh" ]; then
    exit 0
fi

# Wait 3s for Nickel/DBus/hardware status FIFO to settle cleanly
sleep 3

# Layer 3: Honor emergency disable file OR auto_boot = 0 in config.ini
if [ -e "$DISABLE" ]; then
    log_msg "Auto-boot skipped (disable-autoboot file present)."
    exit 0
fi

if [ -r "$CFG" ]; then
    ab_val=$(awk -F '=' '/^[ \t]*auto_boot[ \t]*=/ { gsub(/[ \t\r]/, "", $2); print $2; exit }' "$CFG" 2>/dev/null || true)
    if [ "$ab_val" = "0" ]; then
        log_msg "Auto-boot disabled in config.ini (auto_boot = 0); staying in Stock Kobo UI."
        exit 0
    fi
fi

# Layer 4: Redundant bootloop/crash circuit-breaker!
# If .boot_pending still exists from the previous boot, KoboDash did not survive 15s cleanly -> abort!
if [ -e "$PENDING" ]; then
    log_msg "EMERGENCY BOOTLOOP GUARD: previous auto-boot did not complete cleanly! Disabling auto-boot and staying in Stock Kobo UI."
    rm -f "$PENDING" 2>/dev/null || true
    if [ -w "$CFG" ]; then
        sed 's/^[ \t]*auto_boot[ \t]*=.*/auto_boot = 0/' "$CFG" > /tmp/kd_cfg.tmp 2>/dev/null && mv -f /tmp/kd_cfg.tmp "$CFG" 2>/dev/null || true
        sync
    fi
    exit 0
fi

# Layer 5: Full --self-test verification BEFORE ever stopping Nickel!
export LD_LIBRARY_PATH="$BASE/lib:/mnt/onboard/.adds/dashboard/lib"
cp -f "$BASE/bin/kobodash" /tmp/kd_autoboot_test 2>/dev/null || exit 0
chmod 755 /tmp/kd_autoboot_test
/tmp/kd_autoboot_test --base-dir "$BASE" --self-test >/tmp/kd_autoboot_st.out 2>&1
ST_RC=$?
rm -f /tmp/kd_autoboot_test

ST_OUT=$(cat /tmp/kd_autoboot_st.out 2>/dev/null || true)
rm -f /tmp/kd_autoboot_st.out

if [ "$ST_RC" -ne 0 ] || ! printf '%s' "$ST_OUT" | grep -q 'SELFTEST OK'; then
    log_msg "AUTO-BOOT ABORTED: self-test failed (rc=$ST_RC): $ST_OUT"
    exit 0
fi
log_msg "Self-test passed: $ST_OUT"

# Layer 6: Double-check /mnt/onboard is still mounted (not exported to USB) & arm crash-breaker
if ! grep -q ' /mnt/onboard ' /proc/mounts 2>/dev/null; then
    log_msg "Auto-boot skipped (/mnt/onboard not mounted)."
    exit 0
fi

echo "pending" > "$PENDING" 2>/dev/null || true
sync

log_msg "Launching KoboDash automatically..."
exec /bin/sh "$BASE/launch.sh"
