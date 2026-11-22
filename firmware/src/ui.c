#include "kobodash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t bg;
    uint8_t fg;
    uint8_t muted;
    uint8_t card_bg;
    uint8_t border;
    uint8_t subtle;
    uint8_t pill_bg;
    uint8_t pill_fg;
} Theme;

static Theme get_theme(const Config *cfg) {
    Theme t;
    if (cfg && cfg->invert_mode) {
        t.bg      = 0x00;
        t.fg      = 0xFF;
        t.muted   = 0xAA;
        t.card_bg = 0x18;
        t.border  = 0xBB;
        t.subtle  = 0x44;
        t.pill_bg = 0xEE;
        t.pill_fg = 0x00;
    } else {
        t.bg      = 0xFF;
        t.fg      = 0x00;
        t.muted   = 0x55;
        t.card_bg = 0xF5;
        t.border  = 0x22;
        t.subtle  = 0xCC;
        t.pill_bg = 0x11;
        t.pill_fg = 0xFF;
    }
    return t;
}

/* Clock digits dirty rect (used for zero-flash partial DU updates) */
static const KdRect CLOCK_DIGITS_RECT = { 24, 172, 552, 248 };

/* Multi-tab Settings & multi-page Dashboard state */
static SettingsTab g_settings_tab = SET_TAB_GENERAL;
static const WifiState *g_wifi_state = NULL;
static const DashboardBundle *g_dash_bundle = NULL;
static int g_dash_page = 0;
static int g_clock_strip_page = 0;
static bool g_clock_strip_visible = false;
static int g_power_confirm = 0; /* 0=none, 1=reboot, 2=poweroff */
static char g_device_notice[96] = "";

void ui_set_settings_tab(SettingsTab tab) {
    if ((int)tab >= 0 && tab < SET_TAB_COUNT) {
        g_settings_tab = tab;
        g_power_confirm = 0;
    }
}
SettingsTab ui_get_settings_tab(void) { return g_settings_tab; }
void ui_set_wifi_state(const WifiState *ws) { g_wifi_state = ws; }
void ui_set_dashboard_bundle(const DashboardBundle *b, int page_idx) {
    g_dash_bundle = b;
    int maxp = (b && b->has_bundle && b->page_count > 0) ? b->page_count : 1;
    if (page_idx < 0) page_idx = 0;
    if (page_idx >= maxp) page_idx = maxp - 1;
    g_dash_page = page_idx;
}
int ui_get_dashboard_page(void) { return g_dash_page; }
void ui_set_power_confirm(int which) { g_power_confirm = which; }
int ui_get_power_confirm(void) { return g_power_confirm; }
void ui_set_device_notice(const char *msg) {
    snprintf(g_device_notice, sizeof(g_device_notice), "%s", msg ? msg : "");
}

/* Settings row geometry */
#define SET_X0        24
#define SET_W         552
#define SET_TAB_Y     64
#define SET_TAB_H     44
#define SET_Y0        118
#define SET_ROW_H     56
#define SET_ROW_G     7
#define SET_ROW_STRIDE (SET_ROW_H + SET_ROW_G)
#define SET_SRV_Y0    202
#define SET_SRV_H     54
#define SET_SRV_STRIDE 62
#define SET_WIFI_Y0   270
#define SET_WIFI_H    60
#define SET_WIFI_STRIDE 68
#define SET_WIFI_PER_PAGE 5

const char *ui_screen_name(ScreenId s) {
    switch (s) {
        case SCREEN_CLOCK:     return "CLOCK";
        case SCREEN_LIBRARY:   return "LIBRARY";
        case SCREEN_DASHBOARD: return "DASHBOARD";
        case SCREEN_STATUS:    return "SYSTEM STATUS";
        case SCREEN_GALLERY:   return "GALLERY";
        case SCREEN_SETTINGS:  return "SETTINGS";
        case SCREEN_READER:    return "READER";
        default:               return "KOBODASH";
    }
}

static bool screen_enabled(ScreenId s, const Gallery *gal) {
    if (s == SCREEN_GALLERY) {
        return (gal && gal->count > 0);
    }
    return (s >= SCREEN_CLOCK && s <= SCREEN_STATUS);
}

ScreenId ui_next_screen(ScreenId cur, int delta, const Gallery *gal) {
    ScreenId order[5] = { SCREEN_CLOCK, SCREEN_LIBRARY, SCREEN_DASHBOARD, SCREEN_STATUS, SCREEN_GALLERY };
    int enabled_list[5];
    int n = 0;
    for (int i = 0; i < 5; i++) {
        if (screen_enabled(order[i], gal)) {
            enabled_list[n++] = order[i];
        }
    }
    if (n <= 0) return SCREEN_CLOCK;

    int idx = 0;
    for (int i = 0; i < n; i++) {
        if (enabled_list[i] == (int)cur) {
            idx = i;
            break;
        }
    }
    idx = (idx + delta) % n;
    if (idx < 0) idx += n;
    return (ScreenId)enabled_list[idx];
}

void ui_step_carousel(ScreenId cur, int cur_dash_page, int delta,
                      const Gallery *gal, const DashboardBundle *b,
                      bool skip_library, ScreenId *out_screen, int *out_dash_page) {
    int npages = (b && b->has_bundle && b->page_count > 0) ? b->page_count : 1;
    int dp = cur_dash_page;
    if (dp < 0) dp = 0;
    if (dp >= npages) dp = npages - 1;

    if (cur == SCREEN_DASHBOARD) {
        if (delta > 0 && dp + 1 < npages) {
            *out_screen = SCREEN_DASHBOARD;
            *out_dash_page = dp + 1;
            return;
        }
        if (delta < 0 && dp > 0) {
            *out_screen = SCREEN_DASHBOARD;
            *out_dash_page = dp - 1;
            return;
        }
    }

    ScreenId nxt = cur;
    do {
        nxt = ui_next_screen(nxt, delta >= 0 ? +1 : -1, gal);
    } while (skip_library && nxt == SCREEN_LIBRARY);

    *out_screen = nxt;
    if (nxt == SCREEN_DASHBOARD) {
        *out_dash_page = (delta < 0) ? (npages - 1) : 0;
    } else {
        *out_dash_page = dp;
    }
}

static void fmt_interval_short(int mins, char *buf, size_t sz) {
    if (mins <= 0) snprintf(buf, sz, "OFF");
    else if (mins < 60) snprintf(buf, sz, "%dm", mins);
    else snprintf(buf, sz, "%dh", mins / 60);
}

static void fmt_uptime(long sec, char *buf, size_t sz) {
    if (sec < 0) sec = 0;
    long d = sec / 86400;
    long h = (sec % 86400) / 3600;
    long m = (sec % 3600) / 60;
    if (d > 0) snprintf(buf, sz, "%ldd %ldh %ldm", d, h, m);
    else if (h > 0) snprintf(buf, sz, "%ldh %ldm", h, m);
    else snprintf(buf, sz, "%ld min", m);
}

static void draw_top_bar_shapes(Canvas *c, const Theme *th, bool show_done_btn) {
    canvas_hline(c, 24, 58, 552, 2, th->subtle);
    if (show_done_btn) {
        canvas_fill_rrect(c, 468, 12, 108, 36, 10, th->pill_bg);
    } else {
        canvas_draw_refresh_icon(c, 504, 30, 11, th->fg, th->bg);
        canvas_draw_gear(c, 558, 30, 10, th->fg, th->bg);
    }
}

static void draw_top_bar_text(const char *title, const SyncState *sync,
                              const Theme *th, bool show_done_btn) {
    char left_buf[96];
    if (sync && sync->in_progress) {
        snprintf(left_buf, sizeof(left_buf), "%s  •  SYNCING...", title);
    } else {
        snprintf(left_buf, sizeof(left_buf), "%s", title);
    }
    display_text(left_buf, KD_FONT_BOLD, 20, KD_ALIGN_LEFT,
                 24, 10, 420, 40, th->fg, true, th->bg);
    if (show_done_btn) {
        display_text("DONE", KD_FONT_BOLD, 18, KD_ALIGN_CENTER,
                     468, 12, 108, 36, th->pill_fg, true, th->pill_bg);
    }
}

static void draw_bottom_nav_shapes(Canvas *c, ScreenId active, const Gallery *gal, const Theme *th) {
    canvas_hline(c, 24, 734, 552, 1, th->subtle);

    int npages = (g_dash_bundle && g_dash_bundle->has_bundle && g_dash_bundle->page_count > 0)
                     ? g_dash_bundle->page_count : 1;
    int has_gal = (gal && gal->count > 0) ? 1 : 0;
    int count = 3 + npages + has_gal; /* Clock, Library, DashPages..., Status, [Gallery] */
    int active_idx = 0;
    if (active == SCREEN_CLOCK) active_idx = 0;
    else if (active == SCREEN_LIBRARY) active_idx = 1;
    else if (active == SCREEN_DASHBOARD) active_idx = 2 + g_dash_page;
    else if (active == SCREEN_STATUS) active_idx = 2 + npages;
    else if (active == SCREEN_GALLERY) active_idx = 3 + npages;

    int spacing = (count > 7) ? 18 : 22;
    int total_w = (count - 1) * spacing;
    int start_x = (KD_WIDTH - total_w) / 2;
    int cy = 758;
    for (int i = 0; i < count; i++) {
        if (i == active_idx) {
            canvas_fill_circle(c, start_x + i * spacing, cy, 5, th->fg);
        } else {
            canvas_stroke_circle(c, start_x + i * spacing, cy, 5, 2, th->muted);
        }
    }
}

static void draw_bottom_nav_text(const Theme *th) {
    display_text("< PREV", KD_FONT_BOLD, 17, KD_ALIGN_LEFT,
                 24, 744, 120, 44, th->muted, true, th->bg);
    display_text("NEXT >", KD_FONT_BOLD, 17, KD_ALIGN_RIGHT,
                 456, 744, 120, 44, th->muted, true, th->bg);
    display_text("Hold HOME 2s for Stock UI", KD_FONT_REGULAR, 14, KD_ALIGN_CENTER,
                 150, 770, 300, 24, th->muted, true, th->bg);
}

static void format_clock_str(const Config *cfg, const struct tm *tmv,
                             char *time_buf, size_t time_sz,
                             char *ampm_buf, size_t ampm_sz) {
    int h = tmv->tm_hour;
    ampm_buf[0] = '\0';
    if (!cfg->clock_24h) {
        snprintf(ampm_buf, ampm_sz, "%s", (h >= 12) ? "PM" : "AM");
        h = h % 12;
        if (h == 0) h = 12;
        snprintf(time_buf, time_sz, "%d:%02d", h, tmv->tm_min);
    } else {
        snprintf(time_buf, time_sz, "%02d:%02d", h, tmv->tm_min);
    }
}

