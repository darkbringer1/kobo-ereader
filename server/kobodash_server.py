#!/usr/bin/env python3
"""Minimal, zero-dependency self-hosted reference server for KoboDash.

Implements the native KoboDash v0.2 JSON & SSE push protocol:
  - GET /kobo/bundle  Multi-page English dashboard + offline events + alerts (ETag/304)
  - GET /kobo/alerts  Compact open alerts snapshot
  - GET /kobo/stream  Server-Sent Events stream pushing immediately when alerts change

Configure via environment variables:
  KOBODASH_HOST       Bind address (default: 127.0.0.1)
  KOBODASH_PORT       Port (default: 8080)
  KOBODASH_TOKEN      Optional Bearer token required in Authorization header
  KOBODASH_CITY       City label (default: London)
  KOBODASH_LAT        Latitude for Open-Meteo (default: 51.5074)
  KOBODASH_LON        Longitude for Open-Meteo (default: -0.1278)
  KOBODASH_TZ         IANA timezone (default: Europe/London)
  KOBODASH_DATA_DIR   Directory containing optional events.json / alerts.json / stats.json
"""
from __future__ import annotations

import datetime as dt
import hashlib
import hmac
import json
import os
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any
from zoneinfo import ZoneInfo

HOST = os.environ.get("KOBODASH_HOST", "127.0.0.1")
PORT = int(os.environ.get("KOBODASH_PORT", "8080"))
TOKEN = os.environ.get("KOBODASH_TOKEN", "").strip()
CITY = os.environ.get("KOBODASH_CITY", "London")
LAT = float(os.environ.get("KOBODASH_LAT", "51.5074"))
LON = float(os.environ.get("KOBODASH_LON", "-0.1278"))
TZ = ZoneInfo(os.environ.get("KOBODASH_TZ", "Europe/London"))
DATA_DIR = os.path.expanduser(os.environ.get("KOBODASH_DATA_DIR", "./kobodash-data"))

EN_DAYS = ("Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun")
EN_MONTHS = ("Jan", "Feb", "Mar", "Apr", "May", "Jun",
             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")
EN_MONTHS_FULL = ("January", "February", "March", "April", "May", "June",
                  "July", "August", "September", "October", "November", "December")
WMO_EN = {
    0: "Clear", 1: "Mostly Clear", 2: "Partly Cloudy", 3: "Overcast",
    45: "Fog", 48: "Freezing Fog",
    51: "Light Drizzle", 53: "Drizzle", 55: "Heavy Drizzle",
    61: "Light Rain", 63: "Rain", 65: "Heavy Rain",
    71: "Light Snow", 73: "Snow", 75: "Heavy Snow",
    80: "Rain Showers", 81: "Showers", 82: "Heavy Showers",
    95: "Thunderstorm",
}

_weather_cache: dict[str, Any] = {"at": 0.0, "data": {}}


def _read_json(path: str, default: Any) -> Any:
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def _fetch_weather() -> dict[str, Any]:
    now_m = time.monotonic()
    if _weather_cache["data"] and now_m - _weather_cache["at"] < 900:
        return _weather_cache["data"]
    url = (
        f"https://api.open-meteo.com/v1/forecast?latitude={LAT}&longitude={LON}"
        "&current=temperature_2m,apparent_temperature,wind_speed_10m,weather_code"
        "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset"
        f"&timezone={TZ.key}&forecast_days=4"
    )
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "KoboDash-ReferenceServer/1.0"})
        with urllib.request.urlopen(req, timeout=10) as r:
            data = json.loads(r.read().decode("utf-8"))
        _weather_cache["at"] = now_m
        _weather_cache["data"] = data
        return data
    except Exception:
        return _weather_cache["data"]


def _time_bars(now_dt: dt.datetime) -> list[dict[str, Any]]:
    frac_day = (now_dt.hour * 3600 + now_dt.minute * 60 + now_dt.second) / 86400.0
    wk_idx = now_dt.weekday()
    nxt_mo = dt.date(now_dt.year + (1 if now_dt.month == 12 else 0),
                     1 if now_dt.month == 12 else now_dt.month + 1, 1)
    dim = (nxt_mo - dt.date(now_dt.year, now_dt.month, 1)).days
    diy = (dt.date(now_dt.year + 1, 1, 1) - dt.date(now_dt.year, 1, 1)).days
    yday = now_dt.timetuple().tm_yday
    return [
        {"label": "DAY",   "pct": round(frac_day * 100),                     "detail": f"{round(frac_day * 100)}%"},
        {"label": "WEEK",  "pct": round((wk_idx + frac_day) / 7 * 100),      "detail": f"Day {wk_idx + 1}/7"},
        {"label": "MONTH", "pct": round((now_dt.day - 1 + frac_day) / dim * 100), "detail": f"{now_dt.day}/{dim}"},
        {"label": "YEAR",  "pct": round((yday - 1 + frac_day) / diy * 100),  "detail": f"Day {yday}/{diy}"},
    ]


