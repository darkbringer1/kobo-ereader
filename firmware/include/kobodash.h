#ifndef KOBODASH_H
#define KOBODASH_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifdef __APPLE__
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#else
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#define KD_VERSION "0.2.0"
#define KD_WIDTH   600
#define KD_HEIGHT  800

#define KD_EXIT_STOCK_UI 0
#define KD_EXIT_REBOOT   42
#define KD_EXIT_POWEROFF 43

typedef enum {
    SCREEN_CLOCK = 0,
    SCREEN_LIBRARY = 1,
    SCREEN_DASHBOARD = 2,
    SCREEN_STATUS = 3,
    SCREEN_GALLERY = 4,
    SCREEN_SETTINGS = 5,
    SCREEN_READER = 6,
    SCREEN_COUNT = 7
} ScreenId;

typedef enum {
    KD_FONT_REGULAR = 0,
    KD_FONT_BOLD = 1,
    KD_FONT_CLOCK = 2
} KdFont;

typedef enum {
    KD_ALIGN_LEFT = 0,
    KD_ALIGN_CENTER = 1,
    KD_ALIGN_RIGHT = 2
} KdAlign;

typedef enum {
    KD_REFRESH_FAST_DU = 0,      /* WFM_DU, non-flashing (~260ms, ideal for clock digits & toggles) */
    KD_REFRESH_GRAY_PARTIAL = 1, /* WFM_GC16, non-flashing (crisp grayscale partial update) */
    KD_REFRESH_FULL_FLASH = 2    /* WFM_GC16, flashing (full anti-ghosting refresh) */
} KdRefreshMode;

typedef struct {
    int left;
    int top;
    int width;
    int height;
} KdRect;

typedef enum {
    SLEEP_STYLE_CYCLE = 0,    /* Rotate Today & Progress -> Calendar -> Stats */
    SLEEP_STYLE_TODAY = 1,    /* Today & Progress */
    SLEEP_STYLE_CALENDAR = 2, /* Calendar & Agenda */
    SLEEP_STYLE_STATS = 3,    /* Stats & Sports */
    SLEEP_STYLE_CLOCK = 4     /* Minimal Clock Lock Screen */
} SleepStyle;

typedef enum {
    LOCK_NONE = 0,
    LOCK_TOUCH = 1,      /* Screen visible & updating, all touch input ignored */
    LOCK_DEEP_SLEEP = 2  /* True hardware suspend-to-RAM with RTC wakealarm */
} LockMode;

typedef struct {
    int minute_clock;
    int day_interval_min;
    int night_interval_min;
    int day_start_hour;
    int night_start_hour;
    int auto_rotate_min;
    int auto_sleep_min;      /* 0=OFF, else inactivity timeout (5/10/15/30/60 min) */
    int sleep_style;         /* SleepStyle: 0=Cycle 3 pages, 1=Today, 2=Calendar, 3=Stats, 4=Clock */
    int sleep_rotate_hrs;    /* 0=fixed, else rotate sleep art every N hours (1/2/3/6) */
    int full_refresh_every;
    int invert_mode;
    int clock_24h;
    int gallery_clock_overlay;
    int start_screen;
    int touch_swap_xy;
    int touch_mirror_x;
    int touch_mirror_y;
    int reader_font_px;
    int reader_line_spacing; /* 0=tight, 1=normal, 2=wide */
    int reader_flash_every;
    char reader_font_family[24]; /* "georgia", "charter", "palatino", "baskerville", "times", "verdana", "trebuchet" */
    char dict_lang[16]; /* "en", "en-de", "de-en" */
    char tz_posix[64];
    char ntp_server[128];
    char server_url[256];
    char server_token[128];
    int push_mode; /* 0=poll only, 1=live SSE when charging (default), 2=always live SSE */
    char screensaver_dir[256];
    char cpu_governor[32];
    int auto_boot;
    char base_dir[256];
} Config;

