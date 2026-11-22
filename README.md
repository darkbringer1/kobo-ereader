# KoboDash

**Ultra-lightweight native C11 e-ink shell, book reader, offline dictionary, and self-hosted dashboard for the original Kobo Touch A (N905).**

KoboDash turns a 2011 Kobo Touch A (256 MB RAM, ARMv7 Cortex-A8, Linux 2.6.35.3, glibc 2.19) into a crisp, fast, multi-week-battery e-ink desk display and distraction-free e-reader — without modifying the bootloader, kernel, partitions, or `/etc/init.d/rcS`. You can jump seamlessly back and forth between KoboDash and Stock Kobo UI (Nickel) at any time.

---

## Features

- **Native C11 E-Ink Shell (~492 KB stripped ARM binary, ~2 MB RSS)**
  - Uses [FBInk](https://github.com/NiLuJe/FBInk) (`libfbink`) directly over `/dev/fb0` with waveform-aware partial refreshes (`WFM_DU` for zero-flash minute clock updates & toggles, `WFM_GC16` non-flashing partial grayscale for crisp book pages, configurable periodic anti-ghosting full flashes).
  - Light & Dark (inverted) themes, 12h/24h clock, configurable timezone & SNTP time sync, MicroSD gallery screensaver with clock overlay.
- **Instant Offline Book Reader + Multi-Language Dictionaries**
  - Native `.epub`, `.mobi`, and `.txt` reader with full UTF-8 / CP1252 / UTF-16 decoding, HTML entity & mojibake repair, and MOBI record-boundary stitching.
  - **7 selectable book font families** (Georgia, Charter, Palatino, Baskerville, Times New Roman, Verdana, Trebuchet MS — only the active family is loaded in RAM).
  - **Instant long-press word lookup** backed by pre-indexed binary `.kdict` offline dictionaries (English WordNet + English↔German FreeDict with stemming/lemmatization and automatic fallback).
- **Multi-Page Self-Hosted Dashboard over Native TLS 1.2 / 1.3**
  - Statically linked client-only **mbedTLS 3.6.x** with full X.509 CA verification (`cacert.pem`), `Authorization: Bearer` auth, and `ETag` / `304 Not Modified` caching.
  - Native high-contrast e-ink layouts for 4 rotating glanceable pages (plus backward compatibility with 600×800 PNG/JPG/BMP images and simple JSON):
    1. **Today & Progress** — Weather hero card, live **Day / Week / Month / Year** progress bars (computed locally even offline), daylight/forecast/transit rows.
    2. **Calendar & Agenda** — Native month calendar grid (today highlighted + event-day markers) and upcoming agenda list.
    3. **Waiting on You** — Prioritized action/decision cards (`TODAY` / `OPEN`) with wrapped details.
    4. **Stats & Milestones** — 2×2 KPI metric tiles + milestone/sports rows.
- **Dual Trigger Engine (Offline Calendar + Live Remote Push)**
  - **Offline Calendar Triggers**: Upcoming events are cached locally in `bundle.json`; every minute tick checks upcoming start times and pops a high-contrast reminder overlay (`DISMISS` / `SNOOZE 30m` / `VIEW ALL`) even completely offline.
  - **Real-Time Remote Push (SSE)**: Configurable (`Live on Power` / `Always Live` / `Poll Only`). When live, a tiny background SSE watcher receives new alerts in <1 second and auto-clears overlays when resolved remotely. Never interrupts active book reading.
- **True Hardware Deep Sleep + Touch-Lock Desk Mode**
  - **Slide Power (unplugged)**: Cleanly powers down Wi-Fi (`ar6000`), freezes a rotating locked dashboard screen (`Today & Progress` → `Calendar & Agenda` → `Stats & Sports` every 1–6h via `/sys/class/rtc/rtc0/wakealarm`), and suspends the CPU to RAM (`/sys/power/state` `mem`) for multi-week standby.
  - **Slide Power (plugged in)**: Enters **Touch-Lock Desk Mode** — ignores all accidental touches while continuing to rotate sleep screens and receive live pushes.
  - **Auto-Sleep Inactivity Timeout**: Configurable (`Off / 5m / 10m / 15m / 30m / 60m`).
  - **Hold Power (2.4s)**: Clean power off from anywhere.
- **On-Device 4-Tab Settings, Touch Keyboard & Reliable Touch A Wi-Fi**
  - Configure display, Wi-Fi (scan, WPA/WPA2, hidden SSID, forget, and 1-tap `ar6000` + `sdio_wifi_pwr` hardware chip reset for stubborn routers & personal hotspots), server URL/token/sync/push schedules, CPU governor, touch calibration, SSH, MicroSD mount, sleep art, and auto-boot directly on-device.
- **Brick-Proof Self-Test-Gated Auto-Boot**
  - Optional 2-file udev hook (`KERNEL=="loop0"`, never modifies `/etc/init.d/rcS`) gated by `kobodash --self-test`, a once-per-boot RAM guard (so returning to Stock UI never re-triggers it), and an automatic 15-second crash/bootloop circuit breaker.

---

## Controls

| Action | Behavior |
| --- | --- |
| **Swipe Left / Right** or **`< PREV` / `NEXT >`** | Cycle screens (`Clock → Library → Dashboard Pages → System Status → Gallery`) |
| **Tap top-right Gear** (or **Swipe Up**) | Open 4-tab Settings (`GENERAL`, `WI-FI`, `SERVER`, `DEVICE`) |
| **Tap top Refresh** (or **Swipe Down**) | Sync server & NTP clock immediately |
| **Long-press word** (in Book Reader) | Offline dictionary popup (tap language pill to switch EN / EN→DE / DE→EN) |
| **Short press Home** | Next screen (or unlock Touch-Lock / dismiss alert) |
| **Hold Home (2s)** | Cleanly exit KoboDash and return to **Stock Kobo UI (Nickel)** |
| **Slide Power** | **True Deep Sleep** (unplugged) or **Touch-Lock** (plugged in); slide again to wake/unlock |
| **Hold Power (2.4s)** | Clean Power Off |

---

## Quick Start

### Prerequisites
- **Build host**: macOS or Linux with [Zig](https://ziglang.org/) (`0.14+`, tested with `0.16.0`), C compiler (`cc`), and Python 3.11+ (`Pillow` for host preview rendering).
- **On Kobo**: [NickelMenu](https://pgaskin.net/NickelMenu/) installed, plus `libfbink.so.1` and `libz.so.1` placed in `artifacts/inspect/` (extracted from [NiLuJe's FBInk](https://github.com/NiLuJe/FBInk) and KoboStuff packages).
- **Always back up your Kobo** before making changes (`python3 scripts/backup.py`).

### 1. Build Firmware & Run Host Tests + Previews

```sh
# Fetches mbedTLS 3.6.3 automatically on first build, compiles ARM Cortex-A8 binary,
# runs all 258 host C unit tests, and renders 600x800 PNG previews to output/firmware-preview/
make -C firmware all preview
```

### 2. Build Offline Dictionaries *(Optional)*

```sh
python3 scripts/build_dict.py
# Generates output/en.kdict, output/en-de.kdict, output/de-en.kdict
```

### 3. Deploy to USB-Mounted Kobo

Connect your Kobo in Stock UI (`Connect`), then run:

```sh
# Standard deploy (installs/updates .adds/kobodash/ and NickelMenu entry; preserves existing config.ini)
python3 scripts/deploy_firmware.py

# Optional: configure your self-hosted dashboard URL & token during deploy
python3 scripts/deploy_firmware.py \
  --server-url "https://dashboard.example.com/kobo/bundle" \
  --server-token "your-secret-bearer-token"

# Optional: also stage the 2-file brick-proof udev auto-boot package (.kobo/KoboRoot.tgz)
python3 scripts/deploy_firmware.py --install-autoboot
```

Safely eject USB, then launch **NickelMenu → KoboDash** (or reboot if `--install-autoboot` was enabled).

---

## Self-Hosted Server & JSON API

KoboDash works completely offline as a clock/reader/gallery, or connects to any HTTPS/HTTP server you host. A zero-dependency Python 3 reference server is included at [`server/kobodash_server.py`](server/kobodash_server.py):

```sh
KOBODASH_TOKEN="choose-a-strong-random-token" \
KOBODASH_CITY="London" KOBODASH_LAT="51.5074" KOBODASH_LON="-0.1278" KOBODASH_TZ="Europe/London" \
python3 server/kobodash_server.py
```

Place it behind any HTTPS reverse proxy (Caddy, Nginx, Cloudflare Tunnel, etc.) and point KoboDash's `server_url` to `https://<your-host>/kobo/bundle`.

### Supported Endpoints
- **`GET /kobo/bundle`** (or any URL ending in `/bundle`) — Returns multi-page JSON (`ETag` / `If-None-Match` → `304` supported):
  - `pages[]`: array of up to 6 pages with `kind` in `"overview" | "calendar" | "list" | "stats" | "generic"`
  - `events[]`: array of upcoming events (`id`, `start_ts` Unix UTC epoch, `all_day`, `remind_min`, `title`, `tag`, `when_label`) cached locally for offline reminders
  - `alerts[]`: array of open attention items (`id`, `title`, `body`, `source`, `due`, `urgent`, `ts`)
- **`GET /kobo/stream`** *(auto-derived from `.../bundle`)* — Server-Sent Events (`text/event-stream`) emitting `data: {"alerts":[...]}` whenever open alerts change.
- **Image / Legacy JSON fallback**: Pointing `server_url` to any endpoint returning a `600×800` PNG/JPG/BMP/GIF or `{"title":"...","subtitle":"...","lines":[...]}` also works out of the box.

---

## Repository Structure

```text
firmware/
  Makefile                  Cross-compilation (Zig arm-linux-gnueabihf.2.19) & host test build
  include/                  kobodash.h, kd_tls.h, kd_mbedtls_config.h, fbink.h, stb_truetype.h
  src/
    main.c                  Event loop, async sync, SSE watcher, deep sleep & touch-lock
    dashboard.c             JSON bundle/alert parser, offline/remote trigger engine & 4-page renderer
    tls.c / net.c           mbedTLS 3.6 TLS 1.2/1.3 HTTPS/HTTP client, SNTP, ETag & SSE stream
    ui.c / canvas.c         Screens, 4-tab Settings, vector icons & 16-level grayscale primitives
    book.c / reader.c       EPUB/MOBI/TXT decoding, pagination & reading toolbar
    dict.c / font.c         Binary .kdict lookup (EN + EN<->DE) & stb_truetype multi-family loader
    input.c / keyboard.c    Touch/button evdev handling & on-screen e-ink QWERTY keyboard
    sysinfo.c / wifi.c      Telemetry, safe suspend-to-RAM/RTC wakealarm & AR6000 Wi-Fi control
  device/                   Kobo shell wrappers (runner.sh, autoboot.sh, wifi-ctl.sh, udev rule)
  tests/test_kobodash.c     258 host C unit tests + preview frame dumper
server/
  kobodash_server.py        Standalone zero-dependency Python 3 reference server (JSON + SSE)
scripts/
  build_mbedtls.py          Parallel static mbedTLS 3.6 builder (ARM + host)
  build_dict.py             Offline WordNet + FreeDict .kdict compiler
  deploy_firmware.py        Checksum-verified USB deployer (AppleDouble-clean)
  preview_firmware.py       Host PGM+text → PNG preview compositor
docs/
  discovery.md              Verified Kobo Touch A hardware, glibc 2.19, EPDC & sleep findings
  recovery.md               Failsafes, recovery steps & clean uninstall instructions
```

---

## Hardware Notes & Recovery

Before modifying startup or sleep behavior on a Kobo Touch A, read [`docs/discovery.md`](docs/discovery.md) and [`docs/recovery.md`](docs/recovery.md).

## Credits & License

- Built with [FBInk](https://github.com/NiLuJe/FBInk) (NiLuJe), [NickelMenu](https://pgaskin.net/NickelMenu/) (pgaskin), [mbedTLS](https://github.com/Mbed-TLS/mbedtls), [stb_truetype](https://github.com/nothings/stb), WordNet, and FreeDict.
- Project source code and documentation are released under the [MIT License](LICENSE).
