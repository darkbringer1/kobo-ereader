#include "kobodash.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void copy_str(char *dst, size_t dst_sz, const char *src) {
    if (!dst || dst_sz == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_sz, "%s", src);
}

static char *trim_ws(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
    return s;
}

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void config_set_defaults(Config *cfg, const char *base_dir) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->minute_clock = 1;
    cfg->day_interval_min = 30;
    cfg->night_interval_min = 120;
    cfg->day_start_hour = 7;
    cfg->night_start_hour = 23;
    cfg->auto_rotate_min = 0;
    cfg->auto_sleep_min = 15;
    cfg->sleep_style = SLEEP_STYLE_CYCLE;
    cfg->sleep_rotate_hrs = 3;
    cfg->full_refresh_every = 15;
    cfg->invert_mode = 0;
    cfg->clock_24h = 1;
    cfg->gallery_clock_overlay = 1;
    cfg->start_screen = SCREEN_CLOCK;
    cfg->touch_swap_xy = 1;
    cfg->touch_mirror_x = 1;
    cfg->touch_mirror_y = 0;
    cfg->reader_font_px = 24;
    cfg->reader_line_spacing = 1;
    cfg->reader_flash_every = 5;
    copy_str(cfg->reader_font_family, sizeof(cfg->reader_font_family), "georgia");
    copy_str(cfg->dict_lang, sizeof(cfg->dict_lang), "en");
    copy_str(cfg->tz_posix, sizeof(cfg->tz_posix), "CET-1CEST,M3.5.0,M10.5.0/3");
    copy_str(cfg->ntp_server, sizeof(cfg->ntp_server), "pool.ntp.org");
    copy_str(cfg->server_url, sizeof(cfg->server_url), "");
    copy_str(cfg->server_token, sizeof(cfg->server_token), "");
    cfg->push_mode = 1; /* 1 = Live SSE when charging, Poll on battery */
    copy_str(cfg->screensaver_dir, sizeof(cfg->screensaver_dir), "");
    copy_str(cfg->cpu_governor, sizeof(cfg->cpu_governor), "");
    cfg->auto_boot = 1;
    copy_str(cfg->base_dir, sizeof(cfg->base_dir),
             (base_dir && base_dir[0]) ? base_dir : "/mnt/onboard/.adds/kobodash");
}

