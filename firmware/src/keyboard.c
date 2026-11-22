#include "kobodash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Clean 600x800 portrait on-screen touch keyboard rendered into Canvas with stb_truetype.
   Typing refreshes only INPUT_BOX_RECT with WFM_DU (~260ms, zero flash!). */

static const KdRect INPUT_BOX_RECT = { 24, 114, 552, 74 };

static const char *ROW0_LOWER = "qwertyuiop";
static const char *ROW0_UPPER = "QWERTYUIOP";
static const char *ROW0_SYM   = "1234567890";

static const char *ROW1_LOWER = "asdfghjkl";
static const char *ROW1_UPPER = "ASDFGHJKL";
static const char *ROW1_SYM   = "-/:;()$@&";

static const char *ROW2_LOWER = "zxcvbnm";
static const char *ROW2_UPPER = "ZXCVBNM";
static const char *ROW2_SYM   = ".,?!_+=~";

#define KEY_H      66
#define KEY_GAP    6
#define ROW0_Y     342
#define ROW1_Y     416
#define ROW2_Y     490
#define ROW3_Y     564
#define ACT_ROW_Y  664
#define ACT_ROW_H  72

static void get_presets(const KeyboardState *kb, const char *labels[4], const char *inserts[4]) {
    if (kb->url_presets) {
        labels[0] = "https://";     inserts[0] = "https://";
        labels[1] = "http://";      inserts[1] = "http://";
        labels[2] = "/kobo/bundle"; inserts[2] = "/kobo/bundle";
        labels[3] = "Clear";        inserts[3] = NULL;
    } else if (kb->ntp_presets) {
        labels[0] = "pool.ntp.org";     inserts[0] = "pool.ntp.org";
        labels[1] = "time.cloudflare";  inserts[1] = "time.cloudflare.com";
        labels[2] = "time.google";      inserts[2] = "time.google.com";
        labels[3] = "Clear";            inserts[3] = NULL;
    } else if (kb->password) {
        labels[0] = kb->show_secret ? "Hide" : "Show"; inserts[0] = NULL;
        labels[1] = ""; inserts[1] = NULL;
        labels[2] = ""; inserts[2] = NULL;
        labels[3] = "Clear"; inserts[3] = NULL;
    } else if (kb->purpose == KBD_SCREENSAVER_DIR) {
        labels[0] = "/mnt/sd/"; inserts[0] = "/mnt/sd/screensavers";
        labels[1] = "screensavers"; inserts[1] = "screensavers";
        labels[2] = ""; inserts[2] = NULL;
        labels[3] = "Clear"; inserts[3] = NULL;
    } else {
        labels[0] = ""; inserts[0] = NULL;
        labels[1] = ""; inserts[1] = NULL;
        labels[2] = ""; inserts[2] = NULL;
        labels[3] = "Clear"; inserts[3] = NULL;
    }
}

void kbd_open(KeyboardState *kb, KeyboardPurpose purpose, const char *title,
              const char *hint, const char *initial, size_t max_len, bool password) {
    memset(kb, 0, sizeof(*kb));
    kb->active = true;
    kb->purpose = purpose;
    snprintf(kb->title, sizeof(kb->title), "%s", title ? title : "Edit Value");
    snprintf(kb->hint, sizeof(kb->hint), "%s", hint ? hint : "");
    kb->max_len = (max_len > 0 && max_len < sizeof(kb->buf)) ? max_len : (sizeof(kb->buf) - 1);
    if (initial && initial[0]) {
        snprintf(kb->buf, kb->max_len + 1, "%s", initial);
    }
    kb->password = password;
    kb->show_secret = !password;
    kb->url_presets = (purpose == KBD_SERVER_URL);
    kb->ntp_presets = (purpose == KBD_NTP_SERVER);
}