typedef struct {
    int bat_pct;
    int bat_mv;
    int bat_ma;
    char bat_status[24];
    int charger_online;

    int wifi_up;
    char wifi_ip[32];
    char wifi_ssid[64];
    int wifi_rssi;
    unsigned long wifi_rx_kb;
    unsigned long wifi_tx_kb;
    int ssh_running;

    unsigned long mem_total_kb;
    unsigned long mem_avail_kb;
    unsigned long self_rss_kb;
    double load1;
    double load5;
    double load15;
    long uptime_sec;
    int cpu_khz;
    char cpu_gov[32];

    unsigned long onboard_total_mb;
    unsigned long onboard_free_mb;
    int sd_mounted;
    unsigned long sd_total_mb;
    unsigned long sd_free_mb;
} Telemetry;

#define KD_MAX_JSON_LINES   10
#define KD_MAX_DASH_PAGES   6
#define KD_MAX_PAGE_BARS    4
#define KD_MAX_PAGE_METRICS 4
#define KD_MAX_PAGE_ITEMS   6
#define KD_MAX_EVENTS       24
#define KD_MAX_ALERTS       8
#define KD_MAX_DISMISSED    32

typedef enum {
    DASH_PAGE_GENERIC = 0,
    DASH_PAGE_OVERVIEW = 1,
    DASH_PAGE_CALENDAR = 2,
    DASH_PAGE_LIST = 3,
    DASH_PAGE_STATS = 4
} DashPageKind;

typedef struct {
    char label[24];
    int pct;
    char detail[32];
} DashBar;

typedef struct {
    char label[32];
    char value[36];
    char detail[52];
} DashMetric;

typedef struct {
    char label[32];
    char value[104];
    char detail[104];
    bool urgent;
} DashItem;

typedef struct {
    char id[24];
    DashPageKind kind;
    char title[48];
    char subtitle[80];
    char badge[24];
    char hero[32];
    char hero_sub[88];
    char footer[96];

    DashBar bars[KD_MAX_PAGE_BARS];
    int bar_count;

    DashMetric metrics[KD_MAX_PAGE_METRICS];
    int metric_count;

    DashItem items[KD_MAX_PAGE_ITEMS];
    int item_count;

    int cal_year;
    int cal_month;
    int cal_today;
    uint32_t cal_marks_mask; /* bit (day-1) set if day has an event */
} DashPage;

typedef struct {
    char id[64];
    time_t start_ts;
    bool all_day;
    int remind_min;
    char title[96];
    char tag[32];
    char when_label[40];
} OfflineEvent;

typedef struct {
    char id[80];
    char title[96];
    char body[160];
    char source[36];
    char due[24];
    bool urgent;
    time_t ts;
} RemoteAlert;

typedef struct {
    bool has_bundle;
    time_t generated_ts;
    char updated[24];
    DashPage pages[KD_MAX_DASH_PAGES];
    int page_count;
    OfflineEvent events[KD_MAX_EVENTS];
    int event_count;
    RemoteAlert alerts[KD_MAX_ALERTS];
    int alert_count;
    char alerts_hash[24];
} DashboardBundle;

typedef struct {
    bool active;
    bool is_remote;
    bool urgent;
    char id[80];
    char badge[28];
    char title[96];
    char body[160];
    char meta[72];
} ActiveAlert;

typedef struct {
    char id[80];
    time_t until_ts; /* 0 = permanently dismissed, >0 = snoozed until epoch */
} DismissEntry;

typedef struct {
    ActiveAlert current;
    DismissEntry records[KD_MAX_DISMISSED];
    int record_count;
} AlertTracker;

typedef struct {
    bool in_progress;
    int last_ok; /* 0 = never synced, 1 = success, -1 = error */
    bool content_changed;
    time_t last_sync_time;
    time_t next_sync_time;
    char last_status[96];
    char last_etag[128];
    char cache_image_path[256];
    char cache_bundle_path[256];
    bool has_cached_image;
    bool has_bundle_json;
    bool is_json_dashboard;
    bool push_connected;
    char json_title[128];
    char json_subtitle[128];
    char json_lines[KD_MAX_JSON_LINES][128];
    int json_line_count;
    bool time_synced;
} SyncState;

#define KD_MAX_GALLERY 128

typedef struct {
    char paths[KD_MAX_GALLERY][256];
    int count;
    int index;
} Gallery;

