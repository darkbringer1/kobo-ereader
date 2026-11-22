#!/bin/sh
# Safe Wi-Fi (with full ar6000 + sdio_wifi_pwr hardware power-cycle) & SD card helper
# for Kobo Touch A (N905 / PLATFORM=freescale, ar6000, wlan0).
# NEVER invokes /dev/ntx_io (not a character device on Touch A).
# BusyBox 1.17 ash safe (zero nested double quotes inside $(...)).
PATH="/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/bin"
export PATH

IFACE="${INTERFACE:-wlan0}"
KMOD_DIR="/drivers/${PLATFORM:-freescale}/wifi"
WMOD="${WIFI_MODULE:-ar6000}"
WPA_CONF="/etc/wpa_supplicant/wpa_supplicant.conf"
WPA_CTRL="/var/run/wpa_supplicant"

msleep_safe() {
    usleep "$1" 2>/dev/null || sleep 1
}

flush_net_state() {
    wpa_cli -i "$IFACE" disconnect >/dev/null 2>&1 || true
    if [ -x /sbin/dhcpcd ]; then
        dhcpcd -k "$IFACE" >/dev/null 2>&1 || true
    fi
    killall -q -TERM dhcpcd udhcpc wpa_supplicant 2>/dev/null || true
    msleep_safe 250000
    killall -q -KILL dhcpcd udhcpc wpa_supplicant 2>/dev/null || true
    rm -rf "$WPA_CTRL/$IFACE" /var/run/dhcpcd* /var/lib/dhcpcd/dhcpcd-"$IFACE".lease 2>/dev/null || true
    ifconfig "$IFACE" 0.0.0.0 down 2>/dev/null || true
    while route del default dev "$IFACE" 2>/dev/null; do :; done
}

unload_wifi_modules() {
    if grep -q "^${WMOD} " /proc/modules 2>/dev/null; then
        rmmod "$WMOD" 2>/dev/null || true
        msleep_safe 350000
    fi
    if grep -q "^sdio_wifi_pwr " /proc/modules 2>/dev/null; then
        rmmod sdio_wifi_pwr 2>/dev/null || true
        msleep_safe 600000
    fi
}

load_wifi_modules() {
    if [ -e "${KMOD_DIR}/sdio_wifi_pwr.ko" ] && ! grep -q "^sdio_wifi_pwr " /proc/modules 2>/dev/null; then
        insmod "${KMOD_DIR}/sdio_wifi_pwr.ko" 2>/dev/null || true
        msleep_safe 500000
    fi
    if ! grep -q "^${WMOD} " /proc/modules 2>/dev/null; then
        if [ -e "${KMOD_DIR}/${WMOD}.ko" ]; then
            insmod "${KMOD_DIR}/${WMOD}.ko" 2>/dev/null || true
        fi
    fi

    # Wait up to 4s for wlan0 netdev to register on SDIO bus
    w=0
    while [ "$w" -lt 20 ]; do
        if [ -d "/sys/class/net/$IFACE" ]; then
            break
        fi
        msleep_safe 200000
        w=$((w + 1))
    done

    ifconfig "$IFACE" up 2>/dev/null || true
    # Disable aggressive AR6000 802.11 power-saving (fixes iPhone Hotspot & router drops!)
    if command -v iwconfig >/dev/null 2>&1; then
        iwconfig "$IFACE" power off 2>/dev/null || true
    fi
}

ensure_wpa_running() {
    if [ ! -f "$WPA_CONF" ]; then
        mkdir -p /etc/wpa_supplicant
        printf 'ctrl_interface=%s\nupdate_config=1\n' "$WPA_CTRL" > "$WPA_CONF"
    fi
    mkdir -p "$WPA_CTRL"
    if ! pidof wpa_supplicant >/dev/null 2>&1; then
        rm -f "$WPA_CTRL/$IFACE" 2>/dev/null || true
        wpa_supplicant -D wext -s -i "$IFACE" -c "$WPA_CONF" -C "$WPA_CTRL" -B 2>/dev/null || true
        msleep_safe 500000
    fi
}

ensure_modules_and_wpa() {
    load_wifi_modules
    ensure_wpa_running
}

# Full hardware power-cycle of the AR6000 Wi-Fi chip (unwedges stuck radio without reboot!)
hard_reset_radio() {
    flush_net_state
    unload_wifi_modules
    load_wifi_modules
    ensure_wpa_running
}