int config_load(Config *cfg, const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char *p = trim_ws(line);
        if (*p == '\0' || *p == '#' || *p == ';' || *p == '[') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = trim_ws(p);
        char *v = trim_ws(eq + 1);

        if (strcmp(k, "minute_clock") == 0) {
            cfg->minute_clock = atoi(v) ? 1 : 0;
        } else if (strcmp(k, "day_interval_min") == 0) {
            cfg->day_interval_min = clamp_int(atoi(v), 1, 1440);
        } else if (strcmp(k, "night_interval_min") == 0) {
            cfg->night_interval_min = clamp_int(atoi(v), 0, 1440);
        } else if (strcmp(k, "day_start_hour") == 0) {
            cfg->day_start_hour = clamp_int(atoi(v), 0, 23);
        } else if (strcmp(k, "night_start_hour") == 0) {
            cfg->night_start_hour = clamp_int(atoi(v), 0, 23);
        } else if (strcmp(k, "auto_rotate_min") == 0) {
            cfg->auto_rotate_min = clamp_int(atoi(v), 0, 720);
        } else if (strcmp(k, "auto_sleep_min") == 0) {
            cfg->auto_sleep_min = clamp_int(atoi(v), 0, 240);
        } else if (strcmp(k, "sleep_style") == 0) {
            cfg->sleep_style = clamp_int(atoi(v), 0, SLEEP_STYLE_CLOCK);
        } else if (strcmp(k, "sleep_rotate_hrs") == 0) {
            cfg->sleep_rotate_hrs = clamp_int(atoi(v), 0, 24);
        } else if (strcmp(k, "full_refresh_every") == 0) {
            cfg->full_refresh_every = clamp_int(atoi(v), 1, 120);
        } else if (strcmp(k, "invert_mode") == 0) {
            cfg->invert_mode = atoi(v) ? 1 : 0;
        } else if (strcmp(k, "clock_24h") == 0) {
            cfg->clock_24h = atoi(v) ? 1 : 0;
        } else if (strcmp(k, "gallery_clock_overlay") == 0) {
            cfg->gallery_clock_overlay = atoi(v) ? 1 : 0;
        } else if (strcmp(k, "start_screen") == 0) {
            if (strcmp(v, "clock") == 0) cfg->start_screen = SCREEN_CLOCK;
            else if (strcmp(v, "library") == 0) cfg->start_screen = SCREEN_LIBRARY;
            else if (strcmp(v, "dashboard") == 0) cfg->start_screen = SCREEN_DASHBOARD;
            else if (strcmp(v, "status") == 0) cfg->start_screen = SCREEN_STATUS;
            else if (strcmp(v, "gallery") == 0) cfg->start_screen = SCREEN_GALLERY;
            else cfg->start_screen = clamp_int(atoi(v), 0, SCREEN_GALLERY);
        } else if (strcmp(k, "reader_font_px") == 0) {
            cfg->reader_font_px = clamp_int(atoi(v), 16, 36);
        } else if (strcmp(k, "reader_line_spacing") == 0) {
            cfg->reader_line_spacing = clamp_int(atoi(v), 0, 2);
        } else if (strcmp(k, "reader_flash_every") == 0) {
            cfg->reader_flash_every = clamp_int(atoi(v), 1, 50);
        } else if (strcmp(k, "reader_font_family") == 0) {
            copy_str(cfg->reader_font_family, sizeof(cfg->reader_font_family), v);
        } else if (strcmp(k, "dict_lang") == 0) {
            copy_str(cfg->dict_lang, sizeof(cfg->dict_lang), v);
        } else if (strcmp(k, "touch_swap_xy") == 0) {
            cfg->touch_swap_xy = atoi(v) ? 1 : 0;
        } else if (strcmp(k, "touch_mirror_x") == 0) {
            cfg->touch_mirror_x = atoi(v) ? 1 : 0;
        } else if (strcmp(k, "touch_mirror_y") == 0) {
            cfg->touch_mirror_y = atoi(v) ? 1 : 0;
        } else if (strcmp(k, "tz_posix") == 0) {
            copy_str(cfg->tz_posix, sizeof(cfg->tz_posix), v);
        } else if (strcmp(k, "ntp_server") == 0) {
            copy_str(cfg->ntp_server, sizeof(cfg->ntp_server), v);
        } else if (strcmp(k, "server_url") == 0) {
            copy_str(cfg->server_url, sizeof(cfg->server_url), v);
        } else if (strcmp(k, "server_token") == 0) {
            copy_str(cfg->server_token, sizeof(cfg->server_token), v);
        } else if (strcmp(k, "push_mode") == 0) {
            cfg->push_mode = clamp_int(atoi(v), 0, 2);
        } else if (strcmp(k, "screensaver_dir") == 0) {
            copy_str(cfg->screensaver_dir, sizeof(cfg->screensaver_dir), v);
        } else if (strcmp(k, "cpu_governor") == 0) {
            copy_str(cfg->cpu_governor, sizeof(cfg->cpu_governor), v);
        } else if (strcmp(k, "auto_boot") == 0) {
            cfg->auto_boot = atoi(v) ? 1 : 0;
        }
    }
    fclose(fp);
    return 0;
}