typedef enum {
    INPUT_NONE = 0,
    INPUT_TAP,
    INPUT_LONG_TAP,
    INPUT_SWIPE_LEFT,
    INPUT_SWIPE_RIGHT,
    INPUT_SWIPE_UP,
    INPUT_SWIPE_DOWN,
    INPUT_HOME_SHORT,
    INPUT_HOME_LONG,
    INPUT_POWER_SHORT,
    INPUT_POWER_LONG
} InputType;

typedef struct {
    InputType type;
    int x;
    int y;
} InputEvent;

typedef struct {
    uint8_t pixels[KD_WIDTH * KD_HEIGHT];
} Canvas;

typedef enum {
    SET_TAB_GENERAL = 0,
    SET_TAB_WIFI = 1,
    SET_TAB_SERVER = 2,
    SET_TAB_DEVICE = 3,
    SET_TAB_COUNT = 4
} SettingsTab;

#define KD_MAX_WIFI_NETS 16

typedef struct {
    char ssid[64];
    int rssi;
    bool secured;
    bool saved;
    bool connected;
} WifiNetwork;

typedef struct {
    WifiNetwork nets[KD_MAX_WIFI_NETS];
    int count;
    int page;
    char status_msg[96];
    char pending_ssid[64];
    bool pending_secured;
} WifiState;

typedef enum {
    KBD_NONE = 0,
    KBD_WIFI_PASS,
    KBD_WIFI_SSID,
    KBD_SERVER_URL,
    KBD_SERVER_TOKEN,
    KBD_NTP_SERVER,
    KBD_SCREENSAVER_DIR
} KeyboardPurpose;

typedef struct {
    bool active;
    KeyboardPurpose purpose;
    char title[80];
    char hint[96];
    char buf[256];
    size_t max_len;
    bool shift;
    bool symbols;
    bool password;
    bool show_secret;
    bool url_presets;
    bool ntp_presets;
} KeyboardState;

typedef enum {
    ACT_NONE = 0,
    ACT_NEXT_SCREEN,
    ACT_PREV_SCREEN,
    ACT_OPEN_SETTINGS,
    ACT_OPEN_SETTINGS_TAB,
    ACT_CLOSE_SETTINGS,
    ACT_SET_TAB,
    ACT_GOTO_SCREEN,
    ACT_GOTO_DASH_PAGE,
    ACT_GALLERY_NEXT,
    ACT_SYNC_NOW,
    ACT_CLEAR_ETAG_SYNC,
    ACT_FULL_REFRESH,
    ACT_TOGGLE_MINUTE_CLOCK,
    ACT_CYCLE_DAY_INTERVAL,
    ACT_CYCLE_NIGHT_INTERVAL,
    ACT_CYCLE_DAY_START,
    ACT_CYCLE_NIGHT_START,
    ACT_CYCLE_AUTO_ROTATE,
    ACT_CYCLE_AUTO_SLEEP,
    ACT_CYCLE_SLEEP_STYLE,
    ACT_CYCLE_SLEEP_ROTATE,
    ACT_LOCK_OR_SLEEP,
    ACT_CYCLE_TIMEZONE,
    ACT_CYCLE_START_SCREEN,
    ACT_CYCLE_FULL_REFRESH,
    ACT_CYCLE_READER_FONT,
    ACT_CYCLE_DICT_LANG,
    ACT_CYCLE_READER_FLASH,
    ACT_CYCLE_PUSH_MODE,
    ACT_TOGGLE_GALLERY_CLOCK,
    ACT_TOGGLE_THEME,
    ACT_TOGGLE_24H,
    ACT_TOGGLE_SSH,
    ACT_WIFI_TOGGLE,
    ACT_WIFI_SCAN,
    ACT_WIFI_RECONNECT,
    ACT_WIFI_HIDDEN,
    ACT_WIFI_SELECT_NET,
    ACT_WIFI_FORGET_NET,
    ACT_WIFI_PAGE_PREV,
    ACT_WIFI_PAGE_NEXT,
    ACT_EDIT_SERVER_URL,
    ACT_EDIT_SERVER_TOKEN,
    ACT_EDIT_NTP_SERVER,
    ACT_EDIT_SCREENSAVER_DIR,
    ACT_CYCLE_CPU_GOV,
    ACT_TOGGLE_TOUCH_SWAP,
    ACT_TOGGLE_TOUCH_MIRROR_X,
    ACT_TOGGLE_TOUCH_MIRROR_Y,
    ACT_TOGGLE_SD_MOUNT,
    ACT_TOGGLE_AUTO_BOOT,
    ACT_EXIT_STOCK_UI,
    ACT_REBOOT,
    ACT_POWEROFF
} UiActionType;