static void draw_centered_text(Canvas *c, FontStyle style, int px,
                               int x, int y, int w, int h, const char *txt, uint8_t fg) {
    if (!txt || !txt[0]) return;
    int tw = font_measure_utf8(style, px, txt, strlen(txt));
    int asc = 0, desc = 0, lh = 0;
    font_line_metrics(style, px, &asc, &desc, &lh);
    int tx = x + (w - tw) / 2;
    if (tx < x + 4) tx = x + 4;
    int base_y = y + (h - lh) / 2 + asc;
    font_draw_utf8(c, style, px, tx, base_y, txt, strlen(txt), fg);
}

static void draw_key(Canvas *c, int x, int y, int w, int h, const char *label,
                     bool filled, uint8_t bg, uint8_t fg, uint8_t border, int px) {
    if (filled) {
        canvas_fill_rrect(c, x, y, w, h, 8, fg);
        draw_centered_text(c, FONT_STYLE_UI_BOLD, px, x, y, w, h, label, bg);
    } else {
        canvas_fill_rrect(c, x, y, w, h, 8, bg);
        canvas_stroke_rrect(c, x, y, w, h, 8, 2, border);
        draw_centered_text(c, FONT_STYLE_UI_BOLD, px, x, y, w, h, label, fg);
    }
}

static void render_input_box(Canvas *c, const KeyboardState *kb,
                             uint8_t bg, uint8_t fg, uint8_t card, uint8_t border) {
    canvas_fill_rect(c, INPUT_BOX_RECT.left, INPUT_BOX_RECT.top,
                     INPUT_BOX_RECT.width, INPUT_BOX_RECT.height, bg);
    canvas_fill_rrect(c, INPUT_BOX_RECT.left, INPUT_BOX_RECT.top,
                      INPUT_BOX_RECT.width, INPUT_BOX_RECT.height, 10, card);
    canvas_stroke_rrect(c, INPUT_BOX_RECT.left, INPUT_BOX_RECT.top,
                        INPUT_BOX_RECT.width, INPUT_BOX_RECT.height, 10, 2, border);

    char disp[264];
    size_t len = strlen(kb->buf);
    if (kb->password && !kb->show_secret) {
        size_t i = 0;
        for (; i < len && i + 2 < sizeof(disp); i++) disp[i] = '*';
        disp[i++] = '_';
        disp[i] = '\0';
    } else {
        snprintf(disp, sizeof(disp), "%s_", kb->buf);
    }

    /* Tail-scroll text if wider than box */
    int max_w = INPUT_BOX_RECT.width - 28;
    const char *show = disp;
    while (*show && font_measure_utf8(FONT_STYLE_UI_REG, 24, show, strlen(show)) > max_w) {
        show++;
    }
    int asc = 0, desc = 0, lh = 0;
    font_line_metrics(FONT_STYLE_UI_REG, 24, &asc, &desc, &lh);
    int by = INPUT_BOX_RECT.top + (INPUT_BOX_RECT.height - lh) / 2 + asc;
    font_draw_utf8(c, FONT_STYLE_UI_REG, 24, INPUT_BOX_RECT.left + 14, by,
                   show, strlen(show), fg);
}