def _load_events(now_dt: dt.datetime) -> tuple[list[dict[str, Any]], list[int]]:
    raw = _read_json(os.path.join(DATA_DIR, "events.json"), [])
    events: list[dict[str, Any]] = []
    marks: set[int] = set()
    for i, ev in enumerate(raw if isinstance(raw, list) else []):
        try:
            t = dt.datetime.fromisoformat(str(ev["when"]))
            t = t.replace(tzinfo=TZ) if t.tzinfo is None else t.astimezone(TZ)
        except Exception:
            continue
        all_day = bool(ev.get("all_day")) or len(str(ev["when"])) <= 10
        if all_day:
            t = t.replace(hour=9, minute=0, second=0, microsecond=0)
            if t.date() < now_dt.date():
                continue
        elif t < now_dt - dt.timedelta(minutes=30):
            continue
        if t.year == now_dt.year and t.month == now_dt.month:
            marks.add(t.day)
        days = (t.date() - now_dt.date()).days
        day_s = "Today" if days == 0 else ("Tomorrow" if days == 1 else f"{EN_DAYS[t.weekday()]} {t.day} {EN_MONTHS[t.month - 1]}")
        when_lbl = day_s if all_day else f"{day_s} {t:%H:%M}"
        events.append({
            "id": str(ev.get("id") or f"ev_{i}_{int(t.timestamp())}"),
            "start_ts": int(t.timestamp()),
            "all_day": all_day,
            "remind_min": 0 if all_day else int(ev.get("remind_min", 15)),
            "title": str(ev.get("title", "Event"))[:88],
            "tag": str(ev.get("tag", ""))[:28],
            "when_label": when_lbl,
            "soon": days <= 2,
        })
    events.sort(key=lambda x: x["start_ts"])
    return events[:24], sorted(marks)


def _load_alerts() -> list[dict[str, Any]]:
    raw = _read_json(os.path.join(DATA_DIR, "alerts.json"), [])
    out: list[dict[str, Any]] = []
    for a in (raw if isinstance(raw, list) else [])[:8]:
        out.append({
            "id": str(a.get("id") or a.get("title", "alert"))[:80],
            "title": str(a.get("title", "Action Needed"))[:96],
            "body": str(a.get("body", ""))[:160],
            "source": str(a.get("source", "Dashboard"))[:32],
            "due": str(a.get("due", "OPEN")).upper()[:18],
            "urgent": bool(a.get("urgent", False)),
            "ts": int(a.get("ts", 0)),
        })
    return out


def build_alerts_payload() -> dict[str, Any]:
    now_dt = dt.datetime.now(TZ)
    alerts = _load_alerts()
    sig = hashlib.sha256(json.dumps(alerts, sort_keys=True).encode()).hexdigest()[:16]
    return {"version": 1, "ts": int(now_dt.timestamp()), "updated": now_dt.strftime("%H:%M"),
            "count": len(alerts), "hash": sig, "alerts": alerts}