typedef struct {
    UiActionType type;
    int arg;
    KdRect dirty_rect;
} UiAction;

/* config.c */
void config_set_defaults(Config *cfg, const char *base_dir);
int config_load(Config *cfg, const char *path);
int config_save(const Config *cfg, const char *path);
int config_active_interval_min(const Config *cfg, const struct tm *now_tm);
bool config_is_daytime(const Config *cfg, const struct tm *now_tm);
int config_next_day_interval(int cur);
int config_next_night_interval(int cur);
int config_next_rotate_interval(int cur);
int config_next_auto_sleep(int cur);
int config_next_sleep_style(int cur);
const char *config_sleep_style_label(int style);
int config_next_sleep_rotate(int cur);
int config_next_full_refresh(int cur);
int config_next_start_screen(int cur);
int config_next_reader_font(int cur);
int config_next_reader_flash(int cur);
const char *config_next_font_family(const char *cur);
const char *config_font_family_label(const char *id);
const char *config_next_dict_lang(const char *cur);
const char *config_push_mode_label(int mode);
void config_next_timezone(const char *cur, char *out_posix, size_t out_sz);
const char *config_tz_label(const char *tz_posix);

/* canvas.c */
void canvas_clear(Canvas *c, uint8_t gray);
void canvas_fill_rect(Canvas *c, int x, int y, int w, int h, uint8_t gray);
void canvas_stroke_rect(Canvas *c, int x, int y, int w, int h, int thickness, uint8_t gray);
void canvas_fill_rrect(Canvas *c, int x, int y, int w, int h, int r, uint8_t gray);
void canvas_stroke_rrect(Canvas *c, int x, int y, int w, int h, int r, int thickness, uint8_t gray);
void canvas_hline(Canvas *c, int x, int y, int w, int thickness, uint8_t gray);
void canvas_vline(Canvas *c, int x, int y, int h, int thickness, uint8_t gray);
void canvas_fill_circle(Canvas *c, int cx, int cy, int r, uint8_t gray);
void canvas_stroke_circle(Canvas *c, int cx, int cy, int r, int thickness, uint8_t gray);
void canvas_draw_battery(Canvas *c, int x, int y, int pct, int charging, uint8_t fg, uint8_t bg);
void canvas_draw_wifi(Canvas *c, int x, int y, int up, int rssi, uint8_t fg, uint8_t muted);
void canvas_draw_gear(Canvas *c, int cx, int cy, int r, uint8_t fg, uint8_t bg);
void canvas_draw_refresh_icon(Canvas *c, int cx, int cy, int r, uint8_t fg, uint8_t bg);
void canvas_draw_lock(Canvas *c, int cx, int cy, uint8_t fg, uint8_t bg);
void canvas_draw_progress(Canvas *c, int x, int y, int w, int h, int pct, uint8_t fg, uint8_t bg, uint8_t border);
void canvas_invert(Canvas *c);

/* gallery.c */
void gallery_scan(Gallery *g, const Config *cfg);
const char *gallery_current(const Gallery *g);
void gallery_advance(Gallery *g, int delta);

/* sysinfo.c */
void sysinfo_collect(Telemetry *t, const Config *cfg);
int sysinfo_toggle_ssh(const Config *cfg, bool enable);
/* True hardware suspend-to-RAM on Kobo Touch A with optional RTC wakealarm (0=none).
 * Returns 0 on suspend+resume, -1 if unsupported (host build). */
int sysinfo_enter_deep_sleep(const Config *cfg, time_t wake_utc);