void kbd_render(Canvas *c, const KeyboardState *kb, const Config *cfg,
                bool input_only, KdRefreshMode mode) {
    if (!c || !kb) return;
    bool inv = (cfg && cfg->invert_mode);
    uint8_t bg     = inv ? 0x00 : 0xFF;
    uint8_t fg     = inv ? 0xFF : 0x00;
    uint8_t muted  = inv ? 0xAA : 0x55;
    uint8_t card   = inv ? 0x18 : 0xF5;
    uint8_t border = inv ? 0xBB : 0x22;
    uint8_t subtle = inv ? 0x44 : 0xCC;

    if (input_only) {
        render_input_box(c, kb, bg, fg, card, border);
        display_blit_canvas(c, &INPUT_BOX_RECT);
        display_refresh(&INPUT_BOX_RECT, mode);
        return;
    }

    canvas_clear(c, bg);

    /* Header */
    int asc = 0, desc = 0, lh = 0;
    font_line_metrics(FONT_STYLE_UI_BOLD, 24, &asc, &desc, &lh);
    font_draw_utf8(c, FONT_STYLE_UI_BOLD, 24, 24, 34 + asc, kb->title, strlen(kb->title), fg);
    if (kb->hint[0]) {
        font_line_metrics(FONT_STYLE_UI_REG, 17, &asc, &desc, &lh);
        font_draw_utf8(c, FONT_STYLE_UI_REG, 17, 24, 72 + asc, kb->hint, strlen(kb->hint), muted);
    }

    /* Input box */
    render_input_box(c, kb, bg, fg, card, border);

    /* Preset / Utility pills row (y=204..252) */
    const char *plabels[4], *pinserts[4];
    get_presets(kb, plabels, pinserts);
    int px = 24;
    int pw = 132;
    for (int i = 0; i < 4; i++) {
        if (plabels[i] && plabels[i][0]) {
            draw_key(c, px, 204, pw, 48, plabels[i], false, card, fg, border, 17);
        }
        px += pw + 8;
    }

    /* Character count hint */
    char cnt[48];
    snprintf(cnt, sizeof(cnt), "%zu / %zu chars", strlen(kb->buf), kb->max_len);
    font_line_metrics(FONT_STYLE_UI_REG, 15, &asc, &desc, &lh);
    font_draw_utf8(c, FONT_STYLE_UI_REG, 15, 24, 276 + asc, cnt, strlen(cnt), muted);

    canvas_hline(c, 16, 326, KD_WIDTH - 32, 2, subtle);

    /* Row 0: 10 keys (w=52, gap=6 -> 10*52 + 9*6 = 574, margin=13) */
    const char *r0 = kb->symbols ? ROW0_SYM : (kb->shift ? ROW0_UPPER : ROW0_LOWER);
    for (int i = 0; i < 10; i++) {
        char ch[2] = { r0[i], '\0' };
        draw_key(c, 13 + i * (52 + KEY_GAP), ROW0_Y, 52, KEY_H, ch, false, bg, fg, border, 26);
    }

    /* Row 1: 9 keys (centered: 9*56 + 8*6 = 552, margin=24) */
    const char *r1 = kb->symbols ? ROW1_SYM : (kb->shift ? ROW1_UPPER : ROW1_LOWER);
    for (int i = 0; i < 9; i++) {
        char ch[2] = { r1[i], '\0' };
        draw_key(c, 24 + i * (56 + KEY_GAP), ROW1_Y, 56, KEY_H, ch, false, bg, fg, border, 26);
    }

    /* Row 2: [SHIFT] + 7/8 keys + [DEL] */
    if (!kb->symbols) {
        draw_key(c, 13, ROW2_Y, 80, KEY_H, kb->shift ? "ABC" : "abc",
                 kb->shift, card, fg, border, 20);
        const char *r2 = kb->shift ? ROW2_UPPER : ROW2_LOWER;
        for (int i = 0; i < 7; i++) {
            char ch[2] = { r2[i], '\0' };
            draw_key(c, 99 + i * (52 + KEY_GAP), ROW2_Y, 52, KEY_H, ch, false, bg, fg, border, 26);
        }
        draw_key(c, 505, ROW2_Y, 82, KEY_H, "DEL", false, card, fg, border, 20);
    } else {
        for (int i = 0; i < 8; i++) {
            char ch[2] = { ROW2_SYM[i], '\0' };
            draw_key(c, 13 + i * (54 + KEY_GAP), ROW2_Y, 54, KEY_H, ch, false, bg, fg, border, 26);
        }
        draw_key(c, 493, ROW2_Y, 94, KEY_H, "DEL", false, card, fg, border, 20);
    }

    /* Row 3: [123/ABC]  [ SPACE ]  [.]  [:]  [/] */
    draw_key(c, 13, ROW3_Y, 98, KEY_H, kb->symbols ? "ABC" : "123",
             kb->symbols, card, fg, border, 21);
    draw_key(c, 117, ROW3_Y, 262, KEY_H, "SPACE", false, bg, fg, border, 20);
    draw_key(c, 385, ROW3_Y, 62, KEY_H, ".", false, bg, fg, border, 26);
    draw_key(c, 453, ROW3_Y, 62, KEY_H, kb->symbols ? "_" : ":", false, bg, fg, border, 26);
    draw_key(c, 521, ROW3_Y, 66, KEY_H, kb->symbols ? "-" : "/", false, bg, fg, border, 26);

    /* Action Row: [ CANCEL ]  [ SAVE / CONNECT ] */
    draw_key(c, 24, ACT_ROW_Y, 264, ACT_ROW_H, "Cancel", false, card, fg, border, 24);
    const char *ok_lbl = (kb->purpose == KBD_WIFI_PASS) ? "Connect" : "Save";
    draw_key(c, 312, ACT_ROW_Y, 264, ACT_ROW_H, ok_lbl, true, bg, fg, border, 24);

    display_blit_canvas(c, NULL);
    display_refresh(NULL, mode);
}