get_ip() {
    ifconfig "$IFACE" 2>/dev/null | awk '/inet addr:/ { sub("addr:", "", $2); print $2; exit }'
}

obtain_ip() {
    fast_mode="${1:-0}"
    if [ -x /sbin/dhcpcd ]; then
        dhcpcd -k "$IFACE" >/dev/null 2>&1 || true
    fi
    killall -q -TERM dhcpcd udhcpc 2>/dev/null || true
    msleep_safe 150000
    killall -q -KILL dhcpcd udhcpc 2>/dev/null || true
    rm -f /var/run/dhcpcd* /var/lib/dhcpcd/dhcpcd-"$IFACE".lease 2>/dev/null || true
    ifconfig "$IFACE" 0.0.0.0 2>/dev/null || true
    while route del default dev "$IFACE" 2>/dev/null; do :; done

    if command -v iwconfig >/dev/null 2>&1; then
        iwconfig "$IFACE" power off 2>/dev/null || true
    fi

    ip=""
    if [ "$fast_mode" = "1" ]; then
        if [ -x /sbin/udhcpc ] && [ -x /etc/udhcpc.d/default.script ]; then
            udhcpc -i "$IFACE" -s /etc/udhcpc.d/default.script -n -q -t 3 -T 1 >/dev/null 2>&1 || true
            ip=$(get_ip)
        fi
        if [ -z "$ip" ] && [ -x /sbin/dhcpcd ]; then
            dhcpcd -t 4 -w "$IFACE" >/dev/null 2>&1 || true
            ip=$(get_ip)
        fi
    else
        if [ -x /sbin/udhcpc ] && [ -x /etc/udhcpc.d/default.script ]; then
            udhcpc -i "$IFACE" -s /etc/udhcpc.d/default.script -n -q -t 6 -T 2 >/dev/null 2>&1 || true
            ip=$(get_ip)
        fi
        if [ -z "$ip" ] && [ -x /sbin/dhcpcd ]; then
            dhcpcd -t 12 -w "$IFACE" >/dev/null 2>&1 || true
            ip=$(get_ip)
        fi
        if [ -z "$ip" ] && [ -x /sbin/udhcpc ]; then
            udhcpc -i "$IFACE" -s /etc/udhcpc.d/default.script -n -q -t 6 -T 2 >/dev/null 2>&1 || true
            ip=$(get_ip)
        fi
    fi

    if [ -n "$ip" ] && ! grep -q '^nameserver' /etc/resolv.conf 2>/dev/null; then
        gw=$(route -n 2>/dev/null | awk '$1=="0.0.0.0" { print $2; exit }')
        if [ -n "$gw" ]; then
            echo "nameserver $gw" >> /etc/resolv.conf 2>/dev/null || true
        fi
        echo "nameserver 1.1.1.1" >> /etc/resolv.conf 2>/dev/null || true
    fi
    printf '%s' "$ip"
}

wait_associated() {
    max_tries="${1:-20}"
    tries=0
    while [ "$tries" -lt "$max_tries" ]; do
        if wpa_cli -i "$IFACE" status 2>/dev/null | grep -q "wpa_state=COMPLETED"; then
            return 0
        fi
        msleep_safe 500000
        tries=$((tries + 1))
    done
    return 1
}

select_and_associate() {
    target_ssid="$1"
    max_tries="${2:-20}"
    wpa_cli -i "$IFACE" reconfigure >/dev/null 2>&1 || true
    msleep_safe 400000
    if [ -n "$target_ssid" ]; then
        net_id=$(wpa_cli -i "$IFACE" list_networks 2>/dev/null | awk -F '\t' -v s="$target_ssid" 'NR>1 && $2==s { print $1; exit }')
        if [ -n "$net_id" ]; then
            wpa_cli -i "$IFACE" enable_network "$net_id" >/dev/null 2>&1 || true
            wpa_cli -i "$IFACE" select_network "$net_id" >/dev/null 2>&1 || true
        else
            wpa_cli -i "$IFACE" reconnect >/dev/null 2>&1 || true
        fi
    else
        wpa_cli -i "$IFACE" enable_network all >/dev/null 2>&1 || true
        wpa_cli -i "$IFACE" reconnect >/dev/null 2>&1 || true
    fi
    wait_associated "$max_tries"
}