static void draw_clock_digits_text(const Config *cfg, const struct tm *tmv, const Theme *th) {
    char tbuf[16];
    char ampm[8];
    format_clock_str(cfg, tmv, tbuf, sizeof(tbuf), ampm, sizeof(ampm));

    display_text(tbuf, KD_FONT_CLOCK, 196, KD_ALIGN_CENTER,
                 CLOCK_DIGITS_RECT.left, CLOCK_DIGITS_RECT.top,
                 CLOCK_DIGITS_RECT.width, ampm[0] ? 204 : CLOCK_DIGITS_RECT.height,
                 th->fg, true, th->bg);
    if (ampm[0]) {
        display_text(ampm, KD_FONT_BOLD, 24, KD_ALIGN_CENTER,
                     CLOCK_DIGITS_RECT.left, CLOCK_DIGITS_RECT.top + 206,
                     CLOCK_DIGITS_RECT.width, 36,
                     th->muted, true, th->bg);
    }
}

void ui_render_clock_digits_only(Canvas *c, const Config *cfg, const struct tm *now_tm,
                                 KdRefreshMode mode) {
    Theme th = get_theme(cfg);
    canvas_fill_rect(c, CLOCK_DIGITS_RECT.left, CLOCK_DIGITS_RECT.top,
                     CLOCK_DIGITS_RECT.width, CLOCK_DIGITS_RECT.height, th.bg);
    display_blit_canvas(c, &CLOCK_DIGITS_RECT);
    draw_clock_digits_text(cfg, now_tm, &th);
    display_refresh(&CLOCK_DIGITS_RECT, mode);
}

static void render_clock_screen(Canvas *c, const Config *cfg, const Telemetry *tel,
                                const SyncState *sync, const Gallery *gal,
                                const struct tm *now_tm, KdRefreshMode mode) {
    Theme th = get_theme(cfg);
    canvas_clear(c, th.bg);
    draw_top_bar_shapes(c, &th, false);

    /* 1-tap Books pill in top bar */
    canvas_fill_rrect(c, 368, 12, 102, 36, 10, th.card_bg);
    canvas_stroke_rrect(c, 368, 12, 102, 36, 10, 2, th.border);

    /* Subtle divider below clock */
    canvas_hline(c, 240, 444, 120, 3, th.fg);

    /* Optional subtle live event / waiting-on-you pill strip */
    char strip_txt[96] = {0};
    int strip_page = 0;
    g_clock_strip_visible = dashboard_clock_strip(g_dash_bundle, time(NULL),
                                                  strip_txt, sizeof(strip_txt), &strip_page);
    g_clock_strip_page = strip_page;
    if (g_clock_strip_visible) {
        canvas_fill_rrect(c, 48, 550, 504, 34, 10, th.card_bg);
        canvas_stroke_rrect(c, 48, 550, 504, 34, 10, 1, th.border);
    }

    /* Bottom telemetry card */
    canvas_fill_rrect(c, 28, 594, 544, 108, 14, th.card_bg);
    canvas_stroke_rrect(c, 28, 594, 544, 108, 14, 2, th.border);
    canvas_vline(c, 206, 612, 72, 1, th.subtle);
    canvas_vline(c, 398, 612, 72, 1, th.subtle);

    canvas_draw_battery(c, 50, 620, tel->bat_pct, tel->charger_online, th.fg, th.card_bg);
    canvas_draw_wifi(c, 226, 620, tel->wifi_up, tel->wifi_rssi, th.fg, th.subtle);

    draw_bottom_nav_shapes(c, SCREEN_CLOCK, gal, &th);
    display_blit_canvas(c, NULL);

    draw_top_bar_text("KOBODASH", sync, &th, false);
    display_text("BOOKS", KD_FONT_BOLD, 16, KD_ALIGN_CENTER,
                 368, 12, 102, 36, th.fg, true, th.card_bg);
    draw_clock_digits_text(cfg, now_tm, &th);

    /* Date block */
    char dow[32], date_str[64];
    strftime(dow, sizeof(dow), "%A", now_tm);
    for (char *p = dow; *p; p++) {
        if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
    }
    strftime(date_str, sizeof(date_str), "%d %B %Y", now_tm);
    if (now_tm->tm_year + 1900 < 2020) {
        snprintf(date_str, sizeof(date_str), "Clock not synced yet");
    }

    display_text(dow, KD_FONT_BOLD, 30, KD_ALIGN_CENTER,
                 24, 466, 552, 42, th.fg, true, th.bg);
    display_text(date_str, KD_FONT_REGULAR, 24, KD_ALIGN_CENTER,
                 24, 512, 552, 34, th.muted, true, th.bg);

    if (g_clock_strip_visible) {
        display_text(strip_txt, KD_FONT_BOLD, 16, KD_ALIGN_CENTER,
                     56, 550, 488, 34, th.fg, true, th.card_bg);
    }

    /* Card col 1: Battery */
    char buf[64];
    if (tel->bat_pct >= 0) snprintf(buf, sizeof(buf), "%d%%", tel->bat_pct);
    else snprintf(buf, sizeof(buf), "N/A");
    display_text(buf, KD_FONT_BOLD, 24, KD_ALIGN_LEFT,
                 96, 612, 102, 34, th.fg, true, th.card_bg);
    display_text(tel->charger_online ? "Charging" : tel->bat_status,
                 KD_FONT_REGULAR, 16, KD_ALIGN_LEFT,
                 50, 654, 148, 28, th.muted, true, th.card_bg);

    /* Card col 2: Wi-Fi */
    if (tel->wifi_up && tel->wifi_rssi < 0) {
        snprintf(buf, sizeof(buf), "%d dBm", tel->wifi_rssi);
    } else if (tel->wifi_up) {
        snprintf(buf, sizeof(buf), "Online");
    } else {
        snprintf(buf, sizeof(buf), "Offline");
    }
    display_text(buf, KD_FONT_BOLD, 21, KD_ALIGN_LEFT,
                 260, 614, 130, 32, th.fg, true, th.card_bg);
    display_text(tel->wifi_ip, KD_FONT_REGULAR, 15, KD_ALIGN_LEFT,
                 226, 654, 164, 28, th.muted, true, th.card_bg);

    /* Card col 3: Active schedule */
    int active_min = config_active_interval_min(cfg, now_tm);
    char int_s[16];
    fmt_interval_short(active_min, int_s, sizeof(int_s));
    if (cfg->minute_clock) {
        snprintf(buf, sizeof(buf), "1m LIVE");
    } else {
        snprintf(buf, sizeof(buf), "%s %s", config_is_daytime(cfg, now_tm) ? "DAY" : "NIGHT", int_s);
    }
    display_text(buf, KD_FONT_BOLD, 20, KD_ALIGN_CENTER,
                 406, 614, 156, 32, th.fg, true, th.card_bg);

    char sub[48];
    if (cfg->auto_rotate_min > 0) {
        char rot_s[16];
        fmt_interval_short(cfg->auto_rotate_min, rot_s, sizeof(rot_s));
        snprintf(sub, sizeof(sub), "Sync %s • Rot %s", int_s, rot_s);
    } else {
        snprintf(sub, sizeof(sub), "Sync %s (%s)", int_s,
                 config_is_daytime(cfg, now_tm) ? "Day" : "Night");
    }
    display_text(sub, KD_FONT_REGULAR, 15, KD_ALIGN_CENTER,
                 406, 654, 156, 28, th.muted, true, th.card_bg);

    draw_bottom_nav_text(&th);
    display_refresh(NULL, mode);
}