static bool hit(int x, int y, int rx, int ry, int rw, int rh) {
    return (x >= rx && x < rx + rw && y >= ry && y < ry + rh);
}

static void append_str(KeyboardState *kb, const char *s) {
    if (!s) return;
    size_t cur = strlen(kb->buf);
    while (*s && cur < kb->max_len) {
        kb->buf[cur++] = *s++;
    }
    kb->buf[cur] = '\0';
}

KbdResult kbd_handle_input(KeyboardState *kb, const InputEvent *ev) {
    if (!kb || !kb->active || !ev) return KBD_RESULT_NONE;

    if (ev->type == INPUT_HOME_SHORT) {
        kb->active = false;
        return KBD_RESULT_CANCEL;
    }
    if (ev->type != INPUT_TAP && ev->type != INPUT_LONG_TAP) return KBD_RESULT_NONE;

    int x = ev->x;
    int y = ev->y;

    /* Preset / Utility pills row (y=204..252) */
    if (y >= 198 && y < 260) {
        const char *plabels[4], *pinserts[4];
        get_presets(kb, plabels, pinserts);
        int px = 24, pw = 132;
        for (int i = 0; i < 4; i++) {
            if (plabels[i] && plabels[i][0] && hit(x, y, px, 198, pw, 60)) {
                if (i == 3) {
                    kb->buf[0] = '\0';
                    return KBD_RESULT_REDRAW_INPUT;
                }
                if (kb->password && i == 0) {
                    kb->show_secret = !kb->show_secret;
                    return KBD_RESULT_REDRAW_ALL;
                }
                if (pinserts[i]) {
                    if (kb->ntp_presets || kb->purpose == KBD_SCREENSAVER_DIR) {
                        snprintf(kb->buf, kb->max_len + 1, "%s", pinserts[i]);
                    } else {
                        append_str(kb, pinserts[i]);
                    }
                    return KBD_RESULT_REDRAW_INPUT;
                }
            }
            px += pw + 8;
        }
    }

    /* Row 0 */
    if (y >= ROW0_Y - 4 && y < ROW0_Y + KEY_H + 4) {
        const char *r0 = kb->symbols ? ROW0_SYM : (kb->shift ? ROW0_UPPER : ROW0_LOWER);
        for (int i = 0; i < 10; i++) {
            if (hit(x, y, 13 + i * (52 + KEY_GAP) - 2, ROW0_Y - 4, 56, KEY_H + 8)) {
                char s[2] = { r0[i], '\0' };
                append_str(kb, s);
                return KBD_RESULT_REDRAW_INPUT;
            }
        }
    }

    /* Row 1 */
    if (y >= ROW1_Y - 4 && y < ROW1_Y + KEY_H + 4) {
        const char *r1 = kb->symbols ? ROW1_SYM : (kb->shift ? ROW1_UPPER : ROW1_LOWER);
        for (int i = 0; i < 9; i++) {
            if (hit(x, y, 24 + i * (56 + KEY_GAP) - 2, ROW1_Y - 4, 60, KEY_H + 8)) {
                char s[2] = { r1[i], '\0' };
                append_str(kb, s);
                return KBD_RESULT_REDRAW_INPUT;
            }
        }
    }

    /* Row 2 */
    if (y >= ROW2_Y - 4 && y < ROW2_Y + KEY_H + 4) {
        if (!kb->symbols) {
            if (hit(x, y, 10, ROW2_Y - 4, 86, KEY_H + 8)) {
                kb->shift = !kb->shift;
                return KBD_RESULT_REDRAW_ALL;
            }
            if (hit(x, y, 502, ROW2_Y - 4, 90, KEY_H + 8)) {
                size_t n = strlen(kb->buf);
                if (n > 0) kb->buf[n - 1] = '\0';
                return KBD_RESULT_REDRAW_INPUT;
            }
            const char *r2 = kb->shift ? ROW2_UPPER : ROW2_LOWER;
            for (int i = 0; i < 7; i++) {
                if (hit(x, y, 99 + i * (52 + KEY_GAP) - 2, ROW2_Y - 4, 56, KEY_H + 8)) {
                    char s[2] = { r2[i], '\0' };
                    append_str(kb, s);
                    return KBD_RESULT_REDRAW_INPUT;
                }
            }
        } else {
            if (hit(x, y, 490, ROW2_Y - 4, 102, KEY_H + 8)) {
                size_t n = strlen(kb->buf);
                if (n > 0) kb->buf[n - 1] = '\0';
                return KBD_RESULT_REDRAW_INPUT;
            }
            for (int i = 0; i < 8; i++) {
                if (hit(x, y, 13 + i * (54 + KEY_GAP) - 2, ROW2_Y - 4, 58, KEY_H + 8)) {
                    char s[2] = { ROW2_SYM[i], '\0' };
                    append_str(kb, s);
                    return KBD_RESULT_REDRAW_INPUT;
                }
            }
        }
    }

    /* Row 3 */
    if (y >= ROW3_Y - 4 && y < ROW3_Y + KEY_H + 4) {
        if (hit(x, y, 10, ROW3_Y - 4, 104, KEY_H + 8)) {
            kb->symbols = !kb->symbols;
            return KBD_RESULT_REDRAW_ALL;
        }
        if (hit(x, y, 115, ROW3_Y - 4, 266, KEY_H + 8)) {
            append_str(kb, " ");
            return KBD_RESULT_REDRAW_INPUT;
        }
        if (hit(x, y, 383, ROW3_Y - 4, 66, KEY_H + 8)) {
            append_str(kb, ".");
            return KBD_RESULT_REDRAW_INPUT;
        }
        if (hit(x, y, 451, ROW3_Y - 4, 66, KEY_H + 8)) {
            append_str(kb, kb->symbols ? "_" : ":");
            return KBD_RESULT_REDRAW_INPUT;
        }
        if (hit(x, y, 519, ROW3_Y - 4, 72, KEY_H + 8)) {
            append_str(kb, kb->symbols ? "-" : "/");
            return KBD_RESULT_REDRAW_INPUT;
        }
    }

    /* Action Row */
    if (hit(x, y, 20, ACT_ROW_Y - 4, 272, ACT_ROW_H + 10)) {
        kb->active = false;
        return KBD_RESULT_CANCEL;
    }
    if (hit(x, y, 308, ACT_ROW_Y - 4, 272, ACT_ROW_H + 10)) {
        kb->active = false;
        return KBD_RESULT_SUBMIT;
    }

    return KBD_RESULT_NONE;
}