int config_save(const Config *cfg, const char *path) {
    char tmp_path[320];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    FILE *fp = fopen(tmp_path, "w");
    if (!fp) return -1;

    const char *start_name = "clock";
    if (cfg->start_screen == SCREEN_LIBRARY) start_name = "library";
    else if (cfg->start_screen == SCREEN_DASHBOARD) start_name = "dashboard";
    else if (cfg->start_screen == SCREEN_STATUS) start_name = "status";
    else if (cfg->start_screen == SCREEN_GALLERY) start_name = "gallery";

    fprintf(fp,
            "# KoboDash Custom Firmware Configuration\n"
            "# Editable on-device (Settings screen) or over USB/SSH.\n\n"
            "[display]\n"
            "minute_clock = %d\n"
            "invert_mode = %d\n"
            "clock_24h = %d\n"
            "full_refresh_every = %d\n"
            "gallery_clock_overlay = %d\n"
            "start_screen = %s\n\n"
            "[reader]\n"
            "reader_font_px = %d\n"
            "reader_line_spacing = %d\n"
            "reader_flash_every = %d\n"
            "reader_font_family = %s\n"
            "dict_lang = %s\n\n"
            "[schedule]\n"
            "day_interval_min = %d\n"
            "night_interval_min = %d\n"
            "day_start_hour = %d\n"
            "night_start_hour = %d\n"
            "auto_rotate_min = %d\n"
            "auto_sleep_min = %d\n"
            "sleep_style = %d\n"
            "sleep_rotate_hrs = %d\n\n"
            "[time]\n"
            "tz_posix = %s\n"
            "ntp_server = %s\n\n"
            "[server]\n"
            "# Point to your own server URL returning either:\n"
            "#   - A 600x800 PNG / JPG / BMP / GIF image, or\n"
            "#   - JSON: {\"title\":\"...\",\"subtitle\":\"...\",\"lines\":[\"...\",...]}\n"
            "# Example: server_url = https://dashboard.example.com/kobo/bundle\n"
            "server_url = %s\n"
            "# Optional Bearer token sent as Authorization: Bearer <token>\n"
            "server_token = %s\n"
            "# Remote push mode: 0=Poll only, 1=Live SSE while charging, 2=Always live SSE\n"
            "push_mode = %d\n\n"
            "[gallery]\n"
            "# Auto-scans /mnt/sd/screensavers, /mnt/sd/images, /mnt/sd, and .adds/kobodash/screensavers\n"
            "screensaver_dir = %s\n\n"
            "[touch]\n"
            "# Defaults for Kobo Touch A (N905 / neonode_ts)\n"
            "touch_swap_xy = %d\n"
            "touch_mirror_x = %d\n"
            "touch_mirror_y = %d\n\n"
            "[device]\n"
            "cpu_governor = %s\n"
            "auto_boot = %d\n",
            cfg->minute_clock,
            cfg->invert_mode,
            cfg->clock_24h,
            cfg->full_refresh_every,
            cfg->gallery_clock_overlay,
            start_name,
            cfg->reader_font_px,
            cfg->reader_line_spacing,
            cfg->reader_flash_every,
            cfg->reader_font_family,
            cfg->dict_lang,
            cfg->day_interval_min,
            cfg->night_interval_min,
            cfg->day_start_hour,
            cfg->night_start_hour,
            cfg->auto_rotate_min,
            cfg->auto_sleep_min,
            cfg->sleep_style,
            cfg->sleep_rotate_hrs,
            cfg->tz_posix,
            cfg->ntp_server,
            cfg->server_url,
            cfg->server_token,
            cfg->push_mode,
            cfg->screensaver_dir,
            cfg->touch_swap_xy,
            cfg->touch_mirror_x,
            cfg->touch_mirror_y,
            cfg->cpu_governor,
            cfg->auto_boot);

    fflush(fp);
    fsync(fileno(fp));
    if (fclose(fp) != 0) {
        unlink(tmp_path);
        return -1;
    }
    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

bool config_is_daytime(const Config *cfg, const struct tm *now_tm) {
    if (!now_tm) return true;
    int h = now_tm->tm_hour;
    if (cfg->day_start_hour == cfg->night_start_hour) return true;
    if (cfg->day_start_hour < cfg->night_start_hour) {
        return (h >= cfg->day_start_hour && h < cfg->night_start_hour);
    }
    return (h >= cfg->day_start_hour || h < cfg->night_start_hour);
}

int config_active_interval_min(const Config *cfg, const struct tm *now_tm) {
    if (config_is_daytime(cfg, now_tm)) {
        return cfg->day_interval_min;
    }
    return cfg->night_interval_min;
}

int config_next_day_interval(int cur) {
    static const int vals[] = { 1, 5, 15, 30, 60, 120 };
    const int n = (int)(sizeof(vals) / sizeof(vals[0]));
    for (int i = 0; i < n; i++) {
        if (vals[i] > cur) return vals[i];
    }
    return vals[0];
}

int config_next_night_interval(int cur) {
    static const int vals[] = { 30, 60, 120, 240, 0 };
    const int n = (int)(sizeof(vals) / sizeof(vals[0]));
    for (int i = 0; i < n; i++) {
        if (vals[i] == cur) return vals[(i + 1) % n];
    }
    return vals[0];
}

int config_next_rotate_interval(int cur) {
    static const int vals[] = { 0, 1, 5, 15, 30, 60 };
    const int n = (int)(sizeof(vals) / sizeof(vals[0]));
    for (int i = 0; i < n; i++) {
        if (vals[i] == cur) return vals[(i + 1) % n];
    }
    return vals[0];
}

int config_next_auto_sleep(int cur) {
    static const int vals[] = { 0, 5, 10, 15, 30, 60 };
    const int n = (int)(sizeof(vals) / sizeof(vals[0]));
    for (int i = 0; i < n; i++) {
        if (vals[i] == cur) return vals[(i + 1) % n];
    }
    return 15;
}

int config_next_sleep_style(int cur) {
    return (cur + 1) % (SLEEP_STYLE_CLOCK + 1);
}

const char *config_sleep_style_label(int style) {
    switch (style) {
        case SLEEP_STYLE_CYCLE:    return "Cycle 3p";
        case SLEEP_STYLE_TODAY:    return "Today";
        case SLEEP_STYLE_CALENDAR: return "Calendar";
        case SLEEP_STYLE_STATS:    return "Stats";
        case SLEEP_STYLE_CLOCK:    return "Clock";
        default:                   return "Cycle 3p";
    }
}

int config_next_sleep_rotate(int cur) {
    static const int vals[] = { 1, 2, 3, 6, 0 };
    const int n = (int)(sizeof(vals) / sizeof(vals[0]));
    for (int i = 0; i < n; i++) {
        if (vals[i] == cur) return vals[(i + 1) % n];
    }
    return 3;
}

int config_next_full_refresh(int cur) {
    static const int vals[] = { 5, 10, 15, 30, 60 };
    const int n = (int)(sizeof(vals) / sizeof(vals[0]));
    for (int i = 0; i < n; i++) {
        if (vals[i] == cur) return vals[(i + 1) % n];
    }
    return 15;
}

int config_next_start_screen(int cur) {
    static const int vals[] = {
        SCREEN_CLOCK, SCREEN_LIBRARY, SCREEN_DASHBOARD, SCREEN_STATUS, SCREEN_GALLERY
    };
    const int n = (int)(sizeof(vals) / sizeof(vals[0]));
    for (int i = 0; i < n; i++) {
        if (vals[i] == cur) return vals[(i + 1) % n];
    }
    return SCREEN_CLOCK;
}

int config_next_reader_font(int cur) {
    static const int vals[] = { 18, 20, 22, 24, 26, 28, 30, 34 };
    const int n = (int)(sizeof(vals) / sizeof(vals[0]));
    for (int i = 0; i < n; i++) {
        if (vals[i] == cur) return vals[(i + 1) % n];
    }
    return 24;
}

int config_next_reader_flash(int cur) {
    static const int vals[] = { 1, 3, 5, 8, 12, 20 };
    const int n = (int)(sizeof(vals) / sizeof(vals[0]));
    for (int i = 0; i < n; i++) {
        if (vals[i] == cur) return vals[(i + 1) % n];
    }
    return 5;
}

typedef struct {
    const char *id;
    const char *label;
} FontFamilyInfo;

static const FontFamilyInfo FONT_FAMILIES[] = {
    { "georgia",     "Georgia" },
    { "charter",     "Charter" },
    { "palatino",    "Palatino" },
    { "baskerville", "Baskerville" },
    { "times",       "Times" },
    { "verdana",     "Verdana" },
    { "trebuchet",   "Trebuchet" }
};

const char *config_next_font_family(const char *cur) {
    const int n = (int)(sizeof(FONT_FAMILIES) / sizeof(FONT_FAMILIES[0]));
    if (cur && cur[0]) {
        for (int i = 0; i < n; i++) {
            if (strcmp(FONT_FAMILIES[i].id, cur) == 0) {
                return FONT_FAMILIES[(i + 1) % n].id;
            }
        }
    }
    return FONT_FAMILIES[0].id;
}

const char *config_font_family_label(const char *id) {
    const int n = (int)(sizeof(FONT_FAMILIES) / sizeof(FONT_FAMILIES[0]));
    if (id && id[0]) {
        for (int i = 0; i < n; i++) {
            if (strcmp(FONT_FAMILIES[i].id, id) == 0) {
                return FONT_FAMILIES[i].label;
            }
        }
    }
    return "Georgia";
}

const char *config_next_dict_lang(const char *cur) {
    if (!cur || strcmp(cur, "en") == 0) return "en-de";
    if (strcmp(cur, "en-de") == 0) return "de-en";
    return "en";
}

const char *config_push_mode_label(int mode) {
    if (mode == 0) return "Poll Only";
    if (mode == 2) return "Always Live";
    return "Live on Power";
}

typedef struct {
    const char *posix;
    const char *label;
} TzPreset;

static const TzPreset TZ_PRESETS[] = {
    { "CET-1CEST,M3.5.0,M10.5.0/3",     "CET/CEST (Berlin)" },
    { "EET-2EEST,M3.5.0/3,M10.5.0/4",   "EET/EEST (+2/+3)" },
    { "TRT-3",                           "TRT (+3 Istanbul)" },
    { "GMT0BST,M3.5.0/1,M10.5.0",       "GMT/BST (London)" },
    { "UTC0",                            "UTC (+0)" },
    { "EST5EDT,M3.2.0,M11.1.0",         "EST/EDT (New York)" },
    { "PST8PDT,M3.2.0,M11.1.0",         "PST/PDT (LA)" }
};

void config_next_timezone(const char *cur, char *out_posix, size_t out_sz) {
    const int n = (int)(sizeof(TZ_PRESETS) / sizeof(TZ_PRESETS[0]));
    int idx = 0;
    if (cur && cur[0]) {
        for (int i = 0; i < n; i++) {
            if (strcmp(TZ_PRESETS[i].posix, cur) == 0) {
                idx = (i + 1) % n;
                break;
            }
        }
    }
    copy_str(out_posix, out_sz, TZ_PRESETS[idx].posix);
}

const char *config_tz_label(const char *tz_posix) {
    const int n = (int)(sizeof(TZ_PRESETS) / sizeof(TZ_PRESETS[0]));
    if (tz_posix && tz_posix[0]) {
        for (int i = 0; i < n; i++) {
            if (strcmp(TZ_PRESETS[i].posix, tz_posix) == 0) {
                return TZ_PRESETS[i].label;
            }
        }
        return tz_posix;
    }
    return "UTC (+0)";
}