static void render_status_screen(Canvas *c, const Config *cfg, const Telemetry *tel,
                                 const SyncState *sync, const Gallery *gal,
                                 const struct tm *now_tm, KdRefreshMode mode) {
    Theme th = get_theme(cfg);
    canvas_clear(c, th.bg);
    draw_top_bar_shapes(c, &th, false);

    /* 2x2 grid + bottom full-width card */
    const int c1x = 24, c2x = 308, cw = 268;
    const int r1y = 76, r1h = 192;
    const int r2y = 284, r2h = 198;
    const int r3y = 498, r3h = 216;

    canvas_fill_rrect(c, c1x, r1y, cw, r1h, 12, th.card_bg);
    canvas_stroke_rrect(c, c1x, r1y, cw, r1h, 12, 2, th.border);

    canvas_fill_rrect(c, c2x, r1y, cw, r1h, 12, th.card_bg);
    canvas_stroke_rrect(c, c2x, r1y, cw, r1h, 12, 2, th.border);

    canvas_fill_rrect(c, c1x, r2y, cw, r2h, 12, th.card_bg);
    canvas_stroke_rrect(c, c1x, r2y, cw, r2h, 12, 2, th.border);

    canvas_fill_rrect(c, c2x, r2y, cw, r2h, 12, th.card_bg);
    canvas_stroke_rrect(c, c2x, r2y, cw, r2h, 12, 2, th.border);

    canvas_fill_rrect(c, 24, r3y, 552, r3h, 12, th.card_bg);
    canvas_stroke_rrect(c, 24, r3y, 552, r3h, 12, 2, th.border);

    /* Progress bars */
    canvas_draw_progress(c, c1x + 18, r1y + 110, cw - 36, 14,
                         tel->bat_pct >= 0 ? tel->bat_pct : 0, th.fg, th.bg, th.border);
    canvas_draw_wifi(c, c2x + 18, r1y + 46, tel->wifi_up, tel->wifi_rssi, th.fg, th.subtle);

    int ram_pct = 0;
    unsigned long mem_used_mb = 0, mem_tot_mb = 0;
    if (tel->mem_total_kb > 0) {
        unsigned long used_kb = (tel->mem_total_kb > tel->mem_avail_kb)
                                    ? (tel->mem_total_kb - tel->mem_avail_kb) : 0;
        ram_pct = (int)(used_kb * 100UL / tel->mem_total_kb);
        mem_used_mb = used_kb / 1024UL;
        mem_tot_mb = tel->mem_total_kb / 1024UL;
    }
    canvas_draw_progress(c, c1x + 18, r2y + 78, cw - 36, 12, ram_pct, th.fg, th.bg, th.border);

    int ob_pct = 0;
    if (tel->onboard_total_mb > 0) {
        unsigned long used = tel->onboard_total_mb - tel->onboard_free_mb;
        ob_pct = (int)(used * 100UL / tel->onboard_total_mb);
    }
    canvas_draw_progress(c, c2x + 18, r2y + 78, cw - 36, 12, ob_pct, th.fg, th.bg, th.border);

    draw_bottom_nav_shapes(c, SCREEN_STATUS, gal, &th);
    display_blit_canvas(c, NULL);

    draw_top_bar_text("SYSTEM STATUS", sync, &th, false);

    char buf[128];

    /* Card 1: Battery */
    display_text("BATTERY", KD_FONT_BOLD, 16, KD_ALIGN_LEFT,
                 c1x + 18, r1y + 14, cw - 36, 24, th.muted, true, th.card_bg);
    if (tel->bat_pct >= 0) snprintf(buf, sizeof(buf), "%d%%", tel->bat_pct);
    else snprintf(buf, sizeof(buf), "N/A");
    display_text(buf, KD_FONT_CLOCK, 54, KD_ALIGN_LEFT,
                 c1x + 18, r1y + 42, 140, 60, th.fg, true, th.card_bg);
    display_text(tel->charger_online ? "CHARGING" : tel->bat_status,
                 KD_FONT_BOLD, 16, KD_ALIGN_RIGHT,
                 c1x + 130, r1y + 56, 120, 32, th.muted, true, th.card_bg);
    if (tel->bat_mv > 0) {
        snprintf(buf, sizeof(buf), "%.2f V   •   %d mA", tel->bat_mv / 1000.0, tel->bat_ma);
    } else {
        snprintf(buf, sizeof(buf), "Power telemetry ready");
    }
    display_text(buf, KD_FONT_REGULAR, 17, KD_ALIGN_LEFT,
                 c1x + 18, r1y + 140, cw - 36, 30, th.fg, true, th.card_bg);

    /* Card 2: Network */
    display_text("NETWORK", KD_FONT_BOLD, 16, KD_ALIGN_LEFT,
                 c2x + 18, r1y + 14, cw - 36, 24, th.muted, true, th.card_bg);
    display_text(tel->wifi_up ? "Wi-Fi Connected" : "Wi-Fi Offline",
                 KD_FONT_BOLD, 20, KD_ALIGN_LEFT,
                 c2x + 52, r1y + 42, cw - 68, 28, th.fg, true, th.card_bg);
    snprintf(buf, sizeof(buf), "IP: %s", tel->wifi_ip);
    display_text(buf, KD_FONT_REGULAR, 17, KD_ALIGN_LEFT,
                 c2x + 18, r1y + 82, cw - 36, 26, th.fg, true, th.card_bg);
    if (tel->wifi_up && tel->wifi_rssi < 0) {
        snprintf(buf, sizeof(buf), "Signal: %d dBm%s%s",
                 tel->wifi_rssi,
                 tel->wifi_ssid[0] ? " • " : "",
                 tel->wifi_ssid[0] ? tel->wifi_ssid : "");
    } else if (tel->wifi_ssid[0]) {
        snprintf(buf, sizeof(buf), "SSID: %s", tel->wifi_ssid);
    } else {
        snprintf(buf, sizeof(buf), "Signal: --");
    }
    display_text(buf, KD_FONT_REGULAR, 16, KD_ALIGN_LEFT,
                 c2x + 18, r1y + 112, cw - 36, 26, th.muted, true, th.card_bg);
    snprintf(buf, sizeof(buf), "SSH :2222  %s", tel->ssh_running ? "ACTIVE" : "OFF");
    display_text(buf, KD_FONT_BOLD, 16, KD_ALIGN_LEFT,
                 c2x + 18, r1y + 146, cw - 36, 26, th.fg, true, th.card_bg);

    /* Card 3: System & Memory */
    display_text("MEMORY & CPU", KD_FONT_BOLD, 16, KD_ALIGN_LEFT,
                 c1x + 18, r2y + 14, cw - 36, 24, th.muted, true, th.card_bg);
    snprintf(buf, sizeof(buf), "RAM: %lu / %lu MB", mem_used_mb, mem_tot_mb);
    display_text(buf, KD_FONT_BOLD, 19, KD_ALIGN_LEFT,
                 c1x + 18, r2y + 44, cw - 36, 28, th.fg, true, th.card_bg);
    snprintf(buf, sizeof(buf), "KoboDash RSS: %.1f MB", tel->self_rss_kb / 1024.0);
    display_text(buf, KD_FONT_REGULAR, 16, KD_ALIGN_LEFT,
                 c1x + 18, r2y + 100, cw - 36, 26, th.fg, true, th.card_bg);
    snprintf(buf, sizeof(buf), "Load: %.2f  %.2f  %.2f", tel->load1, tel->load5, tel->load15);
    display_text(buf, KD_FONT_REGULAR, 16, KD_ALIGN_LEFT,
                 c1x + 18, r2y + 128, cw - 36, 26, th.muted, true, th.card_bg);
    char upbuf[32];
    fmt_uptime(tel->uptime_sec, upbuf, sizeof(upbuf));
    if (tel->cpu_khz > 0) {
        snprintf(buf, sizeof(buf), "%d MHz  •  Up %s", tel->cpu_khz / 1000, upbuf);
    } else {
        snprintf(buf, sizeof(buf), "Up %s", upbuf);
    }
    display_text(buf, KD_FONT_REGULAR, 16, KD_ALIGN_LEFT,
                 c1x + 18, r2y + 156, cw - 36, 26, th.muted, true, th.card_bg);

    /* Card 4: Storage */
    display_text("STORAGE & SD", KD_FONT_BOLD, 16, KD_ALIGN_LEFT,
                 c2x + 18, r2y + 14, cw - 36, 24, th.muted, true, th.card_bg);
    snprintf(buf, sizeof(buf), "Onboard: %lu MB free", tel->onboard_free_mb);
    display_text(buf, KD_FONT_BOLD, 18, KD_ALIGN_LEFT,
                 c2x + 18, r2y + 44, cw - 36, 28, th.fg, true, th.card_bg);
    if (tel->sd_mounted) {
        snprintf(buf, sizeof(buf), "MicroSD: %lu / %lu MB free", tel->sd_free_mb, tel->sd_total_mb);
    } else {
        snprintf(buf, sizeof(buf), "MicroSD: Not inserted");
    }
    display_text(buf, KD_FONT_REGULAR, 16, KD_ALIGN_LEFT,
                 c2x + 18, r2y + 100, cw - 36, 26, th.fg, true, th.card_bg);
    snprintf(buf, sizeof(buf), "Gallery images: %d", gal ? gal->count : 0);
    display_text(buf, KD_FONT_REGULAR, 16, KD_ALIGN_LEFT,
                 c2x + 18, r2y + 128, cw - 36, 26, th.muted, true, th.card_bg);
    snprintf(buf, sizeof(buf), "Touch A • Linux 2.6.35");
    display_text(buf, KD_FONT_REGULAR, 15, KD_ALIGN_LEFT,
                 c2x + 18, r2y + 156, cw - 36, 26, th.muted, true, th.card_bg);

    /* Card 5: Schedule & Server */
    display_text("SCHEDULE & SERVER SYNC", KD_FONT_BOLD, 16, KD_ALIGN_LEFT,
                 42, r3y + 14, 516, 24, th.muted, true, th.card_bg);

    char d_int[16], n_int[16], r_int[16];
    fmt_interval_short(cfg->day_interval_min, d_int, sizeof(d_int));
    fmt_interval_short(cfg->night_interval_min, n_int, sizeof(n_int));
    fmt_interval_short(cfg->auto_rotate_min, r_int, sizeof(r_int));
    snprintf(buf, sizeof(buf), "Day (%02d-%02d): %s   •   Night: %s   •   Rotate: %s",
             cfg->day_start_hour, cfg->night_start_hour, d_int, n_int, r_int);
    display_text(buf, KD_FONT_BOLD, 17, KD_ALIGN_LEFT,
                 42, r3y + 46, 516, 28, th.fg, true, th.card_bg);

    snprintf(buf, sizeof(buf), "Minute Clock: %s   •   Full Flash: every %d updates",
             cfg->minute_clock ? "ON (Fast DU)" : "OFF", cfg->full_refresh_every);
    display_text(buf, KD_FONT_REGULAR, 16, KD_ALIGN_LEFT,
                 42, r3y + 80, 516, 26, th.muted, true, th.card_bg);

    snprintf(buf, sizeof(buf), "Server: %s",
             cfg->server_url[0] ? cfg->server_url : "(Not configured — edit config.ini)");
    display_text(buf, KD_FONT_REGULAR, 16, KD_ALIGN_LEFT,
                 42, r3y + 114, 516, 26, th.fg, true, th.card_bg);

    char last_t[24] = "--:--";
    if (sync && sync->last_sync_time > 0) {
        struct tm lt;
        localtime_r(&sync->last_sync_time, &lt);
        strftime(last_t, sizeof(last_t), "%H:%M:%S", &lt);
    }
    char next_t[24] = "--:--";
    if (sync && sync->next_sync_time > 0) {
        struct tm nt;
        localtime_r(&sync->next_sync_time, &nt);
        strftime(next_t, sizeof(next_t), "%H:%M", &nt);
    }
    snprintf(buf, sizeof(buf), "Last Sync: %s (%s)",
             last_t, (sync && sync->last_status[0]) ? sync->last_status : "Not synced");
    display_text(buf, KD_FONT_REGULAR, 16, KD_ALIGN_LEFT,
                 42, r3y + 146, 516, 26, th.muted, true, th.card_bg);

    char now_s[32];
    strftime(now_s, sizeof(now_s), "%H:%M", now_tm);
    snprintf(buf, sizeof(buf), "Now: %s   •   Next Sync: %s   •   NTP: %s",
             now_s, next_t, (sync && sync->time_synced) ? "OK" : "Pending");
    display_text(buf, KD_FONT_REGULAR, 16, KD_ALIGN_LEFT,
                 42, r3y + 176, 516, 26, th.muted, true, th.card_bg);

    draw_bottom_nav_text(&th);
    display_refresh(NULL, mode);
}

