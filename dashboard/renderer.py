"""A fixed 600x800 editorial layout, drawn on the Mac for the measured Touch A panel."""
from datetime import date, datetime
import math
from pathlib import Path
from zoneinfo import ZoneInfo

from PIL import Image, ImageDraw, ImageFont


def weather_icon(code, size=90, night=False):
    image = Image.new('L', (256, 208), 255)
    draw = ImageDraw.Draw(image)
    if code in (0, 1, 2):
        if night:
            draw.ellipse((78, 10, 162, 96), fill=0)
            draw.ellipse((104, 1, 178, 77), fill=255)
        else:
            draw.ellipse((82, 30, 142, 90), outline=0, width=7)
            for i in range(8):
                angle = i * math.pi / 4
                draw.line([(112 + math.cos(angle)*42, 60 + math.sin(angle)*42),
                           (112 + math.cos(angle)*56, 60 + math.sin(angle)*56)], fill=0, width=6)
    if code != 0:
        # The white overlap leaves a single continuous cloud silhouette.
        draw.ellipse((34, 83, 122, 161), fill=0)
        draw.ellipse((83, 52, 187, 160), fill=0)
        draw.ellipse((147, 91, 224, 161), fill=0)
        draw.rectangle((73, 116, 187, 161), fill=0)
        draw.ellipse((42, 91, 114, 153), fill=255)
        draw.ellipse((91, 60, 179, 152), fill=255)
        draw.ellipse((155, 99, 216, 153), fill=255)
        draw.rectangle((73, 116, 187, 153), fill=255)
        if code in (45, 48):
            for y in (176, 195): draw.line((48, y, 208, y), fill=0, width=6)
        elif code is not None and code >= 51:
            for x in (80, 128, 176):
                if code in (71, 73, 75, 77, 85, 86):
                    draw.line((x-7, 185, x+7, 185), fill=0, width=5)
                    draw.line((x, 178, x, 192), fill=0, width=5)
                else:
                    draw.line((x, 176, x-10, 198), fill=0, width=6)
    return image.resize((size, round(size*208/256)), Image.Resampling.LANCZOS)


def render(data, config, output):
    image = Image.new('L', (600, 800), 255)
    draw = ImageDraw.Draw(image)
    regular, bold = config['font'], config['bold_font']
    clock_font = config.get('clock_font', bold)
    def text(x, y, value, size=20, strong=False, fill=0, anchor=None, font=None, max_width=None):
        value = str(value)
        face = ImageFont.truetype(font or (bold if strong else regular), size)
        if max_width:
            while draw.textlength(value, font=face) > max_width and size > 10:
                size -= 1
                face = ImageFont.truetype(font or (bold if strong else regular), size)
        draw.text((x, y), value, font=face, fill=fill, anchor=anchor)
    def line(y): draw.line((28, y, 572, y), fill=0, width=2)
    def number(value, suffix=''):
        return f'{value:.0f}{suffix}' if isinstance(value, (int, float)) else '–'

    clock, weather, device = data['clock'], data['weather'], data['device']
    text(28, 25, config['location']['name'].upper(), 23, True)
    # Show raw dBm, rather than inventing a hardware-independent signal percentage.
    rssi = device.get('rssi')
    strength = 0 if rssi is None else 4 if rssi >= -55 else 3 if rssi >= -67 else 2 if rssi >= -75 else 1
    for i in range(4):
        x, height = 288 + i*7, 5+i*4
        draw.rectangle((x, 47-height, x+4, 47), fill=0 if i < strength else 204)
    text(325, 29, f'{rssi} dBm' if rssi is not None else 'Wi-Fi —', 18)
    draw.rounded_rectangle((451, 31, 478, 45), radius=2, outline=0, width=2)
    draw.rectangle((479, 35, 481, 41), fill=0)
    battery = device.get('battery')
    if battery is not None:
        draw.rectangle((454, 34, 454+round(21*battery/100), 42), fill=0)
    text(492, 28, f'{battery}%' if battery is not None else '—', 19, True)
    line(66)

    text(23, 80, clock['time'], 112, font=clock_font)
    text(396, 103, clock['weekday'].upper(), 16, True)
    text(394, 130, clock['date'].upper(), 31, True)
    text(396, 174, f'{clock["year"]}  /  {clock["timezone"]}', 15)
    line(218)

    text(28, 237, 'THE WEATHER', 15, True)
    badge = 'CACHED WEATHER' if weather.get('cached') else 'CURRENT CONDITIONS'
    text(572, 239, badge, 12, anchor='ra')
    temperature = weather.get('temperature')
    text(24, 262, number(temperature, '°'), 109, True)
    text(227, 294, 'C', 27)
    icon = weather_icon(weather.get('code'), 150, not weather.get('is_day', 1))
    image.paste(icon, (407, 278))
    text(31, 385, weather.get('condition', 'Unavailable'), 26, True, max_width=350)
    text(31, 427, f'Feels {number(weather.get("feels_like"), "°")}  ·  Wind {number(weather.get("wind"))} km/h', 18)
    text(572, 427, f'Humidity {number(weather.get("humidity"), "%")}', 18, anchor='ra')
    line(463)

    text(28, 481, 'THE DAYS AHEAD', 14, True)
    days = weather.get('days', [])
    for i, day in enumerate(days):
        center = 80+i*110
        day_date = date.fromisoformat(day['date'])
        label = 'TODAY' if day_date.isoformat() == clock['rendered_at'][:10] else day_date.strftime('%a').upper()
        text(center, 514, label, 14, True, anchor='ma')
        image.paste(weather_icon(day['code'], 55), (center-27, 537))
        text(center, 584, f'{number(day["high"], "°")} / {number(day["low"], "°")}', 18, True, anchor='ma')
        text(center, 610, f'{number(day["rain"], "%")} rain', 13, anchor='ma')
    if not days:
        text(28, 540, 'Forecast unavailable — reconnect to refresh.', 21)

    draw.rectangle((28, 649, 572, 752), outline=0, width=2)
    message = data['custom']
    text(44, 661, message['title'], 22, True, max_width=510)
    face = ImageFont.truetype(regular, 18)
    words, lines, row = message['text'].split(), [], ''
    for word in words:
        proposed = (row + ' ' + word).strip()
        if row and draw.textlength(proposed, font=face) > 510:
            lines.append(row)
            row = word
        else: row = proposed
    if row: lines.append(row)
    for i, row in enumerate(lines[:2]):
        text(44, 695+i*22, row, 18, max_width=510)

    weather_time = weather.get('observed_at', '')[11:16] or 'unavailable'
    text(28, 773, f'Open-Meteo · weather {weather_time} · screen {clock["time"]}', 12)
    measured_at = device.get('measured_at')
    if measured_at:
        measured = datetime.fromisoformat(measured_at).astimezone(ZoneInfo(config['location']['timezone']))
        telemetry = f'{"Cached" if device.get("cached") else "Device"} {measured:%H:%M}'
    else: telemetry = 'Device unavailable'
    text(572, 773, telemetry, 12, anchor='ra')
    # A small panel benefits from crisp edges and at most 16 gray levels.
    image = image.point(lambda value: round(value/17)*17)
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix('.new.png')
    image.save(temporary)
    temporary.replace(output)
    return image