def build_bundle() -> dict[str, Any]:
    now_dt = dt.datetime.now(TZ)
    w = _fetch_weather()
    cur = w.get("current") or {}
    daily = w.get("daily") or {}

    temp = round(cur["temperature_2m"]) if "temperature_2m" in cur else None
    feels = round(cur["apparent_temperature"]) if "apparent_temperature" in cur else None
    wind = round(cur["wind_speed_10m"]) if "wind_speed_10m" in cur else None
    desc = WMO_EN.get(cur.get("weather_code", -1), "Weather")

    today_items: list[dict[str, Any]] = []
    if daily.get("sunrise") and daily.get("sunset"):
        today_items.append({
            "label": "Daylight",
            "value": f"{daily['sunrise'][0][11:16]} - {daily['sunset'][0][11:16]}",
            "detail": f"{CITY} local time",
            "urgent": False,
        })
    times = daily.get("time") or []
    for i in range(1, min(4, len(times))):
        d = dt.date.fromisoformat(times[i])
        hi = round(daily["temperature_2m_max"][i])
        lo = round(daily["temperature_2m_min"][i])
        rp = int(daily.get("precipitation_probability_max", [0] * 4)[i] or 0)
        cond = WMO_EN.get(daily.get("weather_code", [-1] * 4)[i], "")
        today_items.append({
            "label": EN_DAYS[d.weekday()],
            "value": f"{lo}° / {hi}°C",
            "detail": f"{cond}  •  {rp}% rain" if rp >= 20 else (cond or "Dry"),
            "urgent": False,
        })

    events, marks = _load_events(now_dt)
    alerts = _load_alerts()
    stats = _read_json(os.path.join(DATA_DIR, "stats.json"), {})

    pages = [
        {
            "id": "today", "kind": "overview", "title": "TODAY & PROGRESS",
            "subtitle": f"{CITY}  •  {EN_DAYS[now_dt.weekday()]} {now_dt.day} {EN_MONTHS[now_dt.month - 1]} {now_dt.year}",
            "hero": f"{temp}°C" if temp is not None else "--°C",
            "hero_sub": f"Feels {feels}°C  •  {desc}  •  Wind {wind} km/h" if feels is not None else desc,
            "bars": _time_bars(now_dt),
            "items": today_items,
            "footer": f"Updated {now_dt.strftime('%H:%M')}  •  Open-Meteo",
        },
        {
            "id": "calendar", "kind": "calendar", "title": "CALENDAR & AGENDA",
            "subtitle": f"{EN_MONTHS_FULL[now_dt.month - 1]} {now_dt.year}  •  Week {now_dt.isocalendar()[1]}",
            "cal_year": now_dt.year, "cal_month": now_dt.month, "cal_today": now_dt.day, "cal_marks": marks,
            "items": [{"label": e["when_label"], "value": e["title"], "detail": e["tag"], "urgent": e["soon"]} for e in events[:5]],
            "footer": f"{len(events)} upcoming events cached for offline reminders",
        },
        {
            "id": "waiting", "kind": "list", "title": "WAITING ON YOU",
            "subtitle": f"{len(alerts)} open items  •  Updated {now_dt.strftime('%H:%M')}" if alerts else f"All clear  •  Updated {now_dt.strftime('%H:%M')}",
            "badge": f"{len(alerts)} OPEN" if alerts else "ALL CLEAR",
            "items": [{"label": a["due"], "value": a["title"], "detail": f"From {a['source']}  •  {a['body']}".strip(" •"), "urgent": a["urgent"]} for a in alerts[:4]],
            "footer": "Self-hosted KoboDash server",
        },
        {
            "id": "stats", "kind": "stats", "title": "STATS & MILESTONES",
            "subtitle": f"Updated {now_dt.strftime('%H:%M')}",
            "metrics": stats.get("metrics") or [
                {"label": "UPTIME",    "value": "99.9%", "detail": "All services healthy"},
                {"label": "EVENTS",    "value": str(len(events)), "detail": "Cached upcoming"},
                {"label": "OPEN ITEMS", "value": str(len(alerts)), "detail": "Waiting on you"},
                {"label": "CITY TEMP", "value": f"{temp}°C" if temp is not None else "--", "detail": desc},
            ],
            "items": stats.get("items") or [],
            "footer": f"Synced {now_dt.strftime('%H:%M')}",
        },
    ]

    return {
        "version": 1, "generated_ts": int(now_dt.timestamp()),
        "updated": now_dt.strftime("%H:%M"),
        "pages": pages, "events": events, "alerts": alerts,
    }


class KoboHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _authorized(self) -> bool:
        if not TOKEN:
            return True
        auth = self.headers.get("authorization", "")
        return auth.startswith("Bearer ") and hmac.compare_digest(auth[7:].strip(), TOKEN)

    def _send_json(self, obj: dict[str, Any]) -> None:
        raw = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        etag = '"' + hashlib.sha256(raw).hexdigest()[:20] + '"'
        if self.headers.get("if-none-match", "").strip() == etag:
            self.send_response(304)
            self.send_header("etag", etag)
            self.send_header("content-length", "0")
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("content-type", "application/json; charset=utf-8")
        self.send_header("content-length", str(len(raw)))
        self.send_header("etag", etag)
        self.send_header("cache-control", "no-store")
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self) -> None:
        if not self._authorized():
            body = b'{"error":"unauthorized"}'
            self.send_response(401)
            self.send_header("content-type", "application/json")
            self.send_header("content-length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        path = self.path.split("?", 1)[0]
        if path in ("/kobo/bundle", "/bundle"):
            return self._send_json(build_bundle())
        if path in ("/kobo/alerts", "/alerts"):
            return self._send_json(build_alerts_payload())
        if path in ("/kobo/stream", "/stream"):
            self.send_response(200)
            self.send_header("content-type", "text/event-stream; charset=utf-8")
            self.send_header("cache-control", "no-store")
            self.send_header("connection", "close")
            self.end_headers()
            last_h = ""
            last_ping = 0.0
            deadline = time.monotonic() + 270.0
            try:
                while time.monotonic() < deadline:
                    pl = build_alerts_payload()
                    if pl["hash"] != last_h:
                        last_h = pl["hash"]
                        self.wfile.write(f"event: alerts\ndata: {json.dumps(pl, ensure_ascii=False)}\n\n".encode("utf-8"))
                        self.wfile.flush()
                        last_ping = time.monotonic()
                    elif time.monotonic() - last_ping >= 25.0:
                        self.wfile.write(b": keepalive\n\n")
                        self.wfile.flush()
                        last_ping = time.monotonic()
                    time.sleep(3.0)
            except OSError:
                pass
            return

        self.send_response(404)
        self.send_header("content-length", "0")
        self.end_headers()


if __name__ == "__main__":
    print(f"KoboDash reference server listening on http://{HOST}:{PORT}/kobo/bundle")
    ThreadingHTTPServer((HOST, PORT), KoboHandler).serve_forever()