static void render_dashboard_screen(Canvas *c, const Config *cfg, const Telemetry *tel,
                                    const SyncState *sync, const Gallery *gal,
                                    const struct tm *now_tm, KdRefreshMode mode) {
    Theme th = get_theme(cfg);

    /* Case 0: Multi-page native JSON bundle (custom server) */
    if (g_dash_bundle && g_dash_bundle->has_bundle && g_dash_bundle->page_count > 0 &&
        (!sync || !sync->has_cached_image || sync->has_bundle_json || sync->is_json_dashboard)) {
        dashboard_render_page(c, g_dash_bundle, g_dash_page, cfg, tel, sync, gal, now_tm, mode);
        return;
    }

    /* Case 1: Cached dashboard image exists -> crisp full-screen display */
    if (sync && sync->has_cached_image && !sync->is_json_dashboard && sync->cache_image_path[0]) {
        canvas_clear(c, th.bg);
        display_blit_canvas(c, NULL);
        if (display_image_file(sync->cache_image_path, 0, 0, KD_WIDTH, KD_HEIGHT,
                               true, cfg->invert_mode != 0) == 0) {
            if (sync->in_progress) {
                canvas_fill_rrect(c, 420, 12, 164, 34, 8, th.pill_bg);
                KdRect badge = { 420, 12, 164, 34 };
                display_blit_canvas(c, &badge);
                display_text("SYNCING...", KD_FONT_BOLD, 16, KD_ALIGN_CENTER,
                             420, 12, 164, 34, th.pill_fg, true, th.pill_bg);
            }
            display_refresh(NULL, mode);
            return;
        }
    }

    /* Case 2: Native JSON dashboard from user's server */
    if (sync && sync->is_json_dashboard) {
        canvas_clear(c, th.bg);
        draw_top_bar_shapes(c, &th, false);
        canvas_fill_rrect(c, 24, 78, 552, 636, 14, th.card_bg);
        canvas_stroke_rrect(c, 24, 78, 552, 636, 14, 2, th.border);
        canvas_hline(c, 44, 178, 512, 2, th.subtle);
        draw_bottom_nav_shapes(c, SCREEN_DASHBOARD, gal, &th);
        display_blit_canvas(c, NULL);

        draw_top_bar_text("DASHBOARD", sync, &th, false);
        display_text(sync->json_title, KD_FONT_BOLD, 34, KD_ALIGN_LEFT,
                     44, 94, 512, 46, th.fg, true, th.card_bg);
        if (sync->json_subtitle[0]) {
            display_text(sync->json_subtitle, KD_FONT_REGULAR, 20, KD_ALIGN_LEFT,
                         44, 142, 512, 30, th.muted, true, th.card_bg);
        }
        for (int i = 0; i < sync->json_line_count && i < KD_MAX_JSON_LINES; i++) {
            display_text(sync->json_lines[i], KD_FONT_REGULAR, 22, KD_ALIGN_LEFT,
                         44, 196 + i * 48, 512, 40, th.fg, true, th.card_bg);
        }
        draw_bottom_nav_text(&th);
        display_refresh(NULL, mode);
        return;
    }

    /* Case 3: Setup / Offline placeholder card */
    canvas_clear(c, th.bg);
    draw_top_bar_shapes(c, &th, false);

    canvas_fill_rrect(c, 36, 130, 528, 490, 16, th.card_bg);
    canvas_stroke_rrect(c, 36, 130, 528, 490, 16, 2, th.border);
    /* Two action buttons inside card: Server/Sync + Wi-Fi Settings */
    canvas_fill_rrect(c, 64, 504, 224, 58, 12, th.pill_bg);
    canvas_fill_rrect(c, 312, 504, 224, 58, 12, th.bg);
    canvas_stroke_rrect(c, 312, 504, 224, 58, 12, 2, th.border);

    draw_bottom_nav_shapes(c, SCREEN_DASHBOARD, gal, &th);
    display_blit_canvas(c, NULL);

    draw_top_bar_text("SERVER DASHBOARD", sync, &th, false);

    display_text(cfg->server_url[0] ? "Server View Ready" : "Connect Your Server",
                 KD_FONT_BOLD, 32, KD_ALIGN_CENTER,
                 56, 164, 488, 46, th.fg, true, th.card_bg);

    char buf[160];
    if (cfg->server_url[0]) {
        snprintf(buf, sizeof(buf), "Endpoint: %.54s", cfg->server_url);
    } else {
        snprintf(buf, sizeof(buf), "Tap Server Setup below to enter your HTTPS/HTTP URL");
    }
    display_text(buf, KD_FONT_REGULAR, 19, KD_ALIGN_CENTER,
                 56, 232, 488, 48, th.fg, true, th.card_bg);

    display_text("Supports HTTPS/HTTP servers returning multi-page\nJSON bundles or 600x800 PNG / JPG / BMP images.",
                 KD_FONT_REGULAR, 18, KD_ALIGN_CENTER,
                 56, 294, 488, 56, th.muted, true, th.card_bg);

    snprintf(buf, sizeof(buf), "Status: %s",
             (sync && sync->last_status[0]) ? sync->last_status : "Not synced yet");
    display_text(buf, KD_FONT_BOLD, 18, KD_ALIGN_CENTER,
                 56, 372, 488, 34, th.fg, true, th.card_bg);

    char d_int[16], n_int[16];
    fmt_interval_short(cfg->day_interval_min, d_int, sizeof(d_int));
    fmt_interval_short(cfg->night_interval_min, n_int, sizeof(n_int));
    snprintf(buf, sizeof(buf), "Wi-Fi: %s  •  Day %s / Night %s",
             tel->wifi_ip, d_int, n_int);
    display_text(buf, KD_FONT_REGULAR, 17, KD_ALIGN_CENTER,
                 56, 420, 488, 34, th.muted, true, th.card_bg);

    const char *left_btn = cfg->server_url[0]
                               ? (sync && sync->in_progress ? "SYNCING..." : "SYNC NOW")
                               : "SERVER SETUP";
    display_text(left_btn, KD_FONT_BOLD, 20, KD_ALIGN_CENTER,
                 64, 504, 224, 58, th.pill_fg, true, th.pill_bg);
    display_text("WI-FI SETUP", KD_FONT_BOLD, 20, KD_ALIGN_CENTER,
                 312, 504, 224, 58, th.fg, true, th.bg);

    (void)now_tm;
    draw_bottom_nav_text(&th);
    display_refresh(NULL, mode);
}

static void render_gallery_screen(Canvas *c, const Config *cfg, const Gallery *gal,
                                  const struct tm *now_tm, KdRefreshMode mode) {
    Theme th = get_theme(cfg);
    const char *img = gallery_current(gal);

    if (img) {
        canvas_clear(c, th.bg);
        display_blit_canvas(c, NULL);
        if (display_image_file(img, 0, 0, KD_WIDTH, KD_HEIGHT, true, false) == 0) {
            if (cfg->gallery_clock_overlay) {
                char tbuf[16], ampm[8];
                format_clock_str(cfg, now_tm, tbuf, sizeof(tbuf), ampm, sizeof(ampm));
                canvas_fill_rrect(c, 16, 16, 128, 44, 10, 0xFF);
                canvas_stroke_rrect(c, 16, 16, 128, 44, 10, 2, 0x00);
                KdRect pill = { 16, 16, 128, 44 };
                display_blit_canvas(c, &pill);
                display_text(tbuf, KD_FONT_CLOCK, 32, KD_ALIGN_CENTER,
                             16, 16, 128, 44, 0x00, true, 0xFF);
            }
            display_refresh(NULL, mode);
            return;
        }
    }

    /* Empty gallery fallback */
    canvas_clear(c, th.bg);
    draw_top_bar_shapes(c, &th, false);
    canvas_fill_rrect(c, 40, 200, 520, 300, 14, th.card_bg);
    canvas_stroke_rrect(c, 40, 200, 520, 300, 14, 2, th.border);
    draw_bottom_nav_shapes(c, SCREEN_GALLERY, gal, &th);
    display_blit_canvas(c, NULL);
    draw_top_bar_text("GALLERY", NULL, &th, false);
    display_text("No Screensaver Images Found", KD_FONT_BOLD, 26, KD_ALIGN_CENTER,
                 56, 250, 488, 44, th.fg, true, th.card_bg);
    display_text("Add PNG or JPG files to:\n/mnt/sd/screensavers\nor .adds/kobodash/screensavers",
                 KD_FONT_REGULAR, 19, KD_ALIGN_CENTER,
                 56, 320, 488, 120, th.muted, true, th.card_bg);
    draw_bottom_nav_text(&th);
    display_refresh(NULL, mode);
}

static void draw_tab_bar_shapes(Canvas *c, const Theme *th) {
    for (int i = 0; i < SET_TAB_COUNT; i++) {
        int tx = 24 + i * 140;
        int tw = (i == SET_TAB_COUNT - 1) ? 132 : 132;
        if ((SettingsTab)i == g_settings_tab) {
            canvas_fill_rrect(c, tx, SET_TAB_Y, tw, SET_TAB_H, 10, th->pill_bg);
        } else {
            canvas_fill_rrect(c, tx, SET_TAB_Y, tw, SET_TAB_H, 10, th->card_bg);
            canvas_stroke_rrect(c, tx, SET_TAB_Y, tw, SET_TAB_H, 10, 2, th->border);
        }
    }
}

static void draw_tab_bar_text(const Theme *th) {
    static const char *names[SET_TAB_COUNT] = { "GENERAL", "WI-FI", "SERVER", "DEVICE" };
    for (int i = 0; i < SET_TAB_COUNT; i++) {
        int tx = 24 + i * 140;
        bool active = ((SettingsTab)i == g_settings_tab);
        display_text(names[i], KD_FONT_BOLD, 17, KD_ALIGN_CENTER,
                     tx, SET_TAB_Y, 132, SET_TAB_H,
                     active ? th->pill_fg : th->fg,
                     true, active ? th->pill_bg : th->card_bg);
    }
}

static void draw_row_box(Canvas *c, int y, int h, int pill_x, int pill_w,
                         bool active_pill, const Theme *th) {
    canvas_fill_rrect(c, SET_X0, y, SET_W, h, 10, th->card_bg);
    canvas_stroke_rrect(c, SET_X0, y, SET_W, h, 10, 1, th->border);
    if (pill_w > 0) {
        int py = y + 8;
        int ph = h - 16;
        if (active_pill) {
            canvas_fill_rrect(c, pill_x, py, pill_w, ph, 9, th->pill_bg);
        } else {
            canvas_fill_rrect(c, pill_x, py, pill_w, ph, 9, th->bg);
            canvas_stroke_rrect(c, pill_x, py, pill_w, ph, 9, 2, th->border);
        }
    }
}

static void draw_row_text(int y, int h, int pill_x, int pill_w,
                          const char *label, const char *val,
                          bool active_pill, const Theme *th) {
    int label_w = (pill_w > 0) ? (pill_x - SET_X0 - 24) : (SET_W - 36);
    display_text(label, KD_FONT_BOLD, 19, KD_ALIGN_LEFT,
                 SET_X0 + 16, y + 4, label_w, h - 8, th->fg, true, th->card_bg);
    if (pill_w > 0 && val && val[0]) {
        display_text(val, KD_FONT_BOLD, 17, KD_ALIGN_CENTER,
                     pill_x, y + 8, pill_w, h - 16,
                     active_pill ? th->pill_fg : th->fg,
                     true, active_pill ? th->pill_bg : th->bg);
    }
}

static void draw_split_row_shapes(Canvas *c, int y, int h, bool left_act, bool right_act, const Theme *th) {
    canvas_fill_rrect(c, SET_X0, y, SET_W, h, 10, th->card_bg);
    canvas_stroke_rrect(c, SET_X0, y, SET_W, h, 10, 1, th->border);
    int py = y + 8, ph = h - 16;
    if (left_act) canvas_fill_rrect(c, 298, py, 126, ph, 9, th->pill_bg);
    else {
        canvas_fill_rrect(c, 298, py, 126, ph, 9, th->bg);
        canvas_stroke_rrect(c, 298, py, 126, ph, 9, 2, th->border);
    }
    if (right_act) canvas_fill_rrect(c, 434, py, 128, ph, 9, th->pill_bg);
    else {
        canvas_fill_rrect(c, 434, py, 128, ph, 9, th->bg);
        canvas_stroke_rrect(c, 434, py, 128, ph, 9, 2, th->border);
    }
}