# Safely strip any existing network={...} block matching $1 (exact SSID) from $WPA_CONF
strip_ssid_from_conf() {
    target_ssid="$1"
    [ -r "$WPA_CONF" ] || return 0
    if [ ! -f "${WPA_CONF}.kobodash.bak" ]; then
        cp -f "$WPA_CONF" "${WPA_CONF}.kobodash.bak" 2>/dev/null || true
    fi
    awk -v target="$target_ssid" '
        BEGIN { in_net = 0; buf = ""; matched = 0 }
        /^[ \t]*network[ \t]*=[ \t]*\{/ {
            in_net = 1; buf = $0 "\n"; matched = 0; next
        }
        in_net {
            buf = buf $0 "\n"
            line = $0
            if (line ~ /^[ \t]*ssid[ \t]*=/) {
                sub(/^[ \t]*ssid[ \t]*=[ \t]*"/, "", line)
                sub(/"[ \t]*$/, "", line)
                if (line == target) matched = 1
            }
            if ($0 ~ /^[ \t]*\}/) {
                if (!matched) printf "%s", buf
                in_net = 0; buf = ""; matched = 0
            }
            next
        }
        { print }
        END { if (in_net && !matched) printf "%s", buf }
    ' "$WPA_CONF" > /tmp/kd_wpa.tmp && mv -f /tmp/kd_wpa.tmp "$WPA_CONF"
}

do_scan_once() {
    wpa_cli -i "$IFACE" scan >/dev/null 2>&1 || true
    sleep 2
    wpa_cli -i "$IFACE" scan_results > /tmp/kd_scan.tmp 2>/dev/null || true
    lines=$(wc -l < /tmp/kd_scan.tmp 2>/dev/null || echo 0)
    if [ "$lines" -gt 1 ]; then
        cat /tmp/kd_scan.tmp
        rm -f /tmp/kd_scan.tmp
        return 0
    fi
    rm -f /tmp/kd_scan.tmp
    if command -v iwlist >/dev/null 2>&1; then
        iwlist "$IFACE" scan 2>/dev/null | awk '
            /Cell / { if (ssid != "") printf "00:00:00:00:00:00\t2412\t%s\t%s\t%s\n", sig, enc, ssid; ssid=""; sig="-70"; enc="[OPEN]" }
            /Signal level/ { match($0, /-[0-9]+/); if (RSTART>0) sig=substr($0, RSTART, RLENGTH) }
            /Encryption key:on/ { enc="[WPA2]" }
            /ESSID:/ { sub(/.*ESSID:"/, ""); sub(/".*/, ""); ssid=$0 }
            END { if (ssid != "") printf "00:00:00:00:00:00\t2412\t%s\t%s\t%s\n", sig, enc, ssid }
        ' > /tmp/kd_scan.tmp
        if [ -s /tmp/kd_scan.tmp ]; then
            cat /tmp/kd_scan.tmp
            rm -f /tmp/kd_scan.tmp
            return 0
        fi
        rm -f /tmp/kd_scan.tmp
    fi
    return 1
}

cmd="${1:-}"
case "$cmd" in
    on)
        ensure_modules_and_wpa
        if ! select_and_associate "" 16; then
            hard_reset_radio
            select_and_associate "" 16 || true
        fi
        ip=$(obtain_ip 0)
        if [ -z "$ip" ]; then
            hard_reset_radio
            if select_and_associate "" 16; then
                ip=$(obtain_ip 0)
            fi
        fi
        if [ -n "$ip" ]; then
            echo "OK Connected ($ip)"
        else
            echo "OK Wi-Fi reset (no IP yet)"
        fi
        ;;

    reset)
        hard_reset_radio
        if select_and_associate "" 8; then
            ip=$(obtain_ip 1)
            if [ -n "$ip" ]; then
                echo "OK Wi-Fi chip reset: $ip"
            else
                echo "OK Wi-Fi chip reset (open Wi-Fi to reconnect)"
            fi
        else
            echo "OK Wi-Fi chip reset (ready to join)"
        fi
        ;;

    off)
        flush_net_state
        unload_wifi_modules
        echo "OK Wi-Fi powered off"
        ;;

    scan)
        ensure_modules_and_wpa
        if ! do_scan_once; then
            # Classic wedged ar6000 returns empty scan; power-cycle chip and retry!
            hard_reset_radio
            do_scan_once || true
        fi
        ;;

    saved)
        if [ -r "$WPA_CONF" ]; then
            awk '
                /^[ \t]*ssid[ \t]*=/ {
                    line = $0
                    sub(/^[ \t]*ssid[ \t]*=[ \t]*"/, "", line)
                    sub(/"[ \t]*$/, "", line)
                    if (line != "") print line
                }
            ' "$WPA_CONF"
        fi
        ;;

    connect)
        ssid="${2:-}"
        psk="${3:-}"
        if [ -z "$ssid" ]; then
            echo "ERR Empty SSID"
            exit 1
        fi

        # Always add scan_ssid=1 so iPhone Personal Hotspot (which pauses beacons) connects reliably!
        esc_ssid=$(printf '%s' "$ssid" | sed 's/\\/\\\\/g; s/"/\\"/g')
        strip_ssid_from_conf "$esc_ssid"

        if [ -z "$psk" ]; then
            printf '\nnetwork={\n\tssid="%s"\n\tscan_ssid=1\n\tkey_mgmt=NONE\n}\n' "$esc_ssid" >> "$WPA_CONF"
        else
            esc_psk=$(printf '%s' "$psk" | sed 's/\\/\\\\/g; s/"/\\"/g')
            printf '\nnetwork={\n\tssid="%s"\n\tscan_ssid=1\n\tpsk="%s"\n}\n' "$esc_ssid" "$esc_psk" >> "$WPA_CONF"
        fi
        sync

        # Attempt 1: try associating cleanly
        ensure_modules_and_wpa
        ip=""
        if select_and_associate "$ssid" 16; then
            ip=$(obtain_ip 0)
        fi

        # Attempt 2: Full hardware ar6000 + sdio_wifi_pwr power-cycle (solves 2nd-connect wedge!)
        if [ -z "$ip" ]; then
            hard_reset_radio
            if select_and_associate "$ssid" 20; then
                ip=$(obtain_ip 0)
            fi
        fi

        if [ -n "$ip" ]; then
            echo "OK Connected: $ip"
        else
            echo "ERR Failed (check 2.4GHz / password)"
            exit 1
        fi
        ;;

    forget)
        ssid="${2:-}"
        if [ -z "$ssid" ]; then
            echo "ERR Empty SSID"
            exit 1
        fi
        esc_ssid=$(printf '%s' "$ssid" | sed 's/\\/\\\\/g; s/"/\\"/g')
        strip_ssid_from_conf "$esc_ssid"
        sync
        wpa_cli -i "$IFACE" reconfigure >/dev/null 2>&1 || true
        echo "OK Forgot $ssid"
        ;;

    sd-mount)
        if grep -q ' /mnt/sd ' /proc/mounts 2>/dev/null; then
            echo "OK SD already mounted"
        elif [ -e /dev/mmcblk1p1 ]; then
            mkdir -p /mnt/sd
            if mount -t vfat -o rw,noatime,utf8 /dev/mmcblk1p1 /mnt/sd 2>/dev/null || mount /dev/mmcblk1p1 /mnt/sd 2>/dev/null; then
                echo "OK SD mounted"
            else
                echo "ERR Could not mount SD"
                exit 1
            fi
        elif [ -e /dev/mmcblk1 ]; then
            mkdir -p /mnt/sd
            if mount /dev/mmcblk1 /mnt/sd 2>/dev/null; then
                echo "OK SD mounted"
            else
                echo "ERR Could not mount SD"
                exit 1
            fi
        else
            echo "ERR No microSD card detected"
            exit 1
        fi
        ;;

    sd-unmount)
        if grep -q ' /mnt/sd ' /proc/mounts 2>/dev/null; then
            sync
            if umount /mnt/sd 2>/dev/null; then
                echo "OK SD safely unmounted"
            else
                echo "ERR SD busy"
                exit 1
            fi
        else
            echo "OK SD not mounted"
        fi
        ;;

    *)
        echo "Usage: wifi-ctl.sh {on|off|reset|scan|saved|connect <ssid> [psk]|forget <ssid>|sd-mount|sd-unmount}"
        exit 1
        ;;
esac
exit 0