/* net.c */
bool net_parse_http_date(const char *hdr, time_t *out_utc);
bool net_extract_json_string(const char *json, const char *key, char *out, size_t out_sz);
bool net_parse_dashboard_json(const char *json, SyncState *st);
bool net_sntp_sync(const char *server, time_t *out_utc);
int net_sync_blocking(const Config *cfg, const Telemetry *tel, SyncState *st);
int net_start_async_sync(const Config *cfg, const Telemetry *tel, const SyncState *prev_st,
                         const char *result_path, int *out_pipe_fd, pid_t *out_pid);
bool net_read_sync_result(const char *result_path, SyncState *st);
void net_ensure_wifi(const Config *cfg);
bool net_derive_stream_url(const char *server_url, char *out_url, size_t out_sz);
int net_start_push_watcher(const Config *cfg, const char *alerts_out_path,
                           int *out_pipe_fd, pid_t *out_pid);

/* dashboard.c (Multi-page bundle parser, offline calendar triggers, remote push alerts & renderer) */
bool dashboard_parse_bundle(const char *json, DashboardBundle *out);
bool dashboard_parse_alerts_update(const char *json, DashboardBundle *bundle);
bool dashboard_load_cached(const char *path, DashboardBundle *out);
void dashboard_load_dismissed(const char *path, AlertTracker *tr);
void dashboard_save_dismissed(const char *path, const AlertTracker *tr);
bool dashboard_check_triggers(const DashboardBundle *b, AlertTracker *tr, time_t now);
void dashboard_dismiss_active(AlertTracker *tr, const char *persist_path);
void dashboard_snooze_active(AlertTracker *tr, time_t now, int snooze_min, const char *persist_path);
bool dashboard_clock_strip(const DashboardBundle *b, time_t now,
                           char *out_text, size_t out_sz, int *out_target_page);
void dashboard_compute_local_bars(const struct tm *now_tm, DashBar out[4]);
void dashboard_render_page(Canvas *c, const DashboardBundle *b, int page_idx,
                           const Config *cfg, const Telemetry *tel,
                           const SyncState *sync, const Gallery *gal,
                           const struct tm *now_tm, KdRefreshMode mode);
int dashboard_pick_sleep_page(const DashboardBundle *b, int sleep_style, int cycle_idx);
void dashboard_render_sleep_screen(Canvas *c, const DashboardBundle *b,
                                   int sleep_style, int cycle_idx, LockMode lock_mode,
                                   const Config *cfg, const Telemetry *tel,
                                   const SyncState *sync, const struct tm *now_tm,
                                   KdRefreshMode mode);
void dashboard_render_alert_overlay(Canvas *c, const ActiveAlert *al,
                                    const Config *cfg, KdRefreshMode mode);
/* Returns: 0=no-op, 1=dismiss, 2=snooze 30m, 3=open waiting/calendar page, 4=exit stock UI */
int dashboard_handle_alert_input(const ActiveAlert *al, const InputEvent *ev);

/* input.c */
typedef struct InputState InputState;
InputState *input_open(const Config *cfg);
void input_update_config(InputState *in, const Config *cfg);
void input_close(InputState *in);
int input_get_fds(const InputState *in, int *fds, int max_fds);
int input_poll_timeout_ms(const InputState *in, uint64_t now_ms);
bool input_read_event(InputState *in, int ready_fd, uint64_t now_ms, InputEvent *out);
bool input_check_timers(InputState *in, uint64_t now_ms, InputEvent *out);
bool input_drain_wakeup(InputState *in);
void input_transform_coords(const Config *cfg, int raw_x, int raw_y, int raw_max_x, int raw_max_y, int *sx, int *sy);

/* display_*.c */
const char *display_backend_version(void);
int display_init(const Config *cfg);
void display_close(void);
int display_blit_canvas(const Canvas *c, const KdRect *region);
int display_text(const char *text, KdFont font, int size_px, KdAlign align,
                 int x, int y, int w, int h, uint8_t fg_gray, bool bgless, uint8_t bg_gray);
int display_image_file(const char *path, int x, int y, int max_w, int max_h, bool dither, bool invert);
int display_refresh(const KdRect *region, KdRefreshMode mode);
void display_wait_complete(void);
void display_show_banner(const char *line1, const char *line2, bool invert);

