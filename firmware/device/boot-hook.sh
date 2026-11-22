#!/bin/sh
# Minimal non-blocking udev boot hook for KoboDash.
# Immediately backgrounds a detached process so udevd is NEVER blocked.
renice 0 -p $$ 2>/dev/null || true

# Once-per-boot RAM guard so multiple udev events can never double-fire
if ! mkdir /tmp/kobodash-boothook.lock 2>/dev/null; then
    exit 0
fi

# Detach into background; wait for /mnt/onboard to mount, then delegate to autoboot.sh
(
    i=0
    while [ "$i" -lt 45 ] && [ ! -r /mnt/onboard/.adds/kobodash/autoboot.sh ]; do
        sleep 1
        i=$((i + 1))
    done
    if [ -r /mnt/onboard/.adds/kobodash/autoboot.sh ]; then
        exec /bin/sh /mnt/onboard/.adds/kobodash/autoboot.sh
    fi
) </dev/null >/tmp/kobodash-autoboot.log 2>&1 &

exit 0
