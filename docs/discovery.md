# Kobo Touch A (N905) Hardware & Runtime Discoveries

This document records verified hardware, kernel, toolchain, and runtime findings on the original **Kobo Touch A (N905 / Trilogy / Mark 3)**.

## Confirmed Hardware Profile

- **Model**: Original Kobo Touch A (`N905`), `PLATFORM=freescale`, CPU `Freescale MX50 ARM2 Board` (i.MX508, ARMv7 Cortex-A8, VFP/NEON, 800 MHz).
  - *Note*: `.kobo/version` exports model UUID `...310`, which is shared between Touch A and Touch B. Always confirm `PLATFORM=freescale` at runtime; Touch A uses soldered eMMC rather than an internal microSD slot.
- **Linux / libc**: Kernel `2.6.35.3` (`arm-linux-gnueabihf`), **glibc 2.19**.
- **Memory**: `MemTotal=256124 kB` (~250 MiB usable RAM), no swap.
- **Display**: `mxc_epdc_fb`, 600×800 portrait, 16bpp framebuffer (line stride 1216 bytes), 167 dpi.
- **Touchscreen**: Neonode IR touch (`evdev`), portrait transform `touch_swap_xy=1`, `touch_mirror_x=1`, `touch_mirror_y=0`.
- **Wi-Fi**: `wlan0` (`ar6000` driver at `/drivers/freescale/wifi/ar6000.ko` + `sdio_wifi_pwr.ko`), 2.4 GHz.
- **Battery**: `/sys/class/power_supply/mc13892_bat/uevent`.
- **Power / RTC**: `/sys/power/state` supports `standby mem`, `/sys/power/state-extended` prepares EPDC/touch drivers for suspend, and `/sys/class/rtc/rtc0/wakealarm` wakes from `mem` suspend.
- **`/dev/ntx_io`**: Zero-length regular file on Touch A (not a character device); never invoke `ntx_io` ioctls on Touch A.

## Critical Touch A Engineering Findings

### 1. Cross-Compiling for Kobo `glibc 2.19` with Zig / LLD (`Zig #18773`)
Default LLD linker output places `.got` inside a `PT_GNU_RELRO` segment that causes a pre-`main()` `SIGSEGV` inside Kobo's `ld-linux-armhf.so.3` (glibc 2.19).
- **Fix**: Pass `-Wl,-z,norelro -Wl,-z,lazy` when linking ARM binaries with `zig cc --target=arm-linux-gnueabihf.2.19 -mcpu=cortex_a8`.

### 2. E-Ink Waveforms on `mxc_epdc_fb`
- `WFM_DU` (1-bit fast non-flashing, ~260 ms) is ideal for clock digit updates and UI toggles, but breaks anti-aliased serif glyphs during book reading.
- `WFM_GC16` non-flashing (`KD_REFRESH_GRAY_PARTIAL`) renders crisp 16-level grayscale book pages without flashing; periodic full `WFM_GC16` flashes clear residual ghosting.
- Avoid `WFM_GL16` and `WFM_A2` on Touch A.

### 3. BusyBox 1.17 `ash` Quoting Pitfall
Kobo's stock BusyBox 1.17 `ash` fails with exit status 2 on nested double quotes inside `"$(... "$VAR" ...)"`. Always write `var=$(cmd "$arg")` without outer quotes on variable assignments.

### 4. `ar6000` Wi-Fi Wedge After Reconnect / Personal Hotspot
Reconnecting to Wi-Fi or an iPhone Personal Hotspot a second time wedges `ar6000` unless the radio is truly power-cycled.
- **Fix** (`firmware/device/wifi-ctl.sh`): Full `rmmod ar6000` + `rmmod sdio_wifi_pwr` → `insmod` hardware power-cycle, stale `dhcpcd` lease/PID/route flush, `iwconfig wlan0 power off`, and `scan_ssid=1` in `wpa_supplicant.conf`.

### 5. Cold Auto-Boot Without Modifying `/etc/init.d/rcS`
- Never patch `/etc/init.d/rcS`. Instead, install a 2-file udev rule (`KERNEL=="loop0"`, matching the NiLuJe KoboStuff pattern) that launches a detached supervisor (`autoboot.sh`).
- **Boot animation race**: `/etc/init.d/rcS` launches `/etc/init.d/on-animator.sh` (looping `/usr/local/Kobo/pickel showpic` every ~250 ms) until Nickel finishes initializing. If KoboDash stops Nickel early during auto-boot, `runner.sh` **must** also stop `on-animator.sh` and `pickel` (`killall -KILL on-animator.sh pickel` + `/proc/*/cmdline` scan) before drawing to `/dev/fb0`.
- **Watchdog safety**: Stop `sickel-launcher.sh` before `sickel`/`nickel`, and do not manually launch `sickel-launcher.sh` when restarting Nickel (Nickel spawns it once ready).

### 6. TLS 1.2 / 1.3 on Linux `2.6.35.3`
Linux `2.6.35.3` predates the `getrandom(2)` syscall (added in Linux 3.17). Static **mbedTLS 3.6.x** cleanly handles `ENOSYS` on `getrandom` and falls back to `/dev/urandom`, linking statically into KoboDash (~492 KB stripped total binary with full TLS 1.2 + TLS 1.3 + X.509 PEM verification). Because cold boots without battery can reset the RTC to 1970, KoboDash skips only certificate date checks when `time(NULL) < 2024` (still verifying signature chain & hostname) so the first HTTPS response `Date` header or NTP sync can bootstrap the clock.

### 7. Safe Deep Sleep (`suspend-to-RAM`)
Before entering `mem` suspend on Touch A:
1. Wait for EPDC refresh completion (`display_wait_complete()`).
2. Unload `ar6000` + `sdio_wifi_pwr` (`wifi-ctl.sh off`) so Wi-Fi neither blocks suspend nor wedges on resume.
3. Arm `/sys/class/rtc/rtc0/wakealarm` (clearing to `0` first) for periodic sleep-screen rotation or offline calendar reminders.
4. `sync()`, write `1` to `/sys/power/state-extended`, wait 200 ms, write `mem` to `/sys/power/state`.
5. On resume: write `0` to `/sys/power/state-extended`, clear `wakealarm`, and drain wake events.