typedef enum {
    BOOK_FMT_NONE = 0,
    BOOK_FMT_TXT,
    BOOK_FMT_EPUB,
    BOOK_FMT_MOBI
} BookFormat;

typedef struct {
    char name[160];
    char full_path[384];
    bool is_dir;
    BookFormat fmt;
    int progress_pct; /* -1 = unread, 0..100 */
    long last_read;
} LibEntry;

#define KD_MAX_LIB_ENTRIES 512

typedef struct {
    char root_dir[256];
    char cur_dir[384];
    LibEntry entries[KD_MAX_LIB_ENTRIES];
    int count;
    int page;
    /* Most recently read book for 1-tap Continue Reading */
    bool has_recent;
    char recent_path[384];
    char recent_title[160];
    BookFormat recent_fmt;
    int recent_pct;
} Library;

typedef struct Book Book;
typedef struct Dict Dict;
typedef struct ReaderState ReaderState;

/* font.c */
typedef enum {
    FONT_STYLE_SERIF_REG = 0,
    FONT_STYLE_SERIF_BOLD = 1,
    FONT_STYLE_SERIF_ITALIC = 2,
    FONT_STYLE_UI_REG = 3,
    FONT_STYLE_UI_BOLD = 4
} FontStyle;

int font_init(const Config *cfg);
int font_set_book_family(const Config *cfg, const char *family);
void font_close(void);
bool font_is_ready(void);
void font_line_metrics(FontStyle style, int size_px, int *ascent, int *descent, int *line_h);
int font_measure_utf8(FontStyle style, int size_px, const char *utf8, size_t len);
int font_draw_utf8(Canvas *c, FontStyle style, int size_px, int x, int baseline_y,
                   const char *utf8, size_t len, uint8_t fg_gray);
int font_draw_wrapped(Canvas *c, FontStyle style, int size_px, int x, int y,
                      int max_w, int max_h, int extra_lead, const char *utf8, uint8_t fg_gray);

/* dict.c */
#define KD_MAX_DICTS 4

typedef struct DictSet DictSet;

Dict *dict_open(const char *path);
void dict_close(Dict *d);
uint32_t dict_entry_count(const Dict *d);
bool dict_lookup_lang(const Dict *d, const char *lang_id, const char *raw_word,
                      char *matched_word, size_t matched_sz,
                      char *def_out, size_t def_sz);
bool dict_lookup(const Dict *d, const char *raw_word,
                 char *matched_word, size_t matched_sz,
                 char *def_out, size_t def_sz);

DictSet *dictset_open_dir(const char *dict_dir);
void dictset_close(DictSet *ds);
int dictset_count(const DictSet *ds);
int dictset_find_by_id(const DictSet *ds, const char *id);
const char *dictset_id(const DictSet *ds, int idx);
const char *dictset_label(const DictSet *ds, int idx);
bool dictset_lookup(const DictSet *ds, int preferred_idx, bool allow_fallback,
                    const char *raw_word, int *used_idx,
                    char *matched_word, size_t matched_sz,
                    char *def_out, size_t def_sz);

/* book.c */
BookFormat book_detect_format(const char *path);
const char *book_format_tag(BookFormat fmt);
Book *book_open(const char *path);
void book_close(Book *b);
const char *book_title(const Book *b);
BookFormat book_format(const Book *b);
int book_chapter_count(const Book *b);
size_t book_total_size(const Book *b);
size_t book_chapter_start_offset(const Book *b, int ch_idx);
char *book_load_chapter(Book *b, int ch_idx, size_t *out_len);

void library_init(Library *lib, const char *root_dir, const Config *cfg);
void library_scan(Library *lib, const Config *cfg);
bool library_enter_dir(Library *lib, const char *dir_path, const Config *cfg);
bool library_go_up(Library *lib, const Config *cfg);
bool library_get_progress(const Config *cfg, const char *book_path, int *ch_idx, size_t *byte_off, int *pct);
void library_save_progress(const Config *cfg, const char *book_path, const char *title,
                           BookFormat fmt, int ch_idx, size_t byte_off, int pct);

