#!/bin/sh
# Touch A needs an explicit interface; bare iwconfig can omit its signal.
signal=$(iwconfig "${INTERFACE:-wlan0}" 2>/dev/null | sed -n 's/.*Signal level[:=] *\(-*[0-9]*\).*/\1/p')
case "$signal" in
    -[0-9]|-[0-9][0-9]|-[0-9][0-9][0-9]) printf '%s\n' "$signal" ;;
    *) exit 1 ;;
esac
