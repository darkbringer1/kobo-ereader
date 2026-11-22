# Recovery, Failsafes & Uninstall Guide

KoboDash never modifies the Kobo bootloader, kernel, partitions, or `/etc/init.d/rcS`. Stock Kobo UI (Nickel) remains completely intact and usable at any time.

## Returning to Stock Kobo UI

- **From anywhere inside KoboDash**: Hold the physical **Home** button for **2 seconds** (works from Clock, Dashboard, Book Reader, Settings, Touch-Lock, Alert Overlay, and Keyboard).
- **From Settings**: Tap the gear icon → **Return to Stock Kobo UI** (or `Settings → DEVICE → Stock Kobo UI`).
- Because auto-boot uses a once-per-boot RAM lock (`/tmp/kobodash-autoboot.done`), returning to Stock UI never re-triggers auto-boot until the next full cold reboot.

## Built-In Auto-Boot Failsafes

Auto-boot (`firmware/device/autoboot.sh`) includes multiple redundant safety layers:
1. **Pre-flight `--self-test` gate**: Before stopping Nickel, `autoboot.sh` runs `kobodash --self-test` verifying binary loading, `libfbink`, `libz`, fonts, dictionaries, evdev input, and library access. If anything fails, it aborts and leaves Stock UI running untouched.
2. **Crash / Bootloop circuit-breaker**: Arms `.adds/kobodash/.boot_pending` before launching and clears it after 15 seconds of healthy runtime (or clean exit). If a previous boot crashed before reaching 15 seconds, the next boot automatically sets `auto_boot = 0` and stays in Stock UI.
3. **USB disable switch**: Creating an empty file `.adds/kobodash/disable-autoboot` (or setting `auto_boot = 0` in `.adds/kobodash/config.ini`) immediately disables auto-boot.

> **Tip**: Never hold the physical Home button while rebooting — holding Home during Kobo boot triggers Kobo's built-in factory reset sequence.

## Diagnostics & Logs

Over USB (in Stock UI) or SSH:
- `.adds/kobodash/logs/autoboot.log` — auto-boot self-test & launch trace
- `.adds/kobodash/logs/kobodash.log` — KoboDash runtime supervisor log
- NickelMenu → **KoboDash Self-Test** — runs a live non-destructive check without leaving Stock UI

## Full Uninstall

1. Enter **Stock Kobo UI** and connect USB.
2. Delete `.adds/kobodash/` and `.adds/nm/kobodash`.
3. *(Optional, if auto-boot udev hook was installed)* Remove `/etc/udev/rules.d/99-kobodash.rules` and `/usr/local/kobodash/boot-hook.sh` via SSH (or simply leave them — they are a clean no-op once `.adds/kobodash/autoboot.sh` is removed).
4. *(Optional)* To remove NickelMenu itself, follow [NickelMenu's official uninstall instructions](https://pgaskin.net/NickelMenu/) (create `.adds/nm/uninstall` and reboot).
5. Always leave `ExcludeSyncFolders` in `.kobo/Kobo/Kobo eReader.conf` intact if you keep any other `.adds/` tools so Nickel does not index hidden directories as books.
