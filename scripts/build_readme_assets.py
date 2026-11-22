#!/usr/bin/env python3
"""Generate crisp, framed README showcase assets from host-rendered KoboDash previews.

Run `make -C firmware preview` first to populate output/firmware-preview/.
Then run `python3 scripts/build_readme_assets.py` to refresh docs/images/.
"""
from __future__ import annotations

from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parent.parent
SRC_DIR = ROOT / "output" / "firmware-preview"
OUT_DIR = ROOT / "docs" / "images"

FONT_PATHS = [
    "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
]
FONT_REG_PATHS = [
    "/System/Library/Fonts/Supplemental/Arial.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
]


def _load_font(paths: list[str], size: int) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    for p in paths:
        if Path(p).exists():
            try:
                return ImageFont.truetype(p, size)
            except OSError:
                pass
    return ImageFont.load_default()


def _eink_tint(src_path: Path, size: tuple[int, int]) -> Image.Image:
    """Resize grayscale e-ink render and give it a subtle warm paper-ink tone."""
    g = Image.open(src_path).convert("L").resize(size, Image.Resampling.LANCZOS)
    # Map pure white (255) -> warm e-ink pearl (246, 245, 240), black (0) -> charcoal ink (20, 21, 22)
    lut_r = [int(20 + (246 - 20) * (i / 255.0)) for i in range(256)]
    lut_g = [int(21 + (245 - 21) * (i / 255.0)) for i in range(256)]
    lut_b = [int(22 + (240 - 22) * (i / 255.0)) for i in range(256)]
    r = g.point(lut_r)
    gr = g.point(lut_g)
    b = g.point(lut_b)
    return Image.merge("RGB", (r, gr, b))


def _make_device_card(src_path: Path, screen_w: int, screen_h: int,
                      bezel: int = 16, bottom_chin: int = 38) -> Image.Image:
    """Wrap a rendered e-ink screen in a minimalist Kobo Touch A matte charcoal bezel."""
    w = screen_w + bezel * 2
    h = screen_h + bezel + bottom_chin
    card = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    draw = ImageDraw.Draw(card)

    # Outer charcoal Kobo shell
    draw.rounded_rectangle((0, 0, w - 1, h - 1), radius=18, fill=(36, 39, 44, 255),
                           outline=(62, 66, 74, 255), width=2)
    # Recessed screen well border
    draw.rounded_rectangle((bezel - 2, bezel - 2, bezel + screen_w + 1, bezel + screen_h + 1),
                           radius=4, fill=(22, 24, 28, 255))
    screen = _eink_tint(src_path, (screen_w, screen_h))
    card.paste(screen, (bezel, bezel))

    # Subtle Kobo Touch A physical Home pill on bottom bezel
    pill_w, pill_h = 34, 11
    px = (w - pill_w) // 2
    py = bezel + screen_h + (bottom_chin - pill_h) // 2
    draw.rounded_rectangle((px, py, px + pill_w, py + pill_h), radius=5,
                           fill=(48, 52, 58, 255), outline=(88, 94, 104, 255), width=1)
    return card


