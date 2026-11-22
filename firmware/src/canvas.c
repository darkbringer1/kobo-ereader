#include "kobodash.h"

#include <stdlib.h>
#include <string.h>

static inline void put_px(Canvas *c, int x, int y, uint8_t gray) {
    if ((unsigned)x < KD_WIDTH && (unsigned)y < KD_HEIGHT) {
        c->pixels[y * KD_WIDTH + x] = gray;
    }
}

void canvas_clear(Canvas *c, uint8_t gray) {
    memset(c->pixels, gray, sizeof(c->pixels));
}

void canvas_fill_rect(Canvas *c, int x, int y, int w, int h, uint8_t gray) {
    if (w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > KD_WIDTH ? KD_WIDTH : x + w;
    int y1 = y + h > KD_HEIGHT ? KD_HEIGHT : y + h;
    if (x0 >= x1 || y0 >= y1) return;
    for (int py = y0; py < y1; py++) {
        memset(&c->pixels[py * KD_WIDTH + x0], gray, (size_t)(x1 - x0));
    }
}

void canvas_hline(Canvas *c, int x, int y, int w, int thickness, uint8_t gray) {
    canvas_fill_rect(c, x, y, w, thickness, gray);
}

void canvas_vline(Canvas *c, int x, int y, int h, int thickness, uint8_t gray) {
    canvas_fill_rect(c, x, y, thickness, h, gray);
}

void canvas_stroke_rect(Canvas *c, int x, int y, int w, int h, int thickness, uint8_t gray) {
    if (w <= 0 || h <= 0 || thickness <= 0) return;
    canvas_fill_rect(c, x, y, w, thickness, gray);
    canvas_fill_rect(c, x, y + h - thickness, w, thickness, gray);
    canvas_fill_rect(c, x, y, thickness, h, gray);
    canvas_fill_rect(c, x + w - thickness, y, thickness, h, gray);
}

static bool in_rrect(int dx, int dy, int w, int h, int r) {
    if (dx < 0 || dy < 0 || dx >= w || dy >= h) return false;
    if (r <= 0) return true;
    int cx = -1, cy = -1;
    if (dx < r && dy < r) { cx = r - 1; cy = r - 1; }
    else if (dx >= w - r && dy < r) { cx = w - r; cy = r - 1; }
    else if (dx < r && dy >= h - r) { cx = r - 1; cy = h - r; }
    else if (dx >= w - r && dy >= h - r) { cx = w - r; cy = h - r; }
    if (cx < 0) return true;
    int ox = dx - cx;
    int oy = dy - cy;
    return (ox * ox + oy * oy) <= r * r;
}

void canvas_fill_rrect(Canvas *c, int x, int y, int w, int h, int r, uint8_t gray) {
    if (w <= 0 || h <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r <= 1) {
        canvas_fill_rect(c, x, y, w, h, gray);
        return;
    }
    /* Fast middle strip */
    canvas_fill_rect(c, x, y + r, w, h - 2 * r, gray);
    /* Top and bottom corner strips */
    for (int dy = 0; dy < r; dy++) {
        for (int dx = 0; dx < w; dx++) {
            if (in_rrect(dx, dy, w, h, r)) {
                put_px(c, x + dx, y + dy, gray);
                put_px(c, x + dx, y + h - 1 - dy, gray);
            }
        }
    }
}

void canvas_stroke_rrect(Canvas *c, int x, int y, int w, int h, int r, int thickness, uint8_t gray) {
    if (w <= 0 || h <= 0 || thickness <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    int ir = r - thickness;
    if (ir < 0) ir = 0;
    int iw = w - 2 * thickness;
    int ih = h - 2 * thickness;

    for (int dy = 0; dy < h; dy++) {
        /* Fast skip middle interior */
        if (dy >= r && dy < h - r) {
            canvas_fill_rect(c, x, y + dy, thickness, 1, gray);
            canvas_fill_rect(c, x + w - thickness, y + dy, thickness, 1, gray);
            continue;
        }
        for (int dx = 0; dx < w; dx++) {
            if (!in_rrect(dx, dy, w, h, r)) continue;
            if (iw > 0 && ih > 0 && in_rrect(dx - thickness, dy - thickness, iw, ih, ir)) continue;
            put_px(c, x + dx, y + dy, gray);
        }
    }
}

void canvas_fill_circle(Canvas *c, int cx, int cy, int r, uint8_t gray) {
    int r2 = r * r;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy <= r2) {
                put_px(c, cx + dx, cy + dy, gray);
            }
        }
    }
}

void canvas_stroke_circle(Canvas *c, int cx, int cy, int r, int thickness, uint8_t gray) {
    int ro2 = r * r;
    int ri = r - thickness;
    int ri2 = ri > 0 ? ri * ri : 0;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 <= ro2 && d2 >= ri2) {
                put_px(c, cx + dx, cy + dy, gray);
            }
        }
    }
}