static void draw_split_row_text(int y, int h, const char *label,
                                const char *lval, bool lact,
                                const char *rval, bool ract, const Theme *th) {
    display_text(label, KD_FONT_BOLD, 19, KD_ALIGN_LEFT,
                 SET_X0 + 16, y + 4, 260, h - 8, th->fg, true, th->card_bg);
    display_text(lval, KD_FONT_BOLD, 16, KD_ALIGN_CENTER,
                 298, y + 8, 126, h - 16,
                 lact ? th->pill_fg : th->fg, true, lact ? th->pill_bg : th->bg);
    display_text(rval, KD_FONT_BOLD, 16, KD_ALIGN_CENTER,
                 434, y + 8, 128, h - 16,
                 ract ? th->pill_fg : th->fg, true, ract ? th->pill_bg : th->bg);
}

static const char *dict_lang_label(const char *id) {
    if (!id || strcmp(id, "en") == 0) return "Dict: EN";
    if (strcmp(id, "en-de") == 0) return "Dict: EN>DE";
    if (strcmp(id, "de-en") == 0) return "Dict: DE>EN";
    return id;
}

static void truncate_str(const char *src, char *dst, size_t dst_sz, size_t max_chars) {
    if (!src || !src[0]) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n <= max_chars && n < dst_sz) {
        memcpy(dst, src, n + 1);
        return;
    }
    if (max_chars >= dst_sz) max_chars = dst_sz - 1;
    if (max_chars > 3) {
        memcpy(dst, src, max_chars - 3);
        memcpy(dst + max_chars - 3, "...", 4);
    } else {
        memcpy(dst, src, max_chars);
        dst[max_chars] = '\0';
    }
}

static void render_settings_general(Canvas *c, const Config *cfg, const Telemetry *tel,
                                    const SyncState *sync, const Theme *th) {
    draw_row_box(c, SET_Y0 + 0 * SET_ROW_STRIDE, SET_ROW_H, 366, 196, cfg->minute_clock != 0, th);
    draw_split_row_shapes(c, SET_Y0 + 1 * SET_ROW_STRIDE, SET_ROW_H, cfg->invert_mode != 0, false, th);
    draw_row_box(c, SET_Y0 + 2 * SET_ROW_STRIDE, SET_ROW_H, 316, 246, false, th);
    draw_row_box(c, SET_Y0 + 3 * SET_ROW_STRIDE, SET_ROW_H, 366, 196, false, th);
    draw_split_row_shapes(c, SET_Y0 + 4 * SET_ROW_STRIDE, SET_ROW_H,
                          cfg->auto_rotate_min > 0, cfg->auto_sleep_min > 0, th);
    draw_row_box(c, SET_Y0 + 5 * SET_ROW_STRIDE, SET_ROW_H, 366, 196, false, th);
    draw_split_row_shapes(c, SET_Y0 + 6 * SET_ROW_STRIDE, SET_ROW_H, false, false, th);
    draw_row_box(c, SET_Y0 + 7 * SET_ROW_STRIDE, SET_ROW_H, 366, 196, cfg->gallery_clock_overlay != 0, th);

    /* Bottom primary buttons */
    canvas_fill_rrect(c, 24, 646, 356, 66, 12, th->pill_bg);
    canvas_fill_rrect(c, 396, 646, 180, 66, 12, th->card_bg);
    canvas_stroke_rrect(c, 396, 646, 180, 66, 12, 2, th->border);

    display_blit_canvas(c, NULL);
    draw_top_bar_text("SETTINGS", sync, th, true);
    draw_tab_bar_text(th);

    char buf[96];
    draw_row_text(SET_Y0 + 0 * SET_ROW_STRIDE, SET_ROW_H, 366, 196,
                  "Minute Clock Refresh", cfg->minute_clock ? "ON • Fast DU" : "OFF",
                  cfg->minute_clock != 0, th);
    draw_split_row_text(SET_Y0 + 1 * SET_ROW_STRIDE, SET_ROW_H, "Theme & Clock",
                        cfg->invert_mode ? "Dark" : "Light", cfg->invert_mode != 0,
                        cfg->clock_24h ? "24-Hour" : "12-Hour", false, th);
    draw_row_text(SET_Y0 + 2 * SET_ROW_STRIDE, SET_ROW_H, 316, 246,
                  "Timezone", config_tz_label(cfg->tz_posix), false, th);
    draw_row_text(SET_Y0 + 3 * SET_ROW_STRIDE, SET_ROW_H, 366, 196,
                  "Start Screen", ui_screen_name((ScreenId)cfg->start_screen), false, th);
    {
        char r_s[16], s_s[16], lbuf[32], rbuf[32];
        fmt_interval_short(cfg->auto_rotate_min, r_s, sizeof(r_s));
        fmt_interval_short(cfg->auto_sleep_min, s_s, sizeof(s_s));
        snprintf(lbuf, sizeof(lbuf), "Rot %s", r_s);
        snprintf(rbuf, sizeof(rbuf), "Sleep %s", s_s);
        draw_split_row_text(SET_Y0 + 4 * SET_ROW_STRIDE, SET_ROW_H, "Rotate & Sleep",
                            lbuf, cfg->auto_rotate_min > 0,
                            rbuf, cfg->auto_sleep_min > 0, th);
    }
    snprintf(buf, sizeof(buf), "Every %d", cfg->full_refresh_every);
    draw_row_text(SET_Y0 + 5 * SET_ROW_STRIDE, SET_ROW_H, 366, 196,
                  "Anti-Ghosting Flash", buf, false, th);
    snprintf(buf, sizeof(buf), "Font %dpx", cfg->reader_font_px);
    draw_split_row_text(SET_Y0 + 6 * SET_ROW_STRIDE, SET_ROW_H, "Reader Defaults",
                        buf, false, dict_lang_label(cfg->dict_lang), false, th);
    draw_row_text(SET_Y0 + 7 * SET_ROW_STRIDE, SET_ROW_H, 366, 196,
                  "Gallery Clock Overlay", cfg->gallery_clock_overlay ? "ON" : "OFF",
                  cfg->gallery_clock_overlay != 0, th);

    display_text("Return to Stock Kobo UI", KD_FONT_BOLD, 21, KD_ALIGN_CENTER,
                 24, 646, 356, 66, th->pill_fg, true, th->pill_bg);
    display_text("Clean Flash", KD_FONT_BOLD, 20, KD_ALIGN_CENTER,
                 396, 646, 180, 66, th->fg, true, th->card_bg);

    snprintf(buf, sizeof(buf), "KoboDash v%s  •  Kobo Touch A  •  IP %s",
             KD_VERSION, tel ? tel->wifi_ip : "Offline");
    display_text(buf, KD_FONT_REGULAR, 15, KD_ALIGN_CENTER,
                 24, 736, 552, 34, th->muted, true, th->bg);
}

