#!/bin/sh
# 100% Read-Only KoboDash diagnostic self-test.
# Tests the actual kobodash binary WITHOUT stopping Nickel or writing /dev/fb0.
PATH="/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/bin"
export PATH

BASE=/mnt/onboard/.adds/kobodash
LOG="$BASE/logs/selftest.log"
TMP_BIN=/tmp/kd_selftest
TMP_OUT=/tmp/kd_selftest.out

mkdir -p "$BASE/logs" 2>/dev/null || true
export LD_LIBRARY_PATH="$BASE/lib:/mnt/onboard/.adds/dashboard/lib"

cp -f "$BASE/bin/kobodash" "$TMP_BIN" 2>"$TMP_OUT"
CP_RC=$?
if [ "$CP_RC" -ne 0 ]; then
    printf 'Copy failed (rc=%s): ' "$CP_RC"
    cat "$TMP_OUT" 2>/dev/null || true
    rm -f "$TMP_OUT"
    exit 0
fi

chmod 755 "$TMP_BIN"
"$TMP_BIN" --base-dir "$BASE" --self-test >"$TMP_OUT" 2>&1
RC=$?
rm -f "$TMP_BIN"

{
    echo "=== KoboDash Self-Test $(date) ==="
    echo "FW: $(cat /mnt/onboard/.kobo/version 2>/dev/null || echo unknown)"
    echo "kobodash --self-test (rc=$RC):"
    cat "$TMP_OUT" 2>/dev/null || true
} > "$LOG" 2>&1

printf 'Exit %s: ' "$RC"
cat "$TMP_OUT" 2>/dev/null || true
rm -f "$TMP_OUT"
exit 0