/* reader.c */
ReaderState *reader_create(const Config *cfg, DictSet *dicts);
void reader_destroy(ReaderState *rs);
bool reader_open_book(ReaderState *rs, const char *path, const Config *cfg);
void reader_close_book(ReaderState *rs, const Config *cfg);
bool reader_has_book(const ReaderState *rs);
void reader_render_library(Canvas *c, const Library *lib, const Config *cfg,
                           const Telemetry *tel, const SyncState *sync,
                           const Gallery *gal, KdRefreshMode mode);
void reader_render_page(Canvas *c, ReaderState *rs, const Config *cfg,
                        const Telemetry *tel, const struct tm *now_tm, KdRefreshMode mode);
bool reader_handle_library_input(Library *lib, ReaderState *rs, const InputEvent *ev,
                                 const Config *cfg, ScreenId *out_screen,
                                 bool *out_open_settings, bool *out_exit_stock,
                                 bool *out_prev_screen, bool *out_next_screen,
                                 bool *out_sync_now, bool *out_need_full);
bool reader_handle_book_input(ReaderState *rs, Library *lib, const InputEvent *ev,
                              Config *cfg, bool *cfg_dirty,
                              ScreenId *out_screen, bool *out_exit_stock,
                              KdRefreshMode *out_refresh_mode);
void reader_debug_lookup_word(ReaderState *rs, const char *word);

/* wifi.c (Wi-Fi & Device hardware control) */
int wifi_parse_scan_results(const char *scan_raw, const char *saved_raw,
                            const char *active_ssid, WifiState *ws);
int wifi_set_power(const Config *cfg, bool on, char *status_out, size_t status_sz);
int wifi_hard_reset(const Config *cfg, char *status_out, size_t status_sz);
int wifi_scan(const Config *cfg, const Telemetry *tel, WifiState *ws);
int wifi_connect(const Config *cfg, const char *ssid, const char *psk,
                 char *status_out, size_t status_sz);
int wifi_forget(const Config *cfg, const char *ssid, char *status_out, size_t status_sz);
int device_list_governors(char out[][24], int max_out);
int device_set_governor(const char *gov);
int device_toggle_sd(const Config *cfg, bool mount_it, char *status_out, size_t status_sz);

/* keyboard.c (On-screen e-ink touch keyboard) */
typedef enum {
    KBD_RESULT_NONE = 0,
    KBD_RESULT_REDRAW_INPUT,
    KBD_RESULT_REDRAW_ALL,
    KBD_RESULT_SUBMIT,
    KBD_RESULT_CANCEL
} KbdResult;

void kbd_open(KeyboardState *kb, KeyboardPurpose purpose, const char *title,
              const char *hint, const char *initial, size_t max_len, bool password);
void kbd_render(Canvas *c, const KeyboardState *kb, const Config *cfg,
                bool input_only, KdRefreshMode mode);
KbdResult kbd_handle_input(KeyboardState *kb, const InputEvent *ev);

/* ui.c */
void ui_set_settings_tab(SettingsTab tab);
SettingsTab ui_get_settings_tab(void);
void ui_set_wifi_state(const WifiState *ws);
void ui_set_dashboard_bundle(const DashboardBundle *b, int page_idx);
int ui_get_dashboard_page(void);
void ui_set_power_confirm(int which); /* 0=none, 1=reboot, 2=poweroff */
int ui_get_power_confirm(void);
void ui_set_device_notice(const char *msg);
void ui_render_screen(Canvas *c, ScreenId screen, const Config *cfg,
                      const Telemetry *tel, const SyncState *sync,
                      const Gallery *gal, const struct tm *now_tm,
                      KdRefreshMode mode);
void ui_render_clock_digits_only(Canvas *c, const Config *cfg, const struct tm *now_tm,
                                 KdRefreshMode mode);
UiAction ui_handle_input(ScreenId current, ScreenId prev_non_settings,
                         const InputEvent *ev, const Config *cfg, const Gallery *gal);
ScreenId ui_next_screen(ScreenId cur, int delta, const Gallery *gal);
void ui_step_carousel(ScreenId cur, int cur_dash_page, int delta,
                      const Gallery *gal, const DashboardBundle *b,
                      bool skip_library, ScreenId *out_screen, int *out_dash_page);
const char *ui_screen_name(ScreenId s);

#endif /* KOBODASH_H */