static void render_settings_wifi(Canvas *c, const Telemetry *tel,
                                 const SyncState *sync, const Theme *th) {
    /* Status card */
    canvas_fill_rrect(c, 24, 118, 552, 78, 12, th->card_bg);
    canvas_stroke_rrect(c, 24, 118, 552, 78, 12, 2, th->border);

    /* 3 Control buttons: Wi-Fi ON/OFF, Scan, Hidden SSID */
    bool w_up = (tel && tel->wifi_up);
    if (w_up) canvas_fill_rrect(c, 24, 206, 176, 52, 10, th->pill_bg);
    else {
        canvas_fill_rrect(c, 24, 206, 176, 52, 10, th->bg);
        canvas_stroke_rrect(c, 24, 206, 176, 52, 10, 2, th->border);
    }
    canvas_fill_rrect(c, 212, 206, 176, 52, 10, th->card_bg);
    canvas_stroke_rrect(c, 212, 206, 176, 52, 10, 2, th->border);
    canvas_fill_rrect(c, 400, 206, 176, 52, 10, th->card_bg);
    canvas_stroke_rrect(c, 400, 206, 176, 52, 10, 2, th->border);

    /* Network list rows */
    int total = g_wifi_state ? g_wifi_state->count : 0;
    int page = g_wifi_state ? g_wifi_state->page : 0;
    int start = page * SET_WIFI_PER_PAGE;
    int shown = 0;
    for (int i = 0; i < SET_WIFI_PER_PAGE && (start + i) < total; i++) {
        int ry = SET_WIFI_Y0 + i * SET_WIFI_STRIDE;
        const WifiNetwork *n = &g_wifi_state->nets[start + i];
        canvas_fill_rrect(c, 24, ry, 552, SET_WIFI_H, 10, th->card_bg);
        canvas_stroke_rrect(c, 24, ry, 552, SET_WIFI_H, 10, n->connected ? 2 : 1, th->border);
        if (n->saved) {
            canvas_fill_rrect(c, 474, ry + 11, 90, 38, 8, th->bg);
            canvas_stroke_rrect(c, 474, ry + 11, 90, 38, 8, 1, th->border);
        }
        shown++;
    }
    if (shown == 0) {
        canvas_fill_rrect(c, 24, SET_WIFI_Y0, 552, 140, 12, th->card_bg);
        canvas_stroke_rrect(c, 24, SET_WIFI_Y0, 552, 140, 12, 1, th->subtle);
    }

    /* Bottom bar: Reconnect/DHCP + Pagination / Stock UI */
    bool multi_page = (total > SET_WIFI_PER_PAGE);
    if (multi_page) {
        canvas_fill_rrect(c, 24, 646, 110, 66, 12, th->card_bg);
        canvas_stroke_rrect(c, 24, 646, 110, 66, 12, 2, th->border);
        canvas_fill_rrect(c, 146, 646, 110, 66, 12, th->card_bg);
        canvas_stroke_rrect(c, 146, 646, 110, 66, 12, 2, th->border);
        canvas_fill_rrect(c, 268, 646, 308, 66, 12, th->pill_bg);
    } else {
        canvas_fill_rrect(c, 24, 646, 270, 66, 12, th->pill_bg);
        canvas_fill_rrect(c, 306, 646, 270, 66, 12, th->card_bg);
        canvas_stroke_rrect(c, 306, 646, 270, 66, 12, 2, th->border);
    }

    display_blit_canvas(c, NULL);
    draw_top_bar_text("SETTINGS", sync, th, true);
    draw_tab_bar_text(th);

    char buf[128];
    if (w_up) {
        snprintf(buf, sizeof(buf), "Connected: %s",
                 tel->wifi_ssid[0] ? tel->wifi_ssid : "wlan0");
    } else {
        snprintf(buf, sizeof(buf), "Wi-Fi Disconnected / Off");
    }
    display_text(buf, KD_FONT_BOLD, 20, KD_ALIGN_LEFT,
                 40, 126, 520, 30, th->fg, true, th->card_bg);
    if (g_wifi_state && g_wifi_state->status_msg[0]) {
        snprintf(buf, sizeof(buf), "%s  •  IP: %s", g_wifi_state->status_msg,
                 tel ? tel->wifi_ip : "Offline");
    } else if (w_up) {
        snprintf(buf, sizeof(buf), "IP: %s  •  Signal: %d dBm",
                 tel->wifi_ip, tel->wifi_rssi);
    } else {
        snprintf(buf, sizeof(buf), "Tap Scan Networks to join a Wi-Fi network");
    }
    display_text(buf, KD_FONT_REGULAR, 16, KD_ALIGN_LEFT,
                 40, 160, 520, 28, th->muted, true, th->card_bg);

    display_text(w_up ? "Wi-Fi: ON" : "Wi-Fi: OFF", KD_FONT_BOLD, 18, KD_ALIGN_CENTER,
                 24, 206, 176, 52, w_up ? th->pill_fg : th->fg, true, w_up ? th->pill_bg : th->bg);
    display_text("Scan Networks", KD_FONT_BOLD, 18, KD_ALIGN_CENTER,
                 212, 206, 176, 52, th->fg, true, th->card_bg);
    display_text("Hidden SSID", KD_FONT_BOLD, 18, KD_ALIGN_CENTER,
                 400, 206, 176, 52, th->fg, true, th->card_bg);

    for (int i = 0; i < shown; i++) {
        int ry = SET_WIFI_Y0 + i * SET_WIFI_STRIDE;
        const WifiNetwork *n = &g_wifi_state->nets[start + i];
        int name_w = n->saved ? 310 : 380;
        display_text(n->ssid, KD_FONT_BOLD, 19, KD_ALIGN_LEFT,
                     40, ry + 6, name_w, 26, th->fg, true, th->card_bg);
        snprintf(buf, sizeof(buf), "%s%s%s%s%d dBm",
                 n->connected ? "CONNECTED • " : (n->saved ? "Saved • " : ""),
                 n->secured ? "WPA" : "Open",
                 n->rssi > -99 ? " • " : "",
                 n->rssi > -99 ? "" : "",
                 n->rssi);
        if (n->rssi <= -99) {
            snprintf(buf, sizeof(buf), "%s • Out of range",
                     n->saved ? "Saved" : "Network");
        }
        display_text(buf, KD_FONT_REGULAR, 15, KD_ALIGN_LEFT,
                     40, ry + 34, name_w + 60, 22, th->muted, true, th->card_bg);
        if (n->saved) {
            display_text("Forget", KD_FONT_BOLD, 15, KD_ALIGN_CENTER,
                         474, ry + 11, 90, 38, th->fg, true, th->bg);
        } else {
            display_text("Join >", KD_FONT_BOLD, 16, KD_ALIGN_RIGHT,
                         470, ry + 16, 90, 28, th->muted, true, th->card_bg);
        }
    }
    if (shown == 0) {
        display_text("Tap 'Scan Networks' above to find nearby Wi-Fi",
                     KD_FONT_BOLD, 19, KD_ALIGN_CENTER,
                     40, SET_WIFI_Y0 + 34, 520, 34, th->fg, true, th->card_bg);
        display_text("Supports WPA/WPA2, Open & Hidden SSIDs (2.4 GHz)",
                     KD_FONT_REGULAR, 16, KD_ALIGN_CENTER,
                     40, SET_WIFI_Y0 + 78, 520, 30, th->muted, true, th->card_bg);
    }

    if (multi_page) {
        display_text("< Prev", KD_FONT_BOLD, 18, KD_ALIGN_CENTER,
                     24, 646, 110, 66, th->fg, true, th->card_bg);
        display_text("Next >", KD_FONT_BOLD, 18, KD_ALIGN_CENTER,
                     146, 646, 110, 66, th->fg, true, th->card_bg);
        display_text("Reset Wi-Fi Chip", KD_FONT_BOLD, 20, KD_ALIGN_CENTER,
                     268, 646, 308, 66, th->pill_fg, true, th->pill_bg);
    } else {
        display_text("Reset Wi-Fi Chip", KD_FONT_BOLD, 19, KD_ALIGN_CENTER,
                     24, 646, 270, 66, th->pill_fg, true, th->pill_bg);
        display_text("Return to Stock UI", KD_FONT_BOLD, 19, KD_ALIGN_CENTER,
                     306, 646, 270, 66, th->fg, true, th->card_bg);
    }

    snprintf(buf, sizeof(buf), "Tap any Wi-Fi network to connect  •  Saved to wpa_supplicant.conf");
    display_text(buf, KD_FONT_REGULAR, 14, KD_ALIGN_CENTER,
                 24, 736, 552, 34, th->muted, true, th->bg);
}

static void render_settings_server(Canvas *c, const Config *cfg, const Telemetry *tel,
                                   const SyncState *sync, const Theme *th) {
    /* Status card */
    canvas_fill_rrect(c, 24, 118, 552, 74, 12, th->card_bg);
    canvas_stroke_rrect(c, 24, 118, 552, 74, 12, 2, th->border);

    draw_row_box(c, SET_SRV_Y0 + 0 * SET_SRV_STRIDE, SET_SRV_H, 232, 330, false, th);
    draw_row_box(c, SET_SRV_Y0 + 1 * SET_SRV_STRIDE, SET_SRV_H, 336, 226, cfg->server_token[0] != '\0', th);
    draw_row_box(c, SET_SRV_Y0 + 2 * SET_SRV_STRIDE, SET_SRV_H, 286, 276, false, th);
    draw_split_row_shapes(c, SET_SRV_Y0 + 3 * SET_SRV_STRIDE, SET_SRV_H, false, false, th);
    draw_split_row_shapes(c, SET_SRV_Y0 + 4 * SET_SRV_STRIDE, SET_SRV_H, false, false, th);
    draw_row_box(c, SET_SRV_Y0 + 5 * SET_SRV_STRIDE, SET_SRV_H, 346, 216, cfg->push_mode > 0, th);
    draw_row_box(c, SET_SRV_Y0 + 6 * SET_SRV_STRIDE, SET_SRV_H, 286, 276, false, th);

    /* Bottom bar: Sync Server & Clock Now + Reset ETag */
    canvas_fill_rrect(c, 24, 646, 356, 66, 12, th->pill_bg);
    canvas_fill_rrect(c, 396, 646, 180, 66, 12, th->card_bg);
    canvas_stroke_rrect(c, 396, 646, 180, 66, 12, 2, th->border);

    display_blit_canvas(c, NULL);
    draw_top_bar_text("SETTINGS", sync, th, true);
    draw_tab_bar_text(th);

    char buf[128], shortv[48], b2[32];
    snprintf(buf, sizeof(buf), "Server: %s",
             (sync && sync->last_status[0]) ? sync->last_status : "Not synced yet");
    display_text(buf, KD_FONT_BOLD, 18, KD_ALIGN_LEFT,
                 40, 124, 520, 28, th->fg, true, th->card_bg);

    char last_t[16] = "--:--", next_t[16] = "--:--";
    if (sync && sync->last_sync_time > 0) {
        struct tm lt;
        localtime_r(&sync->last_sync_time, &lt);
        strftime(last_t, sizeof(last_t), "%H:%M", &lt);
    }
    if (sync && sync->next_sync_time > 0) {
        struct tm nt;
        localtime_r(&sync->next_sync_time, &nt);
        strftime(next_t, sizeof(next_t), "%H:%M", &nt);
    }
    snprintf(buf, sizeof(buf), "Last: %s  •  Next: %s  •  NTP: %s  •  IP: %s",
             last_t, next_t,
             (sync && sync->time_synced) ? "Synced" : "Pending",
             tel ? tel->wifi_ip : "Offline");
    display_text(buf, KD_FONT_REGULAR, 15, KD_ALIGN_LEFT,
                 40, 156, 520, 26, th->muted, true, th->card_bg);

    if (cfg->server_url[0]) {
        truncate_str(cfg->server_url, shortv, sizeof(shortv), 28);
    } else {
        snprintf(shortv, sizeof(shortv), "Tap to Set URL");
    }
    draw_row_text(SET_SRV_Y0 + 0 * SET_SRV_STRIDE, SET_SRV_H, 232, 330,
                  "Server URL", shortv, false, th);

    draw_row_text(SET_SRV_Y0 + 1 * SET_SRV_STRIDE, SET_SRV_H, 336, 226,
                  "Auth Bearer Token",
                  cfg->server_token[0] ? "Set (Edit)" : "None (Tap)",
                  cfg->server_token[0] != '\0', th);

    if (cfg->ntp_server[0]) {
        truncate_str(cfg->ntp_server, shortv, sizeof(shortv), 24);
    } else {
        snprintf(shortv, sizeof(shortv), "Disabled (Tap)");
    }
    draw_row_text(SET_SRV_Y0 + 2 * SET_SRV_STRIDE, SET_SRV_H, 286, 276,
                  "NTP Time Server", shortv, false, th);

    char d_int[16], n_int[16];
    fmt_interval_short(cfg->day_interval_min, d_int, sizeof(d_int));
    fmt_interval_short(cfg->night_interval_min, n_int, sizeof(n_int));
    snprintf(shortv, sizeof(shortv), "Day %s", d_int);
    snprintf(b2, sizeof(b2), "Night %s", n_int);
    draw_split_row_text(SET_SRV_Y0 + 3 * SET_SRV_STRIDE, SET_SRV_H,
                        "Sync Intervals", shortv, false, b2, false, th);

    snprintf(shortv, sizeof(shortv), "Day %02d:00", cfg->day_start_hour);
    snprintf(b2, sizeof(b2), "Night %02d:00", cfg->night_start_hour);
    draw_split_row_text(SET_SRV_Y0 + 4 * SET_SRV_STRIDE, SET_SRV_H,
                        "Schedule Hours", shortv, false, b2, false, th);

    draw_row_text(SET_SRV_Y0 + 5 * SET_SRV_STRIDE, SET_SRV_H, 346, 216,
                  "Remote Push Alerts", config_push_mode_label(cfg->push_mode),
                  cfg->push_mode > 0, th);

    if (cfg->screensaver_dir[0]) {
        truncate_str(cfg->screensaver_dir, shortv, sizeof(shortv), 24);
    } else {
        snprintf(shortv, sizeof(shortv), "Auto (/mnt/sd)");
    }
    draw_row_text(SET_SRV_Y0 + 6 * SET_SRV_STRIDE, SET_SRV_H, 286, 276,
                  "Gallery Folder", shortv, false, th);

    display_text((sync && sync->in_progress) ? "Syncing..." : "Test & Sync Now",
                 KD_FONT_BOLD, 21, KD_ALIGN_CENTER,
                 24, 646, 356, 66, th->pill_fg, true, th->pill_bg);
    display_text("Force Refresh", KD_FONT_BOLD, 19, KD_ALIGN_CENTER,
                 396, 646, 180, 66, th->fg, true, th->card_bg);

    display_text("Native TLS 1.3 HTTPS/HTTP  •  Multi-page JSON or PNG/JPG  •  config.ini",
                 KD_FONT_REGULAR, 14, KD_ALIGN_CENTER,
                 24, 736, 552, 34, th->muted, true, th->bg);
}