def build_hero() -> None:
    """Create a 4-panel horizontal hero showcase banner."""
    panels = [
        ("dash_today.png",          "Today & Progress",      "Weather • Live Progress Bars"),
        ("dash_calendar.png",       "Calendar & Agenda",     "Month Grid • Offline Alerts"),
        ("reader_dict.png",         "Reader + Offline Dict", "EPUB/MOBI • 7 Fonts • EN/DE"),
        ("sleep_touchlock_stats.png", "Stats • Sleep & Lock", "KPI Tiles • Deep Sleep / Lock"),
    ]
    screen_w, screen_h = 300, 400
    bezel, chin = 14, 34
    card_w = screen_w + bezel * 2
    card_h = screen_h + bezel + chin
    gap = 28
    pad_x = 36
    pad_top = 34
    pad_bot = 68

    total_w = pad_x * 2 + card_w * len(panels) + gap * (len(panels) - 1)
    total_h = pad_top + card_h + pad_bot

    # Clean neutral slate backdrop that looks sharp in both GitHub Light and Dark themes
    bg = Image.new("RGBA", (total_w, total_h), (23, 27, 34, 255))
    bg_draw = ImageDraw.Draw(bg)
    bg_draw.rounded_rectangle((1, 1, total_w - 2, total_h - 2), radius=18,
                              fill=(23, 27, 34, 255), outline=(48, 54, 65, 255), width=2)

    title_font = _load_font(FONT_PATHS, 17)
    sub_font = _load_font(FONT_REG_PATHS, 13)

    for i, (fname, title, subtitle) in enumerate(panels):
        x = pad_x + i * (card_w + gap)
        y = pad_top

        # Soft drop shadow
        shadow = Image.new("RGBA", (card_w + 30, card_h + 30), (0, 0, 0, 0))
        sdraw = ImageDraw.Draw(shadow)
        sdraw.rounded_rectangle((15, 19, 15 + card_w, 19 + card_h), radius=18, fill=(0, 0, 0, 120))
        shadow = shadow.filter(ImageFilter.GaussianBlur(8))
        bg.alpha_composite(shadow, (x - 15, y - 12))

        card = _make_device_card(SRC_DIR / fname, screen_w, screen_h, bezel=bezel, bottom_chin=chin)
        bg.alpha_composite(card, (x, y))

        # Centered labels below each device
        cx = x + card_w // 2
        tb = bg_draw.textbbox((0, 0), title, font=title_font)
        bg_draw.text((cx - (tb[2] - tb[0]) // 2, y + card_h + 14), title,
                     fill=(240, 244, 248, 255), font=title_font)
        sb = bg_draw.textbbox((0, 0), subtitle, font=sub_font)
        bg_draw.text((cx - (sb[2] - sb[0]) // 2, y + card_h + 38), subtitle,
                     fill=(148, 158, 172, 255), font=sub_font)

    out_path = OUT_DIR / "hero-showcase.png"
    hero_q = bg.convert("RGB").quantize(colors=128, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE)
    hero_q.save(out_path, optimize=True)
    print(f"Wrote {out_path} ({out_path.stat().st_size // 1024} KB)")


def build_gallery_cards() -> None:
    """Create compact framed PNGs for the README screenshot tables."""
    mapping = {
        "dash-today.png":      "dash_today.png",
        "dash-calendar.png":   "dash_calendar.png",
        "dash-waiting.png":    "dash_waiting.png",
        "dash-stats.png":      "dash_stats.png",
        "reader-dict.png":     "reader_dict.png",
        "reader-toolbar.png":  "reader_toolbar.png",
        "alert-overlay.png":   "alert_overlay.png",
        "sleep-deep.png":      "sleep_deep_calendar.png",
        "sleep-touchlock.png": "sleep_touchlock_stats.png",
        "clock-light.png":     "clock.png",
        "clock-dark.png":      "clock_dark.png",
        "settings-server.png": "settings_server.png",
        "settings-device.png": "settings_device.png",
        "keyboard.png":        "keyboard.png",
    }
    screen_w, screen_h = 390, 520
    pad = 10
    for out_name, src_name in mapping.items():
        w, h = screen_w + pad * 2, screen_h + pad * 2
        frame = Image.new("RGB", (w, h), (38, 42, 48))
        draw = ImageDraw.Draw(frame)
        draw.rectangle((0, 0, w - 1, h - 1), outline=(72, 78, 88), width=1)
        screen = _eink_tint(SRC_DIR / src_name, (screen_w, screen_h))
        frame.paste(screen, (pad, pad))
        out_path = OUT_DIR / out_name
        frame_q = frame.quantize(colors=64, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE)
        frame_q.save(out_path, optimize=True)
        print(f"Wrote {out_path} ({out_path.stat().st_size // 1024} KB)")


def main() -> int:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    build_hero()
    build_gallery_cards()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