void canvas_draw_battery(Canvas *c, int x, int y, int pct, int charging, uint8_t fg, uint8_t bg) {
    const int bw = 34, bh = 18;
    canvas_stroke_rrect(c, x, y, bw, bh, 3, 2, fg);
    canvas_fill_rect(c, x + bw, y + 5, 3, bh - 10, fg);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int inner_w = (bw - 8) * pct / 100;
    if (inner_w > 0) {
        canvas_fill_rect(c, x + 4, y + 4, inner_w, bh - 8, fg);
    }
    if (charging) {
        /* Tiny lightning bolt cutout/mark inside battery */
        int mx = x + bw / 2;
        int my = y + bh / 2;
        uint8_t mark = (inner_w > bw / 2) ? bg : fg;
        canvas_fill_rect(c, mx - 1, my - 4, 3, 4, mark);
        canvas_fill_rect(c, mx - 2, my - 1, 5, 2, mark);
        canvas_fill_rect(c, mx - 1, my + 1, 3, 4, mark);
    }
}

void canvas_draw_wifi(Canvas *c, int x, int y, int up, int rssi, uint8_t fg, uint8_t muted) {
    int bars = 0;
    if (up) {
        if (rssi == 0) bars = 3; /* Connected, RSSI unavailable */
        else if (rssi >= -55) bars = 4;
        else if (rssi >= -67) bars = 3;
        else if (rssi >= -78) bars = 2;
        else bars = 1;
    }
    for (int i = 0; i < 4; i++) {
        int bh = 5 + i * 4;
        int bx = x + i * 6;
        int by = y + (17 - bh);
        canvas_fill_rect(c, bx, by, 4, bh, (i < bars) ? fg : muted);
    }
    if (!up) {
        /* Small diagonal slash for offline */
        for (int i = 0; i < 18; i++) {
            put_px(c, x + i + 2, y + 17 - i, fg);
            put_px(c, x + i + 3, y + 17 - i, fg);
        }
    }
}

void canvas_draw_gear(Canvas *c, int cx, int cy, int r, uint8_t fg, uint8_t bg) {
    /* 8-tooth crisp geometric gear */
    int tooth = r + 3;
    canvas_fill_rect(c, cx - 2, cy - tooth, 5, tooth * 2 + 1, fg);
    canvas_fill_rect(c, cx - tooth, cy - 2, tooth * 2 + 1, 5, fg);
    int d = (tooth * 7) / 10;
    for (int t = -2; t <= 2; t++) {
        for (int k = -d; k <= d; k++) {
            put_px(c, cx + k + t, cy + k, fg);
            put_px(c, cx + k + t, cy - k, fg);
        }
    }
    canvas_fill_circle(c, cx, cy, r - 1, fg);
    canvas_fill_circle(c, cx, cy, r / 2, bg);
}

void canvas_draw_refresh_icon(Canvas *c, int cx, int cy, int r, uint8_t fg, uint8_t bg) {
    canvas_stroke_circle(c, cx, cy, r, 2, fg);
    /* Open gap on upper-right arc */
    canvas_fill_rect(c, cx + 2, cy - r - 2, r + 2, r, bg);
    /* Clean clockwise arrowhead at top of circle pointing right-down */
    int ax = cx + 5;
    int ay = cy - r + 2;
    for (int i = 0; i <= 5; i++) {
        put_px(c, ax - i, ay - 4 + i, fg);
        put_px(c, ax - i + 1, ay - 4 + i, fg);
        put_px(c, ax - i, ay + 1, fg);
        put_px(c, ax - i, ay + 2, fg);
    }
}

void canvas_draw_lock(Canvas *c, int cx, int cy, uint8_t fg, uint8_t bg) {
    /* Clean vector padlock icon centered at (cx, cy) */
    canvas_stroke_circle(c, cx, cy - 5, 6, 2, fg);
    canvas_fill_rect(c, cx - 9, cy - 2, 19, 14, bg);
    canvas_fill_rrect(c, cx - 9, cy - 1, 18, 13, 3, fg);
    canvas_fill_circle(c, cx, cy + 5, 2, bg);
}

void canvas_draw_progress(Canvas *c, int x, int y, int w, int h, int pct,
                          uint8_t fg, uint8_t bg, uint8_t border) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int r = h / 2;
    canvas_fill_rrect(c, x, y, w, h, r, bg);
    canvas_stroke_rrect(c, x, y, w, h, r, 1, border);
    int inner_w = (w - 4) * pct / 100;
    if (inner_w > 2) {
        canvas_fill_rrect(c, x + 2, y + 2, inner_w, h - 4, (h - 4) / 2, fg);
    }
}

void canvas_invert(Canvas *c) {
    for (size_t i = 0; i < sizeof(c->pixels); i++) {
        c->pixels[i] = (uint8_t)(255u - c->pixels[i]);
    }
}