static void render_settings_device(Canvas *c, const Config *cfg, const Telemetry *tel,
                                   const SyncState *sync, const Theme *th) {
    draw_row_box(c, SET_Y0 + 0 * SET_ROW_STRIDE, SET_ROW_H, 336, 226, false, th);
    draw_row_box(c, SET_Y0 + 1 * SET_ROW_STRIDE, SET_ROW_H, 366, 196, cfg->touch_swap_xy != 0, th);
    draw_split_row_shapes(c, SET_Y0 + 2 * SET_ROW_STRIDE, SET_ROW_H,
                          cfg->touch_mirror_x != 0, cfg->touch_mirror_y != 0, th);
    draw_row_box(c, SET_Y0 + 3 * SET_ROW_STRIDE, SET_ROW_H, 356, 206, tel && tel->ssh_running, th);
    draw_row_box(c, SET_Y0 + 4 * SET_ROW_STRIDE, SET_ROW_H, 356, 206, tel && tel->sd_mounted, th);
    draw_row_box(c, SET_Y0 + 5 * SET_ROW_STRIDE, SET_ROW_H, 366, 196, false, th);
    draw_row_box(c, SET_Y0 + 6 * SET_ROW_STRIDE, SET_ROW_H, 366, 196, cfg->auto_boot != 0, th);
    draw_split_row_shapes(c, SET_Y0 + 7 * SET_ROW_STRIDE, SET_ROW_H,
                          cfg->sleep_style == SLEEP_STYLE_CYCLE, cfg->sleep_rotate_hrs > 0, th);

    /* Bottom 3 buttons: Stock Kobo UI, Reboot (2-tap confirm), Power Off (2-tap confirm) */
    canvas_fill_rrect(c, 24, 646, 236, 66, 12, th->pill_bg);
    if (g_power_confirm == 1) {
        canvas_fill_rrect(c, 272, 646, 146, 66, 12, th->pill_bg);
    } else {
        canvas_fill_rrect(c, 272, 646, 146, 66, 12, th->card_bg);
        canvas_stroke_rrect(c, 272, 646, 146, 66, 12, 2, th->border);
    }
    if (g_power_confirm == 2) {
        canvas_fill_rrect(c, 430, 646, 146, 66, 12, th->pill_bg);
    } else {
        canvas_fill_rrect(c, 430, 646, 146, 66, 12, th->card_bg);
        canvas_stroke_rrect(c, 430, 646, 146, 66, 12, 2, th->border);
    }

    display_blit_canvas(c, NULL);
    draw_top_bar_text("SETTINGS", sync, th, true);
    draw_tab_bar_text(th);

    char buf[128];
    const char *gov = (cfg->cpu_governor[0])
                          ? cfg->cpu_governor
                          : ((tel && tel->cpu_gov[0]) ? tel->cpu_gov : "default");
    if (tel && tel->cpu_khz > 0) {
        snprintf(buf, sizeof(buf), "%s (%dMHz)", gov, tel->cpu_khz / 1000);
    } else {
        snprintf(buf, sizeof(buf), "%s", gov);
    }
    draw_row_text(SET_Y0 + 0 * SET_ROW_STRIDE, SET_ROW_H, 336, 226,
                  "CPU Governor", buf, false, th);

    draw_row_text(SET_Y0 + 1 * SET_ROW_STRIDE, SET_ROW_H, 366, 196,
                  "Touch: Swap X / Y", cfg->touch_swap_xy ? "ON" : "OFF",
                  cfg->touch_swap_xy != 0, th);

    draw_split_row_text(SET_Y0 + 2 * SET_ROW_STRIDE, SET_ROW_H, "Touch Mirror",
                        cfg->touch_mirror_x ? "X: ON" : "X: OFF", cfg->touch_mirror_x != 0,
                        cfg->touch_mirror_y ? "Y: ON" : "Y: OFF", cfg->touch_mirror_y != 0, th);

    draw_row_text(SET_Y0 + 3 * SET_ROW_STRIDE, SET_ROW_H, 356, 206,
                  "SSH Access (:2222)",
                  (tel && tel->ssh_running) ? "Running (Stop)" : "Start SSH",
                  tel && tel->ssh_running, th);

    if (tel && tel->sd_mounted) {
        snprintf(buf, sizeof(buf), "%luMB (Unmount)", tel->sd_free_mb);
    } else {
        snprintf(buf, sizeof(buf), "Mount SD Card");
    }
    draw_row_text(SET_Y0 + 4 * SET_ROW_STRIDE, SET_ROW_H, 356, 206,
                  "MicroSD Storage", buf, tel && tel->sd_mounted, th);

    snprintf(buf, sizeof(buf), cfg->reader_flash_every <= 1 ? "Every Page" : "Every %d pages",
             cfg->reader_flash_every);
    draw_row_text(SET_Y0 + 5 * SET_ROW_STRIDE, SET_ROW_H, 366, 196,
                  "Reader Full Flash", buf, false, th);

    draw_row_text(SET_Y0 + 6 * SET_ROW_STRIDE, SET_ROW_H, 366, 196,
                  "Auto-Boot KoboDash", cfg->auto_boot ? "ON • Safe Boot" : "OFF (Stock UI)",
                  cfg->auto_boot != 0, th);

    {
        char srot[24];
        if (cfg->sleep_rotate_hrs > 0) snprintf(srot, sizeof(srot), "Every %dh", cfg->sleep_rotate_hrs);
        else snprintf(srot, sizeof(srot), "Fixed");
        draw_split_row_text(SET_Y0 + 7 * SET_ROW_STRIDE, SET_ROW_H, "Sleep Screen",
                            config_sleep_style_label(cfg->sleep_style),
                            cfg->sleep_style == SLEEP_STYLE_CYCLE,
                            srot, cfg->sleep_rotate_hrs > 0, th);
    }

    display_text("Stock Kobo UI", KD_FONT_BOLD, 20, KD_ALIGN_CENTER,
                 24, 646, 236, 66, th->pill_fg, true, th->pill_bg);
    display_text(g_power_confirm == 1 ? "Confirm?" : "Reboot", KD_FONT_BOLD, 19, KD_ALIGN_CENTER,
                 272, 646, 146, 66,
                 g_power_confirm == 1 ? th->pill_fg : th->fg,
                 true, g_power_confirm == 1 ? th->pill_bg : th->card_bg);
    display_text(g_power_confirm == 2 ? "Confirm?" : "Power Off", KD_FONT_BOLD, 19, KD_ALIGN_CENTER,
                 430, 646, 146, 66,
                 g_power_confirm == 2 ? th->pill_fg : th->fg,
                 true, g_power_confirm == 2 ? th->pill_bg : th->card_bg);

    if (g_device_notice[0]) {
        snprintf(buf, sizeof(buf), "%s", g_device_notice);
    } else if (tel) {
        snprintf(buf, sizeof(buf), "Touch A • RAM %lu/%lu MB • Slide Power to Sleep/Lock",
                 tel->mem_avail_kb / 1024UL, tel->mem_total_kb / 1024UL);
    } else {
        snprintf(buf, sizeof(buf), "Slide Power to Sleep/Lock  •  Hold Power 2.5s to Power Off");
    }
    display_text(buf, KD_FONT_REGULAR, 14, KD_ALIGN_CENTER,
                 24, 736, 552, 34, th->muted, true, th->bg);
}

static void render_settings_screen(Canvas *c, const Config *cfg, const Telemetry *tel,
                                   const SyncState *sync, KdRefreshMode mode) {
    Theme th = get_theme(cfg);
    canvas_clear(c, th.bg);
    draw_top_bar_shapes(c, &th, true);
    draw_tab_bar_shapes(c, &th);

    switch (g_settings_tab) {
        case SET_TAB_WIFI:
            render_settings_wifi(c, tel, sync, &th);
            break;
        case SET_TAB_SERVER:
            render_settings_server(c, cfg, tel, sync, &th);
            break;
        case SET_TAB_DEVICE:
            render_settings_device(c, cfg, tel, sync, &th);
            break;
        default:
            render_settings_general(c, cfg, tel, sync, &th);
            break;
    }

    display_refresh(NULL, mode);
}

void ui_render_screen(Canvas *c, ScreenId screen, const Config *cfg,
                      const Telemetry *tel, const SyncState *sync,
                      const Gallery *gal, const struct tm *now_tm,
                      KdRefreshMode mode) {
    switch (screen) {
        case SCREEN_CLOCK:
            render_clock_screen(c, cfg, tel, sync, gal, now_tm, mode);
            break;
        case SCREEN_STATUS:
            render_status_screen(c, cfg, tel, sync, gal, now_tm, mode);
            break;
        case SCREEN_DASHBOARD:
            render_dashboard_screen(c, cfg, tel, sync, gal, now_tm, mode);
            break;
        case SCREEN_GALLERY:
            render_gallery_screen(c, cfg, gal, now_tm, mode);
            break;
        case SCREEN_SETTINGS:
            render_settings_screen(c, cfg, tel, sync, mode);
            break;
        default:
            render_clock_screen(c, cfg, tel, sync, gal, now_tm, mode);
            break;
    }
}

static bool hit(int x, int y, int rx, int ry, int rw, int rh) {
    return (x >= rx && x < rx + rw && y >= ry && y < ry + rh);
}

