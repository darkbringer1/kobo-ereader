#!/usr/bin/env python3
"""Composite host-rendered KoboDash PGM canvases + TTF text overlays into PNG previews."""
import os
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

FONTS = {
    0: "/System/Library/Fonts/Supplemental/Arial.ttf",
    1: "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
    2: "/System/Library/Fonts/Supplemental/DIN Alternate Bold.ttf",
}

def composite_one(pgm_path: Path, txt_path: Path, png_path: Path) -> None:
    img = Image.open(pgm_path).convert("L")
    draw = ImageDraw.Draw(img)
    if txt_path.exists():
        for raw in txt_path.read_text(encoding="utf-8").splitlines():
            parts = raw.split("\t", 8)
            if len(parts) != 9:
                continue
            font_id, size_px, align, x, y, w, h, fg = map(int, parts[:8])
            text = parts[8].replace("\\n", "\n")
            font_path = FONTS.get(font_id, FONTS[0])
            # Approximate stb_truetype pixel height sizing
            try:
                font = ImageFont.truetype(font_path, max(10, int(size_px * 0.88)))
            except OSError:
                font = ImageFont.load_default()

            bbox = draw.multiline_textbbox((0, 0), text, font=font, align="center" if align == 1 else "left")
            tw = bbox[2] - bbox[0]
            th = bbox[3] - bbox[1]

            if align == 1:
                tx = x + (w - tw) // 2 - bbox[0]
            elif align == 2:
                tx = x + w - tw - bbox[0]
            else:
                tx = x - bbox[0]
            ty = y + (h - th) // 2 - bbox[1]
            draw.multiline_text((tx, ty), text, fill=fg, font=font,
                                align="center" if align == 1 else ("right" if align == 2 else "left"))
    img.save(png_path)

def main() -> int:
    if len(sys.argv) < 2:
        print("Usage: preview_firmware.py <dump_dir>", file=sys.stderr)
        return 1
    dump_dir = Path(sys.argv[1])
    for pgm in sorted(dump_dir.glob("*.pgm")):
        txt = pgm.with_suffix(".txt")
        png = pgm.with_suffix(".png")
        composite_one(pgm, txt, png)
        print(f"Rendered {png}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
