#!/usr/bin/env python3
"""Render a grayscale smoke-test image on the host, plus a stock-reader CBZ."""
import argparse
from datetime import datetime
from pathlib import Path
import zipfile

from PIL import Image, ImageDraw, ImageFont


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--width', type=int, default=600)
    parser.add_argument('--height', type=int, default=800)
    parser.add_argument('--text', default='Hello World')
    parser.add_argument('--font', default='/System/Library/Fonts/Supplemental/Arial.ttf')
    parser.add_argument('--output', type=Path, default=Path('output'))
    args = parser.parse_args()
    if min(args.width, args.height) < 300:
        parser.error('Use dimensions of at least 300 pixels')
    args.output.mkdir(parents=True, exist_ok=True)
    # Draw in a fixed coordinate system and scale to the measured panel later.
    canvas = Image.new('L', (600, 800), 255)
    draw = ImageDraw.Draw(canvas)

    def text(x, y, value, size=24):
        font = ImageFont.truetype(args.font, size)
        while draw.textlength(value, font=font) > 504 and size > 10:
            size -= 1
            font = ImageFont.truetype(args.font, size)
        draw.text((x, y), value, fill=0, font=font)

    now = datetime.now().astimezone()
    text(48, 45, 'KOBO / OPEN DASHBOARD', 21)
    draw.line((48, 90, 552, 90), fill=0, width=3)
    text(48, 115, args.text, 56)
    text(48, 205, now.strftime('%H:%M'), 105)
    text(48, 330, now.strftime('%A, %d %B %Y'), 25)
    text(48, 370, now.strftime('Rendered on Mac / %Z'), 20)
    draw.line((48, 425, 552, 425), fill=0, width=2)
    text(48, 450, 'YOUR FIRST E-INK DASHBOARD', 23)
    text(48, 500, 'Custom text: This Kobo has a new job.', 23)
    text(48, 553, 'Weather: not connected yet', 23)
    text(48, 596, 'Battery: awaiting device telemetry', 23)
    for i in range(16):
        x = 48 + i * 31
        draw.rectangle((x, 675, x + 30, 710), fill=i * 17)
    text(48, 735, 'Static preview / 16 gray levels / local rendering', 19)
    canvas = canvas.resize((args.width, args.height), Image.Resampling.LANCZOS)
    canvas = canvas.point(lambda value: round(value / 17) * 17)
    png = args.output / 'hello-dashboard.png'
    canvas.save(png)
    cbz = args.output / 'Hello Dashboard.cbz'
    with zipfile.ZipFile(cbz, 'w', compression=zipfile.ZIP_DEFLATED) as book:
        book.write(png, '001.png')
    print(f'{png}: {canvas.width}x{canvas.height}, grayscale, <=16 levels')
    print(cbz)


if __name__ == '__main__':
    main()