UiAction ui_handle_input(ScreenId current, ScreenId prev_non_settings,
                         const InputEvent *ev, const Config *cfg, const Gallery *gal) {
    UiAction act;
    memset(&act, 0, sizeof(act));
    (void)cfg;

    if (!ev || ev->type == INPUT_NONE) return act;

    /* Physical hardware buttons work identically from EVERY screen */
    if (ev->type == INPUT_HOME_LONG) {
        act.type = ACT_EXIT_STOCK_UI;
        return act;
    }
    if (ev->type == INPUT_HOME_SHORT) {
        if (current == SCREEN_SETTINGS) {
            act.type = ACT_CLOSE_SETTINGS;
            act.arg = (int)prev_non_settings;
        } else {
            act.type = ACT_NEXT_SCREEN;
        }
        return act;
    }
    if (ev->type == INPUT_POWER_SHORT) {
        act.type = ACT_LOCK_OR_SLEEP;
        return act;
    }
    if (ev->type == INPUT_POWER_LONG) {
        act.type = ACT_POWEROFF;
        return act;
    }

    /* Gestures */
    if (ev->type == INPUT_SWIPE_LEFT) {
        act.type = (current == SCREEN_SETTINGS) ? ACT_CLOSE_SETTINGS : ACT_NEXT_SCREEN;
        act.arg = (int)prev_non_settings;
        return act;
    }
    if (ev->type == INPUT_SWIPE_RIGHT) {
        act.type = (current == SCREEN_SETTINGS) ? ACT_CLOSE_SETTINGS : ACT_PREV_SCREEN;
        act.arg = (int)prev_non_settings;
        return act;
    }
    if (ev->type == INPUT_SWIPE_UP && current != SCREEN_SETTINGS) {
        act.type = ACT_OPEN_SETTINGS;
        return act;
    }
    if (ev->type == INPUT_SWIPE_DOWN) {
        act.type = (current == SCREEN_SETTINGS) ? ACT_CLOSE_SETTINGS : ACT_SYNC_NOW;
        act.arg = (int)prev_non_settings;
        return act;
    }

    if (ev->type != INPUT_TAP) return act;

    int x = ev->x;
    int y = ev->y;

    /* Settings screen hit-testing (4 tabs: GENERAL, WI-FI, SERVER, DEVICE) */
    if (current == SCREEN_SETTINGS) {
        if (hit(x, y, 450, 0, 150, 60)) {
            act.type = ACT_CLOSE_SETTINGS;
            act.arg = (int)prev_non_settings;
            return act;
        }
        /* Top tab bar (y=60..112) */
        if (y >= 60 && y < 114) {
            for (int i = 0; i < SET_TAB_COUNT; i++) {
                int tx = 24 + i * 140;
                if (hit(x, y, tx - 4, 60, 140, 54)) {
                    act.type = ACT_SET_TAB;
                    act.arg = i;
                    return act;
                }
            }
            return act;
        }

        if (g_settings_tab == SET_TAB_GENERAL) {
            for (int r = 0; r < 8; r++) {
                int ry = SET_Y0 + r * SET_ROW_STRIDE;
                if (!hit(x, y, SET_X0, ry, SET_W, SET_ROW_H)) continue;
                switch (r) {
                    case 0: act.type = ACT_TOGGLE_MINUTE_CLOCK; return act;
                    case 1: act.type = (x >= 430) ? ACT_TOGGLE_24H : ACT_TOGGLE_THEME; return act;
                    case 2: act.type = ACT_CYCLE_TIMEZONE; return act;
                    case 3: act.type = ACT_CYCLE_START_SCREEN; return act;
                    case 4: act.type = (x >= 430) ? ACT_CYCLE_AUTO_SLEEP : ACT_CYCLE_AUTO_ROTATE; return act;
                    case 5: act.type = ACT_CYCLE_FULL_REFRESH; return act;
                    case 6: act.type = (x >= 430) ? ACT_CYCLE_DICT_LANG : ACT_CYCLE_READER_FONT; return act;
                    case 7: act.type = ACT_TOGGLE_GALLERY_CLOCK; return act;
                }
            }
            if (hit(x, y, 24, 642, 356, 76)) { act.type = ACT_EXIT_STOCK_UI; return act; }
            if (hit(x, y, 396, 642, 180, 76)) { act.type = ACT_FULL_REFRESH; return act; }
            return act;
        }

        if (g_settings_tab == SET_TAB_WIFI) {
            if (hit(x, y, 24, 202, 176, 60)) { act.type = ACT_WIFI_TOGGLE; return act; }
            if (hit(x, y, 212, 202, 176, 60)) { act.type = ACT_WIFI_SCAN; return act; }
            if (hit(x, y, 400, 202, 176, 60)) { act.type = ACT_WIFI_HIDDEN; return act; }

            int total = g_wifi_state ? g_wifi_state->count : 0;
            int page = g_wifi_state ? g_wifi_state->page : 0;
            int start = page * SET_WIFI_PER_PAGE;
            for (int i = 0; i < SET_WIFI_PER_PAGE && (start + i) < total; i++) {
                int ry = SET_WIFI_Y0 + i * SET_WIFI_STRIDE;
                if (!hit(x, y, 24, ry, 552, SET_WIFI_H)) continue;
                const WifiNetwork *n = &g_wifi_state->nets[start + i];
                if (n->saved && hit(x, y, 468, ry, 108, SET_WIFI_H)) {
                    act.type = ACT_WIFI_FORGET_NET;
                    act.arg = start + i;
                } else {
                    act.type = ACT_WIFI_SELECT_NET;
                    act.arg = start + i;
                }
                return act;
            }
            bool multi_page = (total > SET_WIFI_PER_PAGE);
            if (multi_page) {
                if (hit(x, y, 24, 642, 110, 76)) { act.type = ACT_WIFI_PAGE_PREV; return act; }
                if (hit(x, y, 146, 642, 110, 76)) { act.type = ACT_WIFI_PAGE_NEXT; return act; }
                if (hit(x, y, 268, 642, 308, 76)) { act.type = ACT_WIFI_RECONNECT; return act; }
            } else {
                if (hit(x, y, 24, 642, 270, 76)) { act.type = ACT_WIFI_RECONNECT; return act; }
                if (hit(x, y, 306, 642, 270, 76)) { act.type = ACT_EXIT_STOCK_UI; return act; }
            }
            return act;
        }

        if (g_settings_tab == SET_TAB_SERVER) {
            for (int r = 0; r < 7; r++) {
                int ry = SET_SRV_Y0 + r * SET_SRV_STRIDE;
                if (!hit(x, y, SET_X0, ry, SET_W, SET_SRV_H)) continue;
                switch (r) {
                    case 0: act.type = ACT_EDIT_SERVER_URL; return act;
                    case 1: act.type = ACT_EDIT_SERVER_TOKEN; return act;
                    case 2: act.type = ACT_EDIT_NTP_SERVER; return act;
                    case 3: act.type = (x >= 430) ? ACT_CYCLE_NIGHT_INTERVAL : ACT_CYCLE_DAY_INTERVAL; return act;
                    case 4: act.type = (x >= 430) ? ACT_CYCLE_NIGHT_START : ACT_CYCLE_DAY_START; return act;
                    case 5: act.type = ACT_CYCLE_PUSH_MODE; return act;
                    case 6: act.type = ACT_EDIT_SCREENSAVER_DIR; return act;
                }
            }
            if (hit(x, y, 24, 642, 356, 76)) { act.type = ACT_SYNC_NOW; return act; }
            if (hit(x, y, 396, 642, 180, 76)) { act.type = ACT_CLEAR_ETAG_SYNC; return act; }
            return act;
        }

        if (g_settings_tab == SET_TAB_DEVICE) {
            for (int r = 0; r < 8; r++) {
                int ry = SET_Y0 + r * SET_ROW_STRIDE;
                if (!hit(x, y, SET_X0, ry, SET_W, SET_ROW_H)) continue;
                switch (r) {
                    case 0: act.type = ACT_CYCLE_CPU_GOV; return act;
                    case 1: act.type = ACT_TOGGLE_TOUCH_SWAP; return act;
                    case 2: act.type = (x >= 430) ? ACT_TOGGLE_TOUCH_MIRROR_Y : ACT_TOGGLE_TOUCH_MIRROR_X; return act;
                    case 3: act.type = ACT_TOGGLE_SSH; return act;
                    case 4: act.type = ACT_TOGGLE_SD_MOUNT; return act;
                    case 5: act.type = ACT_CYCLE_READER_FLASH; return act;
                    case 6: act.type = ACT_TOGGLE_AUTO_BOOT; return act;
                    case 7: act.type = (x >= 430) ? ACT_CYCLE_SLEEP_ROTATE : ACT_CYCLE_SLEEP_STYLE; return act;
                }
            }
            if (hit(x, y, 24, 642, 236, 76)) { act.type = ACT_EXIT_STOCK_UI; return act; }
            if (hit(x, y, 272, 642, 146, 76)) { act.type = ACT_REBOOT; return act; }
            if (hit(x, y, 430, 642, 146, 76)) { act.type = ACT_POWEROFF; return act; }
            return act;
        }
        return act;
    }

    /* Clock top-bar 1-tap BOOKS button */
    if (current == SCREEN_CLOCK && hit(x, y, 360, 0, 114, 64)) {
        act.type = ACT_GOTO_SCREEN;
        act.arg = SCREEN_LIBRARY;
        return act;
    }

    /* Clock subtle live event / waiting-on-you strip -> jump directly to that dashboard page */
    if (current == SCREEN_CLOCK && g_clock_strip_visible && hit(x, y, 44, 544, 512, 46)) {
        act.type = ACT_GOTO_DASH_PAGE;
        act.arg = g_clock_strip_page;
        return act;
    }

    /* Non-settings top-right corner: Settings gear */
    if (hit(x, y, 526, 0, 74, 72)) {
        act.type = ACT_OPEN_SETTINGS;
        return act;
    }
    /* Non-settings top refresh icon (or top-left on full-screen image screens) */
    if (current == SCREEN_DASHBOARD || current == SCREEN_GALLERY) {
        if (hit(x, y, 0, 0, 96, 80) || hit(x, y, 464, 0, 62, 68)) {
            act.type = ACT_SYNC_NOW;
            return act;
        }
    } else if (hit(x, y, 464, 0, 62, 68)) {
        act.type = ACT_SYNC_NOW;
        return act;
    }

    /* Dashboard setup card buttons (only when no bundle/image is displayed) */
    if (current == SCREEN_DASHBOARD && !(g_dash_bundle && g_dash_bundle->has_bundle && g_dash_bundle->page_count > 0)) {
        if (hit(x, y, 60, 496, 232, 74)) {
            if (cfg && cfg->server_url[0]) {
                act.type = ACT_SYNC_NOW;
            } else {
                act.type = ACT_OPEN_SETTINGS_TAB;
                act.arg = SET_TAB_SERVER;
            }
            return act;
        }
        if (hit(x, y, 308, 496, 232, 74)) {
            act.type = ACT_OPEN_SETTINGS_TAB;
            act.arg = SET_TAB_WIFI;
            return act;
        }
    }

    /* Bottom navigation prev/next buttons */
    if (hit(x, y, 0, 724, 175, 76)) {
        act.type = ACT_PREV_SCREEN;
        return act;
    }
    if (hit(x, y, 425, 724, 175, 76)) {
        act.type = ACT_NEXT_SCREEN;
        return act;
    }

    /* Edge taps to switch screens easily */
    if (x < 88 && y >= 72 && y < 724) {
        act.type = ACT_PREV_SCREEN;
        return act;
    }
    if (x > 512 && y >= 72 && y < 724) {
        act.type = ACT_NEXT_SCREEN;
        return act;
    }

    /* Gallery center tap advances photo */
    if (current == SCREEN_GALLERY && gal && gal->count > 1) {
        act.type = ACT_GALLERY_NEXT;
        return act;
    }

    return act;
}
