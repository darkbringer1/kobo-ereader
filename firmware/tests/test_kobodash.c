#include "kobodash.h"

#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* Host display helpers from display_host.c */
extern int display_host_get_refresh_count(void);
extern KdRefreshMode display_host_get_last_mode(void);
extern KdRect display_host_get_last_rect(void);
extern void display_host_begin_capture(const char *text_log_path);
extern int display_host_save_pgm(const char *pgm_path);

static int g_passed = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
    g_passed++; \
} while (0)

static void test_config_roundtrip(void) {
    Config c1, c2;
    config_set_defaults(&c1, "/tmp/kd-base");
    c1.minute_clock = 0;
    c1.day_interval_min = 60;
    c1.night_interval_min = 240;
    c1.day_start_hour = 8;
    c1.night_start_hour = 22;
    c1.auto_rotate_min = 15;
    c1.invert_mode = 1;
    c1.clock_24h = 0;
    c1.auto_boot = 0;
    snprintf(c1.server_url, sizeof(c1.server_url), "https://dashboard.example.com/kobo/bundle");

    const char *path = "/tmp/test_kobodash_cfg.ini";
    CHECK(config_save(&c1, path) == 0);
    config_set_defaults(&c2, "/tmp/kd-base");
    CHECK(config_load(&c2, path) == 0);
    unlink(path);

    CHECK(c2.minute_clock == 0);
    CHECK(c2.day_interval_min == 60);
    CHECK(c2.night_interval_min == 240);
    CHECK(c2.day_start_hour == 8);
    CHECK(c2.night_start_hour == 22);
    CHECK(c2.auto_rotate_min == 15);
    CHECK(c2.invert_mode == 1);
    CHECK(c2.clock_24h == 0);
    CHECK(c2.auto_boot == 0);
    CHECK(strcmp(c2.server_url, "https://dashboard.example.com/kobo/bundle") == 0);
}

static void test_day_night_schedule(void) {
    Config cfg;
    config_set_defaults(&cfg, "/tmp");
    cfg.day_start_hour = 7;
    cfg.night_start_hour = 23;
    cfg.day_interval_min = 30;
    cfg.night_interval_min = 120;

    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_hour = 6;
    CHECK(!config_is_daytime(&cfg, &t));
    CHECK(config_active_interval_min(&cfg, &t) == 120);

    t.tm_hour = 7;
    CHECK(config_is_daytime(&cfg, &t));
    CHECK(config_active_interval_min(&cfg, &t) == 30);

    t.tm_hour = 22;
    CHECK(config_is_daytime(&cfg, &t));
    CHECK(config_active_interval_min(&cfg, &t) == 30);

    t.tm_hour = 23;
    CHECK(!config_is_daytime(&cfg, &t));
    CHECK(config_active_interval_min(&cfg, &t) == 120);

    CHECK(config_next_day_interval(30) == 60);
    CHECK(config_next_day_interval(120) == 1);
    CHECK(config_next_night_interval(120) == 240);
    CHECK(config_next_night_interval(240) == 0);
    CHECK(config_next_night_interval(0) == 30);
    CHECK(config_next_rotate_interval(0) == 1);
    CHECK(config_next_rotate_interval(60) == 0);
}

static void test_touch_transform(void) {
    Config cfg;
    config_set_defaults(&cfg, "/tmp");
    /* Kobo Touch A defaults: swap_xy=1, mirror_x=1, mirror_y=0 */
    int sx = 0, sy = 0;

    /* Raw (0, 0) -> swapped (0, 0) -> mirror_x -> (599, 0) */
    input_transform_coords(&cfg, 0, 0, 799, 599, &sx, &sy);
    CHECK(sx == 599 && sy == 0);

    /* Raw (799, 599) -> swapped (599, 799) -> mirror_x -> (0, 799) */
    input_transform_coords(&cfg, 799, 599, 799, 599, &sx, &sy);
    CHECK(sx == 0 && sy == 799);

    /* Center (400, 300) -> (299, 400) */
    input_transform_coords(&cfg, 400, 300, 799, 599, &sx, &sy);
    CHECK(sx == 299 && sy == 400);
}

static void test_net_parsers(void) {
    time_t utc = 0;
    CHECK(net_parse_http_date("Sat, 21 Nov 2026 20:55:00 GMT", &utc));
    struct tm tmv;
    gmtime_r(&utc, &tmv);
    CHECK(tmv.tm_year + 1900 == 2026);
    CHECK(tmv.tm_mon == 10); /* Nov */
    CHECK(tmv.tm_mday == 21);
    CHECK(tmv.tm_hour == 20);
    CHECK(tmv.tm_min == 55);

    char val[256] = {0};
    const char *sample = "{\"title\":\"Home Server\",\"note\":\"ready\"}";
    CHECK(net_extract_json_string(sample, "note", val, sizeof(val)));
    CHECK(strcmp(val, "ready") == 0);

    SyncState st;
    memset(&st, 0, sizeof(st));
    const char *dash = "{\"title\":\"Home Server\",\"subtitle\":\"All systems nominal\","
                       "\"lines\":[\"CPU: 14%\",\"RAM: 6.2 / 16 GB\",\"Backup: OK\"]}";
    CHECK(net_parse_dashboard_json(dash, &st));
    CHECK(st.is_json_dashboard);
    CHECK(strcmp(st.json_title, "Home Server") == 0);
    CHECK(st.json_line_count == 3);
    CHECK(strcmp(st.json_lines[2], "Backup: OK") == 0);
}

static void test_http_client_loopback(void) {
    /* Start ephemeral loopback HTTP server serving 3 requests:
       1) PNG image with ETag "v1"
       2) 304 Not Modified when If-None-Match: "v1" is received
       3) Chunked JSON dashboard response */
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(srv >= 0);
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    CHECK(bind(srv, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    CHECK(listen(srv, 4) == 0);

    socklen_t alen = sizeof(addr);
    CHECK(getsockname(srv, (struct sockaddr *)&addr, &alen) == 0);
    int port = ntohs(addr.sin_port);

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        for (int req_i = 0; req_i < 3; req_i++) {
            int cfd = accept(srv, NULL, NULL);
            if (cfd < 0) _exit(2);
            char req[2048];
            ssize_t n = recv(cfd, req, sizeof(req) - 1, 0);
            if (n < 0) n = 0;
            req[n] = '\0';

            if (req_i == 0) {
                const unsigned char png_stub[16] = {
                    0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
                    0, 0, 0, 0, 'I', 'E', 'N', 'D'
                };
                const char *hdr =
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: image/png\r\n"
                    "ETag: \"v1\"\r\n"
                    "Date: Sat, 21 Nov 2026 21:00:00 GMT\r\n"
                    "Content-Length: 16\r\n"
                    "Connection: close\r\n\r\n";
                (void)send(cfd, hdr, strlen(hdr), 0);
                (void)send(cfd, png_stub, sizeof(png_stub), 0);
            } else if (req_i == 1) {
                const char *resp = strstr(req, "If-None-Match: \"v1\"")
                    ? "HTTP/1.1 304 Not Modified\r\nETag: \"v1\"\r\nConnection: close\r\n\r\n"
                    : "HTTP/1.1 500 Missing ETag\r\nConnection: close\r\n\r\n";
                (void)send(cfd, resp, strlen(resp), 0);
            } else {
                const char *json = "{\"title\":\"My Server\",\"subtitle\":\"LAN\",\"lines\":[\"Line 1\",\"Line 2\"]}";
                char resp[512];
                int rlen = snprintf(resp, sizeof(resp),
                                    "HTTP/1.1 200 OK\r\n"
                                    "Transfer-Encoding: chunked\r\n"
                                    "Connection: close\r\n\r\n"
                                    "%zx\r\n%s\r\n0\r\n\r\n",
                                    strlen(json), json);
                (void)send(cfd, resp, (size_t)rlen, 0);
            }
            close(cfd);
        }
        close(srv);
        _exit(0);
    }
    close(srv);

    const char *base = "/tmp/test_kd_http";
    mkdir(base, 0755);
    Config cfg;
    config_set_defaults(&cfg, base);
    cfg.ntp_server[0] = '\0'; /* skip NTP in offline unit test */
    snprintf(cfg.server_url, sizeof(cfg.server_url), "http://127.0.0.1:%d/dashboard.png", port);

    Telemetry tel;
    memset(&tel, 0, sizeof(tel));
    tel.bat_pct = 82;
    tel.wifi_rssi = -50;

    SyncState st;
    memset(&st, 0, sizeof(st));
    snprintf(st.cache_image_path, sizeof(st.cache_image_path), "%s/cache/dashboard.img", base);

    /* Request 1: Downloads PNG + records ETag */
    CHECK(net_sync_blocking(&cfg, &tel, &st) == 0);
    CHECK(st.last_ok == 1);
    CHECK(st.has_cached_image);
    CHECK(st.content_changed);
    CHECK(strcmp(st.last_etag, "\"v1\"") == 0);

    /* Request 2: Sends If-None-Match -> receives 304 -> content_changed == false */
    CHECK(net_sync_blocking(&cfg, &tel, &st) == 0);
    CHECK(st.last_ok == 1);
    CHECK(!st.content_changed);
    CHECK(strstr(st.last_status, "304") != NULL);

    /* Request 3: Chunked JSON response */
    st.last_etag[0] = '\0';
    CHECK(net_sync_blocking(&cfg, &tel, &st) == 0);
    CHECK(st.is_json_dashboard);
    CHECK(st.content_changed);
    CHECK(strcmp(st.json_title, "My Server") == 0);
    CHECK(st.json_line_count == 2);

    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    unlink(st.cache_image_path);
    rmdir("/tmp/test_kd_http/cache");
    rmdir(base);
}

static void test_gallery_scan(void) {
    const char *dir = "/tmp/test_kd_gallery";
    mkdir(dir, 0755);
    FILE *f1 = fopen("/tmp/test_kd_gallery/b_art.jpg", "wb");
    fputs("jpg", f1); fclose(f1);
    FILE *f2 = fopen("/tmp/test_kd_gallery/a_cover.png", "wb");
    fputs("png", f2); fclose(f2);
    FILE *f3 = fopen("/tmp/test_kd_gallery/notes.txt", "wb");
    fputs("txt", f3); fclose(f3);

    Config cfg;
    config_set_defaults(&cfg, "/tmp/no_such_base");
    snprintf(cfg.screensaver_dir, sizeof(cfg.screensaver_dir), "%s", dir);

    Gallery g;
    memset(&g, 0, sizeof(g));
    gallery_scan(&g, &cfg);
    CHECK(g.count == 2);
    CHECK(strstr(gallery_current(&g), "a_cover.png") != NULL);
    gallery_advance(&g, 1);
    CHECK(strstr(gallery_current(&g), "b_art.jpg") != NULL);
    gallery_advance(&g, 1);
    CHECK(strstr(gallery_current(&g), "a_cover.png") != NULL);

    unlink("/tmp/test_kd_gallery/b_art.jpg");
    unlink("/tmp/test_kd_gallery/a_cover.png");
    unlink("/tmp/test_kd_gallery/notes.txt");
    rmdir(dir);
}

static void test_ui_hit_and_partial_refresh(void) {
    Config cfg;
    config_set_defaults(&cfg, "/tmp");
    Gallery empty_gal;
    memset(&empty_gal, 0, sizeof(empty_gal));
    Gallery full_gal;
    memset(&full_gal, 0, sizeof(full_gal));
    full_gal.count = 2;

    /* Screen rotation: CLOCK -> LIBRARY -> DASHBOARD -> STATUS -> [GALLERY] -> CLOCK */
    CHECK(ui_next_screen(SCREEN_CLOCK, +1, &empty_gal) == SCREEN_LIBRARY);
    CHECK(ui_next_screen(SCREEN_STATUS, +1, &empty_gal) == SCREEN_CLOCK);
    CHECK(ui_next_screen(SCREEN_STATUS, +1, &full_gal) == SCREEN_GALLERY);
    CHECK(ui_next_screen(SCREEN_GALLERY, +1, &full_gal) == SCREEN_CLOCK);

    /* Hold HOME anywhere -> exit to Stock UI */
    InputEvent ev = { INPUT_HOME_LONG, 0, 0 };
    UiAction a = ui_handle_input(SCREEN_CLOCK, SCREEN_CLOCK, &ev, &cfg, &empty_gal);
    CHECK(a.type == ACT_EXIT_STOCK_UI);

    /* Tap top-right gear -> open settings */
    ev = (InputEvent){ INPUT_TAP, 560, 30 };
    a = ui_handle_input(SCREEN_CLOCK, SCREEN_CLOCK, &ev, &cfg, &empty_gal);
    CHECK(a.type == ACT_OPEN_SETTINGS);

    /* Tap Settings tab bar -> WI-FI, SERVER, DEVICE, back to GENERAL */
    ui_set_settings_tab(SET_TAB_GENERAL);
    ev = (InputEvent){ INPUT_TAP, 220, 86 };
    a = ui_handle_input(SCREEN_SETTINGS, SCREEN_CLOCK, &ev, &cfg, &empty_gal);
    CHECK(a.type == ACT_SET_TAB && a.arg == SET_TAB_WIFI);

    ev = (InputEvent){ INPUT_TAP, 360, 86 };
    a = ui_handle_input(SCREEN_SETTINGS, SCREEN_CLOCK, &ev, &cfg, &empty_gal);
    CHECK(a.type == ACT_SET_TAB && a.arg == SET_TAB_SERVER);

    ev = (InputEvent){ INPUT_TAP, 500, 86 };
    a = ui_handle_input(SCREEN_SETTINGS, SCREEN_CLOCK, &ev, &cfg, &empty_gal);
    CHECK(a.type == ACT_SET_TAB && a.arg == SET_TAB_DEVICE);

    /* Tap Settings GENERAL row 0 -> toggle minute clock (y=118..174) */
    ui_set_settings_tab(SET_TAB_GENERAL);
    ev = (InputEvent){ INPUT_TAP, 420, 146 };
    a = ui_handle_input(SCREEN_SETTINGS, SCREEN_CLOCK, &ev, &cfg, &empty_gal);
    CHECK(a.type == ACT_TOGGLE_MINUTE_CLOCK);

    /* Tap Settings SERVER row 0 -> edit server URL (y=202..256) */
    ui_set_settings_tab(SET_TAB_SERVER);
    ev = (InputEvent){ INPUT_TAP, 360, 228 };
    a = ui_handle_input(SCREEN_SETTINGS, SCREEN_CLOCK, &ev, &cfg, &empty_gal);
    CHECK(a.type == ACT_EDIT_SERVER_URL);

    /* Tap Settings DEVICE row 0 -> cycle CPU governor, row 6 -> Auto-Boot toggle, bottom -> Stock Kobo UI */
    ui_set_settings_tab(SET_TAB_DEVICE);
    ev = (InputEvent){ INPUT_TAP, 420, 146 };
    a = ui_handle_input(SCREEN_SETTINGS, SCREEN_CLOCK, &ev, &cfg, &empty_gal);
    CHECK(a.type == ACT_CYCLE_CPU_GOV);

    ev = (InputEvent){ INPUT_TAP, 420, 118 + 6 * 63 + 20 };
    a = ui_handle_input(SCREEN_SETTINGS, SCREEN_CLOCK, &ev, &cfg, &empty_gal);
    CHECK(a.type == ACT_TOGGLE_AUTO_BOOT);

    ev = (InputEvent){ INPUT_TAP, 140, 680 };
    a = ui_handle_input(SCREEN_SETTINGS, SCREEN_CLOCK, &ev, &cfg, &empty_gal);
    CHECK(a.type == ACT_EXIT_STOCK_UI);

    /* Tap Dashboard unconfigured setup buttons -> Server tab & Wi-Fi tab */
    cfg.server_url[0] = '\0';
    ev = (InputEvent){ INPUT_TAP, 160, 530 };
    a = ui_handle_input(SCREEN_DASHBOARD, SCREEN_CLOCK, &ev, &cfg, &empty_gal);
    CHECK(a.type == ACT_OPEN_SETTINGS_TAB && a.arg == SET_TAB_SERVER);
    ev = (InputEvent){ INPUT_TAP, 420, 530 };
    a = ui_handle_input(SCREEN_DASHBOARD, SCREEN_CLOCK, &ev, &cfg, &empty_gal);
    CHECK(a.type == ACT_OPEN_SETTINGS_TAB && a.arg == SET_TAB_WIFI);

    ui_set_settings_tab(SET_TAB_GENERAL);

    /* Verify partial minute clock refresh uses WFM_DU and bounded rect */
    static Canvas c;
    display_init(&cfg);
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = 126; tmv.tm_mon = 10; tmv.tm_mday = 21;
    tmv.tm_wday = 6; tmv.tm_hour = 21; tmv.tm_min = 47;

    ui_render_clock_digits_only(&c, &cfg, &tmv, KD_REFRESH_FAST_DU);
    CHECK(display_host_get_last_mode() == KD_REFRESH_FAST_DU);
    KdRect r = display_host_get_last_rect();
    CHECK(r.width < KD_WIDTH && r.height < 300);
    display_close();
}

static void test_dict_and_books(void) {
    /* 1. Test Multi-Dictionary DictSet (EN WordNet + EN->DE + DE->EN FreeDict) */
    DictSet *ds = dictset_open_dir("../output");
    CHECK(ds != NULL);
    CHECK(dictset_count(ds) >= 3);

    int en_idx = dictset_find_by_id(ds, "en");
    int ende_idx = dictset_find_by_id(ds, "en-de");
    int deen_idx = dictset_find_by_id(ds, "de-en");
    int used = -1;

    char matched[64] = {0}, def[512] = {0};
    CHECK(dictset_lookup(ds, en_idx, false, "citadel", &used, matched, sizeof(matched), def, sizeof(def)));
    CHECK(used == en_idx && strcmp(matched, "citadel") == 0);
    CHECK(strstr(def, "stronghold") != NULL);

    /* English lemmatization */
    CHECK(dictset_lookup(ds, en_idx, false, "\"Heretics,\"", &used, matched, sizeof(matched), def, sizeof(def)));
    CHECK(strcmp(matched, "heretic") == 0);
    CHECK(dictset_lookup(ds, en_idx, false, "sorceries", &used, matched, sizeof(matched), def, sizeof(def)));
    CHECK(strcmp(matched, "sorcery") == 0);
    CHECK(dictset_lookup(ds, en_idx, false, "marched", &used, matched, sizeof(matched), def, sizeof(def)));
    CHECK(strcmp(matched, "march") == 0);
    CHECK(dictset_lookup(ds, en_idx, false, "shrugging", &used, matched, sizeof(matched), def, sizeof(def)));
    CHECK(strcmp(matched, "shrug") == 0);
    CHECK(dictset_lookup(ds, en_idx, false, "wolves", &used, matched, sizeof(matched), def, sizeof(def)));
    CHECK(strcmp(matched, "wolf") == 0);
    CHECK(dictset_lookup(ds, en_idx, false, "groaned", &used, matched, sizeof(matched), def, sizeof(def)));
    CHECK(strcmp(matched, "groan") == 0);

    /* English -> German lookup */
    CHECK(dictset_lookup(ds, ende_idx, false, "ephemeral", &used, matched, sizeof(matched), def, sizeof(def)));
    CHECK(used == ende_idx && def[0] != '\0');
    CHECK(dictset_lookup(ds, ende_idx, false, "shadow", &used, matched, sizeof(matched), def, sizeof(def)));
    CHECK(strstr(def, "Schatten") != NULL);

    /* German -> English lookup: UTF-8 umlauts, ß <-> ss, capitalization, lemmatization! */
    CHECK(dictset_lookup(ds, deen_idx, false, "Überraschung", &used, matched, sizeof(matched), def, sizeof(def)));
    CHECK(used == deen_idx && strstr(def, "surprise") != NULL);
    CHECK(dictset_lookup(ds, deen_idx, false, "Straße", &used, matched, sizeof(matched), def, sizeof(def)));
    CHECK(strstr(def, "street") != NULL || strstr(def, "road") != NULL);
    CHECK(dictset_lookup(ds, deen_idx, false, "Strasse", &used, matched, sizeof(matched), def, sizeof(def)));
    CHECK(strstr(def, "street") != NULL || strstr(def, "road") != NULL);
    CHECK(dictset_lookup(ds, deen_idx, false, "Häuser", &used, matched, sizeof(matched), def, sizeof(def)));
    CHECK(strstr(def, "house") != NULL || strstr(def, "houses") != NULL);
    CHECK(dictset_lookup(ds, deen_idx, false, "kämpfte", &used, matched, sizeof(matched), def, sizeof(def)));
    CHECK(strstr(def, "battled") != NULL || strstr(def, "fight") != NULL);
    CHECK(dictset_lookup(ds, deen_idx, false, "kämpftest", &used, matched, sizeof(matched), def, sizeof(def)));
    CHECK(strcmp(matched, "kämpfen") == 0 && strstr(def, "fight") != NULL);

    /* Auto-fallback: German word looked up while preferred dict is EN falls back to DE->EN! */
    CHECK(dictset_lookup(ds, en_idx, true, "Überraschung", &used, matched, sizeof(matched), def, sizeof(def)));
    CHECK(used == deen_idx && strstr(def, "surprise") != NULL);

    /* 2. Test real TXT, EPUB, and MOBI book opening from user's Kobo/backup if present */
    const char *sample_txt = "/tmp/test_kd_book.txt";
    FILE *fp = fopen(sample_txt, "wb");
    CHECK(fp != NULL);
    fputs("The Citadel of Shadows\n\n"
          "High above the valley stood an ancient citadel of dark basalt. "
          "Heretics and sorceries had long plagued the eastern marches, yet the "
          "inquisitor marched quietly through the ephemeral mist beneath the stars.\n\n"
          "He paused beside the iron gates, listening to the distant howl of wolves "
          "echoing across the frozen tundra while checking his chronometer.\n", fp);
    fclose(fp);

    Book *bt = book_open(sample_txt);
    CHECK(bt != NULL);
    CHECK(book_format(bt) == BOOK_FMT_TXT);
    CHECK(book_chapter_count(bt) == 1);
    size_t clen = 0;
    char *ct = book_load_chapter(bt, 0, &clen);
    CHECK(ct != NULL && clen > 100 && strstr(ct, "citadel") != NULL);
    free(ct);
    book_close(bt);

    /* Optional real EPUB/MOBI tests via environment variables (KOBO_TEST_EPUB / KOBO_TEST_MOBI) */
    const char *real_epub = getenv("KOBO_TEST_EPUB");
    if (real_epub && access(real_epub, R_OK) == 0) {
        Book *be = book_open(real_epub);
        CHECK(be != NULL);
        CHECK(book_format(be) == BOOK_FMT_EPUB);
        CHECK(book_chapter_count(be) >= 1);
        size_t elen = 0;
        char *ec = book_load_chapter(be, 0, &elen);
        CHECK(ec != NULL && elen > 10);
        free(ec);
        book_close(be);
    }

    const char *real_mobi = getenv("KOBO_TEST_MOBI");
    if (real_mobi && access(real_mobi, R_OK) == 0) {
        Book *bm = book_open(real_mobi);
        CHECK(bm != NULL);
        CHECK(book_format(bm) == BOOK_FMT_MOBI);
        CHECK(book_chapter_count(bm) >= 1);
        size_t mlen = 0;
        char *mc = book_load_chapter(bm, 0, &mlen);
        CHECK(mc != NULL && mlen > 10);
        free(mc);
        book_close(bm);
    }

    /* 3. Test Reader pagination, inline popup language switching, and progress persistence */
    Config cfg;
    config_set_defaults(&cfg, "/tmp/test_kd_reader");
    mkdir("/tmp/test_kd_reader", 0755);
    CHECK(font_init(&cfg) == 0);
    ReaderState *rs = reader_create(&cfg, ds);
    CHECK(rs != NULL);
    CHECK(reader_open_book(rs, sample_txt, &cfg));
    CHECK(reader_has_book(rs));

    static Canvas c;
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_hour = 22; tmv.tm_min = 35;
    display_init(&cfg);
    reader_render_page(&c, rs, &cfg, NULL, &tmv, KD_REFRESH_FULL_FLASH);
    reader_debug_lookup_word(rs, "citadel");
    reader_render_page(&c, rs, &cfg, NULL, &tmv, KD_REFRESH_GRAY_PARTIAL);

    /* Tap language pill inside popup -> switches from EN to EN>DE! */
    Library dummy_lib;
    memset(&dummy_lib, 0, sizeof(dummy_lib));
    bool dirty = false, ex = false;
    ScreenId ns = SCREEN_READER;
    KdRefreshMode rm = KD_REFRESH_FAST_DU;
    InputEvent tap_lang = { INPUT_TAP, 430, 520 };
    CHECK(reader_handle_book_input(rs, &dummy_lib, &tap_lang, &cfg, &dirty, &ns, &ex, &rm));
    CHECK(dirty && strcmp(cfg.dict_lang, "en-de") == 0);

    int saved_ch = -1, saved_pct = -1;
    size_t saved_off = 0;
    CHECK(library_get_progress(&cfg, sample_txt, &saved_ch, &saved_off, &saved_pct));
    CHECK(saved_ch == 0 && saved_pct >= 0);

    /* Verify all 7 book font families load cleanly without errors */
    const char *fams[] = { "georgia", "charter", "palatino", "baskerville", "times", "verdana", "trebuchet" };
    for (size_t fi = 0; fi < sizeof(fams) / sizeof(fams[0]); fi++) {
        CHECK(font_set_book_family(&cfg, fams[fi]) == 0);
        CHECK(font_measure_utf8(FONT_STYLE_SERIF_REG, 24, "Citadel", 7) > 30);
    }
    CHECK(font_set_book_family(&cfg, "georgia") == 0);

    /* Verify page turns use crisp WFM_GC16 grayscale partial (never 1-bit WFM_DU!) and Flash Every Page */
    cfg.reader_flash_every = 1;
    InputEvent tap_next = { INPUT_TAP, 520, 400 };
    CHECK(reader_handle_book_input(rs, &dummy_lib, &tap_next, &cfg, &dirty, &ns, &ex, &rm));
    CHECK(rm == KD_REFRESH_FULL_FLASH || rm == KD_REFRESH_GRAY_PARTIAL);
    CHECK(rm != KD_REFRESH_FAST_DU);

    reader_close_book(rs, &cfg);
    reader_destroy(rs);
    display_close();
    font_close();
    dictset_close(ds);
    unlink(sample_txt);
    unlink("/tmp/test_kd_reader/progress.db");
    rmdir("/tmp/test_kd_reader");
}

static void test_wifi_and_keyboard(void) {
    const char *scan =
        "bssid / frequency / signal level / flags / ssid\n"
        "aa:bb:cc:dd:ee:01\t2412\t-68\t[WPA2-PSK-CCMP][ESS]\tHomeWiFi\n"
        "aa:bb:cc:dd:ee:02\t2437\t-48\t[WPA2-PSK-CCMP][ESS]\tHomeWiFi\n"
        "aa:bb:cc:dd:ee:03\t2462\t-55\t[ESS]\tCafeGuest\n"
        "aa:bb:cc:dd:ee:04\t2412\t-72\t[WPA2-PSK-CCMP][ESS]\t\\x00\\x00\n"
        "aa:bb:cc:dd:ee:05\t2412\t-60\t[WPA2-PSK-CCMP][ESS]\tiPhoneHotspot\n";
    const char *saved = "HomeWiFi\nOldSavedNet\n";

    WifiState ws;
    memset(&ws, 0, sizeof(ws));
    int n = wifi_parse_scan_results(scan, saved, "HomeWiFi", &ws);
    CHECK(n == 4);
    CHECK(strcmp(ws.nets[0].ssid, "HomeWiFi") == 0);
    CHECK(ws.nets[0].connected && ws.nets[0].saved && ws.nets[0].rssi == -48 && ws.nets[0].secured);
    CHECK(strcmp(ws.nets[1].ssid, "CafeGuest") == 0 && !ws.nets[1].secured);
    CHECK(strcmp(ws.nets[2].ssid, "iPhoneHotspot") == 0 && ws.nets[2].secured);
    CHECK(strcmp(ws.nets[3].ssid, "OldSavedNet") == 0 && ws.nets[3].saved && ws.nets[3].rssi == -99);

    /* Keyboard typing, URL presets, shift, symbols, backspace, submit */
    KeyboardState kb;
    kbd_open(&kb, KBD_SERVER_URL, "Dashboard Server URL", "http://...", "", 128, false);
    CHECK(kb.active && kb.buf[0] == '\0');
    /* Tap preset 0 ("https://") at x=60, y=224 */
    InputEvent ev = { INPUT_TAP, 60, 224 };
    CHECK(kbd_handle_input(&kb, &ev) == KBD_RESULT_REDRAW_INPUT);
    CHECK(strcmp(kb.buf, "https://") == 0);
    /* Tap preset 2 ("/kobo/bundle") at x=340, y=224 */
    ev = (InputEvent){ INPUT_TAP, 340, 224 };
    CHECK(kbd_handle_input(&kb, &ev) == KBD_RESULT_REDRAW_INPUT);
    CHECK(strcmp(kb.buf, "https:///kobo/bundle") == 0);
    /* Tap 123 key at x=50, y=590 -> switches to symbols */
    ev = (InputEvent){ INPUT_TAP, 50, 590 };
    CHECK(kbd_handle_input(&kb, &ev) == KBD_RESULT_REDRAW_ALL);
    CHECK(kb.symbols);
    /* Tap '1' (row 0 col 0: x=35, y=370) */
    ev = (InputEvent){ INPUT_TAP, 35, 370 };
    CHECK(kbd_handle_input(&kb, &ev) == KBD_RESULT_REDRAW_INPUT);
    CHECK(strcmp(kb.buf, "https:///kobo/bundle1") == 0);
    /* Tap DEL in symbol mode (x=540, y=520) */
    ev = (InputEvent){ INPUT_TAP, 540, 520 };
    CHECK(kbd_handle_input(&kb, &ev) == KBD_RESULT_REDRAW_INPUT);
    CHECK(strcmp(kb.buf, "https:///kobo/bundle") == 0);
    /* Tap Save button (x=440, y=700) */
    ev = (InputEvent){ INPUT_TAP, 440, 700 };
    CHECK(kbd_handle_input(&kb, &ev) == KBD_RESULT_SUBMIT);
    CHECK(!kb.active);

    /* Config helpers */
    CHECK(strcmp(config_next_font_family("georgia"), "charter") == 0);
    CHECK(strcmp(config_next_font_family("trebuchet"), "georgia") == 0);
    CHECK(config_next_reader_flash(1) == 3);
    CHECK(config_next_reader_flash(20) == 1);
    char tz[64];
    config_next_timezone("CET-1CEST,M3.5.0,M10.5.0/3", tz, sizeof(tz));
    CHECK(strstr(tz, "EET") != NULL);
}

static void test_book_decoding(void) {
    /* 1. CP1252 .txt with >8KB leading ASCII then CP1252 apostrophe/quotes/ellipsis/dash/NBSP */
    const char *cp_path = "/tmp/test_kd_cp1252.txt";
    FILE *fp = fopen(cp_path, "wb");
    CHECK(fp != NULL);
    for (int i = 0; i < 8200; i++) fputc((i % 60 == 59) ? '\n' : 'a', fp);
    fputs("\n\nIt\x92s a \x93test\x94\x85\x97with\xA0NBSP.\n", fp);
    fclose(fp);

    Book *bc = book_open(cp_path);
    CHECK(bc != NULL);
    size_t clen = 0;
    char *ct = book_load_chapter(bc, 0, &clen);
    CHECK(ct != NULL);
    CHECK(strstr(ct, "It\xE2\x80\x99s a \xE2\x80\x9Ctest\xE2\x80\x9D\xE2\x80\xA6\xE2\x80\x94with NBSP.") != NULL);
    free(ct);
    book_close(bc);
    unlink(cp_path);

    /* 2. UTF-8 .txt with 3-byte apostrophe straddling byte 8191..8193 (previously caused mojibake!) */
    const char *u8_path = "/tmp/test_kd_utf8_8k.txt";
    fp = fopen(u8_path, "wb");
    CHECK(fp != NULL);
    for (int i = 0; i < 8191; i++) fputc((i % 60 == 59) ? '\n' : 'x', fp);
    fputs("\xE2\x80\x99s town\xE2\x80\x99s gate.\n", fp);
    fclose(fp);

    Book *bu = book_open(u8_path);
    CHECK(bu != NULL);
    char *ut = book_load_chapter(bu, 0, &clen);
    CHECK(ut != NULL);
    CHECK(strstr(ut, "town\xE2\x80\x99s gate.") != NULL);
    CHECK(strstr(ut, "\xC3\xA2\xE2\x82\xAC") == NULL); /* zero mojibake! */
    free(ut);
    book_close(bu);
    unlink(u8_path);
}

static void dump_previews(const char *dir) {
    Config cfg;
    config_set_defaults(&cfg, "/tmp");
    snprintf(cfg.server_url, sizeof(cfg.server_url), "https://dashboard.example.com/kobo/bundle");

    Telemetry tel;
    memset(&tel, 0, sizeof(tel));
    tel.bat_pct = 74;
    tel.bat_mv = 3920;
    tel.bat_ma = 145;
    snprintf(tel.bat_status, sizeof(tel.bat_status), "Discharging");
    tel.wifi_up = 1;
    snprintf(tel.wifi_ip, sizeof(tel.wifi_ip), "192.0.2.20");
    snprintf(tel.wifi_ssid, sizeof(tel.wifi_ssid), "HomeWiFi");
    tel.wifi_rssi = -54;
    tel.mem_total_kb = 256124;
    tel.mem_avail_kb = 236000;
    tel.self_rss_kb = 2150;
    tel.load1 = 0.04; tel.load5 = 0.02; tel.load15 = 0.01;
    tel.uptime_sec = 9420;
    tel.cpu_khz = 800000;
    tel.onboard_total_mb = 1404;
    tel.onboard_free_mb = 412;
    tel.sd_mounted = 1;
    tel.sd_total_mb = 15200;
    tel.sd_free_mb = 14800;

    SyncState sync;
    memset(&sync, 0, sizeof(sync));
    sync.last_ok = 1;
    sync.time_synced = true;
    snprintf(sync.last_status, sizeof(sync.last_status), "Image updated");

    Gallery gal;
    memset(&gal, 0, sizeof(gal));
    gal.count = 3;

    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = 126; tmv.tm_mon = 10; tmv.tm_mday = 21;
    tmv.tm_wday = 6; tmv.tm_hour = 21; tmv.tm_min = 48;
    sync.last_sync_time = mktime(&tmv) - 600;
    sync.next_sync_time = mktime(&tmv) + 1200;

    static Canvas c;
    display_init(&cfg);
    font_init(&cfg);

    static WifiState mock_ws;
    memset(&mock_ws, 0, sizeof(mock_ws));
    wifi_parse_scan_results(
        "bssid / freq / signal / flags / ssid\n"
        "aa:bb:cc:dd:ee:01\t2412\t-52\t[WPA2-PSK-CCMP][ESS]\tHomeWiFi\n"
        "aa:bb:cc:dd:ee:02\t2437\t-61\t[WPA2-PSK-CCMP][ESS]\tiPhone Hotspot\n"
        "aa:bb:cc:dd:ee:03\t2462\t-70\t[ESS]\tGuestWiFi\n",
        "HomeWiFi\n", "HomeWiFi", &mock_ws);
    snprintf(mock_ws.status_msg, sizeof(mock_ws.status_msg), "Found 3 networks");
    ui_set_wifi_state(&mock_ws);

    const struct { ScreenId id; const char *name; } screens[] = {
        { SCREEN_CLOCK, "clock" },
        { SCREEN_STATUS, "status" },
        { SCREEN_DASHBOARD, "dashboard" },
        { SCREEN_SETTINGS, "settings" }
    };

    ui_set_settings_tab(SET_TAB_GENERAL);
    for (size_t i = 0; i < sizeof(screens) / sizeof(screens[0]); i++) {
        char pgm[320], txt[320];
        snprintf(pgm, sizeof(pgm), "%s/%s.pgm", dir, screens[i].name);
        snprintf(txt, sizeof(txt), "%s/%s.txt", dir, screens[i].name);
        display_host_begin_capture(txt);
        ui_render_screen(&c, screens[i].id, &cfg, &tel, &sync, &gal, &tmv, KD_REFRESH_FULL_FLASH);
        display_host_save_pgm(pgm);
    }

    /* Preview the other 3 Settings tabs + On-Screen Keyboard */
    const struct { SettingsTab tab; const char *name; } set_tabs[] = {
        { SET_TAB_WIFI, "settings_wifi" },
        { SET_TAB_SERVER, "settings_server" },
        { SET_TAB_DEVICE, "settings_device" }
    };
    for (size_t i = 0; i < sizeof(set_tabs) / sizeof(set_tabs[0]); i++) {
        char pgm[320], txt[320];
        ui_set_settings_tab(set_tabs[i].tab);
        snprintf(pgm, sizeof(pgm), "%s/%s.pgm", dir, set_tabs[i].name);
        snprintf(txt, sizeof(txt), "%s/%s.txt", dir, set_tabs[i].name);
        display_host_begin_capture(txt);
        ui_render_screen(&c, SCREEN_SETTINGS, &cfg, &tel, &sync, &gal, &tmv, KD_REFRESH_FULL_FLASH);
        display_host_save_pgm(pgm);
    }
    ui_set_settings_tab(SET_TAB_GENERAL);

    {
        KeyboardState kb;
        kbd_open(&kb, KBD_SERVER_URL, "Dashboard Server URL",
                 "https:// or http:// (JSON bundle or PNG/JPG)",
                 "https://dashboard.example.com/kobo/bundle", 200, false);
        char pgm[320], txt[320];
        snprintf(pgm, sizeof(pgm), "%s/keyboard.pgm", dir);
        snprintf(txt, sizeof(txt), "%s/keyboard.txt", dir);
        display_host_begin_capture(txt);
        kbd_render(&c, &kb, &cfg, false, KD_REFRESH_FULL_FLASH);
        display_host_save_pgm(pgm);
    }

    /* Also preview Dark theme clock */
    cfg.invert_mode = 1;
    char pgm[320], txt[320];
    snprintf(pgm, sizeof(pgm), "%s/clock_dark.pgm", dir);
    snprintf(txt, sizeof(txt), "%s/clock_dark.txt", dir);
    display_host_begin_capture(txt);
    ui_render_screen(&c, SCREEN_CLOCK, &cfg, &tel, &sync, &gal, &tmv, KD_REFRESH_FULL_FLASH);
    display_host_save_pgm(pgm);

    /* Preview Library, Book Reader page, Dictionary lookup popup (EN & EN>DE), and Reader Toolbar! */
    cfg.invert_mode = 0;
    font_init(&cfg);
    DictSet *ds = dictset_open_dir("../output");

    const char *demo_book = "/tmp/kd_preview_book.txt";
    FILE *bfp = fopen(demo_book, "wb");
    if (bfp) {
        fputs("\x05" "Chapter One: The Iron Citadel\n\n"
              "High above the ash wastes stood an ancient citadel carved from black basalt. "
              "Its towering battlements caught the faint silver gleam of a dying moon while "
              "distant thunder rolled across the plains.\n\n"
              "For three centuries the inquisitor had guarded the archives against heretics "
              "and forbidden sorceries. Now, as ephemeral embers drifted through the cold "
              "night air, the iron gates groaned open once more.\n\n"
              "\"Every word in this chronicle,\" he murmured quietly, \"must survive the "
              "coming pestilence.\" Beneath his boots, frost crackled like shattered glass, "
              "and somewhere beyond the outer ridge a pack of wolves answered the wind.\n\n"
              "He turned the brass lantern higher and stepped into the silent nave, where "
              "rows of weathered tomes waited in unbroken stillness.\n", bfp);
        fclose(bfp);
    }

    Library lib;
    library_init(&lib, "/tmp", &cfg);
    lib.has_recent = true;
    snprintf(lib.recent_title, sizeof(lib.recent_title), "The Citadel of Shadows");
    snprintf(lib.recent_path, sizeof(lib.recent_path), "%s", demo_book);
    lib.recent_fmt = BOOK_FMT_TXT;
    lib.recent_pct = 38;

    snprintf(pgm, sizeof(pgm), "%s/library.pgm", dir);
    snprintf(txt, sizeof(txt), "%s/library.txt", dir);
    display_host_begin_capture(txt);
    reader_render_library(&c, &lib, &cfg, &tel, &sync, &gal, KD_REFRESH_FULL_FLASH);
    display_host_save_pgm(pgm);

    ReaderState *rs = reader_create(&cfg, ds);
    if (rs && reader_open_book(rs, demo_book, &cfg)) {
        snprintf(pgm, sizeof(pgm), "%s/reader.pgm", dir);
        snprintf(txt, sizeof(txt), "%s/reader.txt", dir);
        display_host_begin_capture(txt);
        reader_render_page(&c, rs, &cfg, &tel, &tmv, KD_REFRESH_FULL_FLASH);
        display_host_save_pgm(pgm);

        reader_debug_lookup_word(rs, "ephemeral");
        snprintf(pgm, sizeof(pgm), "%s/reader_dict.pgm", dir);
        snprintf(txt, sizeof(txt), "%s/reader_dict.txt", dir);
        display_host_begin_capture(txt);
        reader_render_page(&c, rs, &cfg, &tel, &tmv, KD_REFRESH_GRAY_PARTIAL);
        display_host_save_pgm(pgm);

        /* Tap language badge inside popup -> switches immediately to EN>DE! */
        bool dirty = false, ex = false;
        ScreenId ns = SCREEN_READER;
        KdRefreshMode rm = KD_REFRESH_FAST_DU;
        InputEvent tap_lang = { INPUT_TAP, 430, 520 };
        reader_handle_book_input(rs, &lib, &tap_lang, &cfg, &dirty, &ns, &ex, &rm);
        snprintf(pgm, sizeof(pgm), "%s/reader_dict_de.pgm", dir);
        snprintf(txt, sizeof(txt), "%s/reader_dict_de.txt", dir);
        display_host_begin_capture(txt);
        reader_render_page(&c, rs, &cfg, &tel, &tmv, KD_REFRESH_FAST_DU);
        display_host_save_pgm(pgm);

        /* Dismiss dict popup and open toolbar */
        InputEvent tap_close = { INPUT_TAP, 300, 200 };
        reader_handle_book_input(rs, &lib, &tap_close, &cfg, &dirty, &ns, &ex, &rm);
        InputEvent tap_top = { INPUT_TAP, 300, 20 };
        reader_handle_book_input(rs, &lib, &tap_top, &cfg, &dirty, &ns, &ex, &rm);
        snprintf(pgm, sizeof(pgm), "%s/reader_toolbar.pgm", dir);
        snprintf(txt, sizeof(txt), "%s/reader_toolbar.txt", dir);
        display_host_begin_capture(txt);
        reader_render_page(&c, rs, &cfg, &tel, &tmv, KD_REFRESH_GRAY_PARTIAL);
        display_host_save_pgm(pgm);

        reader_close_book(rs, &cfg);
    }
    /* Preview multi-page English Dashboard (4 pages) + Attention Alert Overlay + Sleep Screens */
    {
        const char *demo_bundle =
            "{\"version\":1,\"generated_ts\":1795342000,\"updated\":\"10:55\","
            "\"pages\":["
              "{\"id\":\"today\",\"kind\":\"overview\",\"title\":\"TODAY & PROGRESS\","
               "\"subtitle\":\"Metro City  •  Sun 22 Nov 2026\",\"hero\":\"4°C\","
               "\"hero_sub\":\"Feels 0°C  •  Overcast  •  Wind 11 km/h\","
               "\"bars\":[{\"label\":\"DAY\",\"pct\":46,\"detail\":\"46%\"},"
               "{\"label\":\"WEEK\",\"pct\":92,\"detail\":\"Day 7/7\"},"
               "{\"label\":\"MONTH\",\"pct\":72,\"detail\":\"22/30\"},"
               "{\"label\":\"YEAR\",\"pct\":89,\"detail\":\"Day 326/365\"}],"
               "\"items\":["
                 "{\"label\":\"Daylight\",\"value\":\"07:43 - 16:04\",\"detail\":\"8h 21m  •  Golden 15:18\",\"urgent\":false},"
                 "{\"label\":\"Mon\",\"value\":\"0° / 4°C\",\"detail\":\"Overcast  •  28% rain\",\"urgent\":false},"
                 "{\"label\":\"Tue\",\"value\":\"0° / 2°C\",\"detail\":\"Light Snow\",\"urgent\":false},"
                 "{\"label\":\"Wed\",\"value\":\"-1° / 1°C\",\"detail\":\"Partly Cloudy\",\"urgent\":false},"
                 "{\"label\":\"Transit\",\"value\":\"2 major, 1 warning\",\"detail\":\"Local Metro & Rail\",\"urgent\":true}"
               "],\"footer\":\"Updated 10:55\"},"
              "{\"id\":\"calendar\",\"kind\":\"calendar\",\"title\":\"CALENDAR & AGENDA\","
               "\"subtitle\":\"November 2026  •  Week 47\","
               "\"cal_year\":2026,\"cal_month\":11,\"cal_today\":22,\"cal_marks\":[22,24,27,29],"
               "\"items\":["
                 "{\"label\":\"Today 14:00\",\"value\":\"Dentist appointment\",\"detail\":\"Personal\",\"urgent\":true},"
                 "{\"label\":\"Tomorrow 10:00\",\"value\":\"Sprint review call\",\"detail\":\"Work\",\"urgent\":true},"
                 "{\"label\":\"Tue 24 Nov\",\"value\":\"Flight check-in opens\",\"detail\":\"Travel\",\"urgent\":false},"
                 "{\"label\":\"Fri 27 Nov\",\"value\":\"Monthly budget review\",\"detail\":\"Finance\",\"urgent\":false}"
               "],\"footer\":\"4 upcoming events cached for offline alerts\"},"
              "{\"id\":\"waiting\",\"kind\":\"list\",\"title\":\"WAITING ON YOU\","
               "\"subtitle\":\"2 open items  •  Updated 10:55\",\"badge\":\"2 OPEN\","
               "\"items\":["
                 "{\"label\":\"TODAY\",\"value\":\"Approve v1.2.0 release notes\",\"detail\":\"From CI Bot  •  Release notes and checksums ready to publish.\",\"urgent\":true},"
                 "{\"label\":\"THIS WEEK\",\"value\":\"Confirm December rail ticket\",\"detail\":\"From Planner  •  Flexible ticket hold expires Wednesday at 18:00.\",\"urgent\":false}"
               "],\"footer\":\"3 services healthy  •  Self-hosted dashboard\"},"
              "{\"id\":\"stats\",\"kind\":\"stats\",\"title\":\"STATS & SPORTS\","
               "\"subtitle\":\"Last 30 days  •  Updated 10:55\",\"metrics\":["
                 "{\"label\":\"WEB VISITS (30D)\",\"value\":\"350\",\"detail\":\"7d: 28  •  Today: 0\"},"
                 "{\"label\":\"APP DOWNLOADS\",\"value\":\"14\",\"detail\":\"Redl: 6  •  Upd: 16\"},"
                 "{\"label\":\"RELEASES (30D)\",\"value\":\"105\",\"detail\":\"App A 29  •  App B 76\"},"
                 "{\"label\":\"OPEN TASKS\",\"value\":\"12 done\",\"detail\":\"7d: 3  •  Waiting: 1\"}"
               "],"
               "\"items\":["
                 "{\"label\":\"Football\",\"value\":\"Home FC vs Away FC  •  Today 14:00\",\"detail\":\"League  •  Rank #4 (22 pts)\",\"urgent\":true},"
                 "{\"label\":\"Next F1\",\"value\":\"Las Vegas Grand Prix\",\"detail\":\"Round 21  •  Today 07:00\",\"urgent\":false},"
                 "{\"label\":\"Reading\",\"value\":\"18 / 24 books read\",\"detail\":\"2026 Annual Goal\",\"urgent\":false}"
               "],\"footer\":\"Synced 10:55  •  Cached locally\"}"
            "],"
            "\"events\":[],\"alerts\":[]}";
        DashboardBundle db;
        if (dashboard_parse_bundle(demo_bundle, &db)) {
            static const char *pnames[4] = { "dash_today", "dash_calendar", "dash_waiting", "dash_stats" };
            sync.push_connected = true;
            for (int pi = 0; pi < db.page_count && pi < 4; pi++) {
                ui_set_dashboard_bundle(&db, pi);
                snprintf(pgm, sizeof(pgm), "%s/%s.pgm", dir, pnames[pi]);
                snprintf(txt, sizeof(txt), "%s/%s.txt", dir, pnames[pi]);
                display_host_begin_capture(txt);
                dashboard_render_page(&c, &db, pi, &cfg, &tel, &sync, &gal, &tmv, KD_REFRESH_FULL_FLASH);
                display_host_save_pgm(pgm);
            }
            ActiveAlert demo_al;
            memset(&demo_al, 0, sizeof(demo_al));
            demo_al.active = true;
            demo_al.is_remote = true;
            demo_al.urgent = true;
            snprintf(demo_al.badge, sizeof(demo_al.badge), "TODAY");
            snprintf(demo_al.title, sizeof(demo_al.title), "Approve v1.2.0 release notes");
            snprintf(demo_al.body, sizeof(demo_al.body),
                     "Release notes and binary checksums are ready. Confirm to publish or snooze until later.");
            snprintf(demo_al.meta, sizeof(demo_al.meta), "From CI Bot  •  dashboard.example.com");
            snprintf(pgm, sizeof(pgm), "%s/alert_overlay.pgm", dir);
            snprintf(txt, sizeof(txt), "%s/alert_overlay.txt", dir);
            display_host_begin_capture(txt);
            dashboard_render_alert_overlay(&c, &demo_al, &cfg, KD_REFRESH_FULL_FLASH);
            display_host_save_pgm(pgm);

            /* Preview True Deep Sleep screen (Calendar & Agenda) + Touch-Lock screen (Stats & Sports) */
            snprintf(pgm, sizeof(pgm), "%s/sleep_deep_calendar.pgm", dir);
            snprintf(txt, sizeof(txt), "%s/sleep_deep_calendar.txt", dir);
            display_host_begin_capture(txt);
            dashboard_render_sleep_screen(&c, &db, SLEEP_STYLE_CYCLE, 1, LOCK_DEEP_SLEEP,
                                          &cfg, &tel, &sync, &tmv, KD_REFRESH_FULL_FLASH);
            display_host_save_pgm(pgm);

            snprintf(pgm, sizeof(pgm), "%s/sleep_touchlock_stats.pgm", dir);
            snprintf(txt, sizeof(txt), "%s/sleep_touchlock_stats.txt", dir);
            display_host_begin_capture(txt);
            dashboard_render_sleep_screen(&c, &db, SLEEP_STYLE_CYCLE, 2, LOCK_TOUCH,
                                          &cfg, &tel, &sync, &tmv, KD_REFRESH_FULL_FLASH);
            display_host_save_pgm(pgm);

            ui_set_dashboard_bundle(NULL, 0);
        }
    }

    reader_destroy(rs);
    dictset_close(ds);
    font_close();
    unlink(demo_book);

    display_close();
}

static void test_dashboard_bundle_and_triggers(void) {
    /* 1. Parse full 4-page English bundle + events + remote alerts */
    const char *sample =
        "{"
        "\"version\":1,\"generated_ts\":1795342000,\"updated\":\"10:55\","
        "\"pages\":["
          "{\"id\":\"today\",\"kind\":\"overview\",\"title\":\"TODAY & PROGRESS\","
           "\"subtitle\":\"Metro City  •  Sun 22 Nov 2026\",\"hero\":\"4°C\","
           "\"hero_sub\":\"Feels 0°C  •  Overcast  •  Wind 11 km/h\","
           "\"bars\":["
             "{\"label\":\"Day\",\"pct\":46,\"detail\":\"46%\"},"
             "{\"label\":\"Week\",\"pct\":92,\"detail\":\"Day 7/7\"},"
             "{\"label\":\"Month\",\"pct\":72,\"detail\":\"22/30\"},"
             "{\"label\":\"Year\",\"pct\":89,\"detail\":\"Day 326/365\"}"
           "],"
           "\"items\":["
             "{\"label\":\"Daylight\",\"value\":\"07:43 - 16:04\",\"detail\":\"8h 21m  •  Golden 15:18\",\"urgent\":false},"
             "{\"label\":\"Transit\",\"value\":\"2 major, 1 warning\",\"detail\":\"Local Metro & Rail\",\"urgent\":true}"
           "],"
           "\"footer\":\"Updated 10:55\"},"
          "{\"id\":\"calendar\",\"kind\":\"calendar\",\"title\":\"CALENDAR & AGENDA\","
           "\"subtitle\":\"November 2026  •  Week 47\","
           "\"cal_year\":2026,\"cal_month\":11,\"cal_today\":22,\"cal_marks\":[22,24,27],"
           "\"items\":["
             "{\"label\":\"Today 14:00\",\"value\":\"Dentist appointment\",\"detail\":\"Personal\",\"urgent\":true},"
             "{\"label\":\"Tue 24 Nov\",\"value\":\"Flight check-in\",\"detail\":\"Travel\",\"urgent\":false}"
           "],"
           "\"footer\":\"2 upcoming events cached\"},"
          "{\"id\":\"waiting\",\"kind\":\"list\",\"title\":\"WAITING ON YOU\","
           "\"subtitle\":\"1 open item  •  Updated 10:55\",\"badge\":\"1 OPEN\","
           "\"items\":["
             "{\"label\":\"TODAY\",\"value\":\"Approve release notes\",\"detail\":\"From CI Bot  •  v1.2.0 ready to ship\",\"urgent\":true}"
           "],"
           "\"footer\":\"3 services healthy\"},"
          "{\"id\":\"stats\",\"kind\":\"stats\",\"title\":\"STATS & SPORTS\","
           "\"subtitle\":\"Last 30 days\",\"metrics\":["
             "{\"label\":\"WEB VISITS (30D)\",\"value\":\"350\",\"detail\":\"7d: 28  •  Today: 0\"},"
             "{\"label\":\"APP DOWNLOADS\",\"value\":\"14\",\"detail\":\"Redl: 6  •  Upd: 16\"},"
             "{\"label\":\"RELEASES (30D)\",\"value\":\"105\",\"detail\":\"App A 29  •  App B 76\"},"
             "{\"label\":\"OPEN TASKS\",\"value\":\"12 done\",\"detail\":\"7d: 3  •  Waiting: 1\"}"
           "],"
           "\"items\":["
             "{\"label\":\"Football\",\"value\":\"Home FC vs Away FC  •  Today 14:00\",\"detail\":\"Rank #4 (22 pts)\",\"urgent\":true},"
             "{\"label\":\"Next F1\",\"value\":\"Las Vegas Grand Prix\",\"detail\":\"Round 21  •  Today 07:00\",\"urgent\":false}"
           "]}"
        "],"
        "\"events\":["
          "{\"id\":\"ev1\",\"start_ts\":1795352400,\"all_day\":false,\"remind_min\":15,"
           "\"title\":\"Dentist appointment\",\"tag\":\"Personal\",\"when_label\":\"Today 14:00\"}"
        "],"
        "\"alerts\":["
          "{\"id\":\"extra:rel91\",\"title\":\"Approve release notes\","
           "\"body\":\"v1.2.0 ready to ship\",\"source\":\"CI Bot\","
           "\"due\":\"TODAY\",\"urgent\":true,\"ts\":1795340000}"
        "]"
        "}";

    DashboardBundle b;
    CHECK(dashboard_parse_bundle(sample, &b));
    CHECK(b.has_bundle);
    CHECK(b.page_count == 4);
    CHECK(b.pages[0].kind == DASH_PAGE_OVERVIEW);
    CHECK(b.pages[1].kind == DASH_PAGE_CALENDAR);
    CHECK((b.pages[1].cal_marks_mask & (1u << 21)) != 0); /* day 22 */
    CHECK((b.pages[1].cal_marks_mask & (1u << 23)) != 0); /* day 24 */
    CHECK(b.pages[2].kind == DASH_PAGE_LIST);
    CHECK(b.pages[3].kind == DASH_PAGE_STATS);
    CHECK(b.event_count == 1);
    CHECK(b.alert_count == 1);
    CHECK(strcmp(b.alerts[0].id, "extra:rel91") == 0);

    /* 2. Multi-page carousel rotation (including auto-rotate skipping Library) */
    Gallery empty_gal;
    memset(&empty_gal, 0, sizeof(empty_gal));
    ScreenId scr = SCREEN_CLOCK;
    int dp = 0;
    ui_step_carousel(scr, dp, +1, &empty_gal, &b, true, &scr, &dp);
    CHECK(scr == SCREEN_DASHBOARD && dp == 0);
    ui_step_carousel(scr, dp, +1, &empty_gal, &b, true, &scr, &dp);
    CHECK(scr == SCREEN_DASHBOARD && dp == 1);
    ui_step_carousel(scr, dp, +1, &empty_gal, &b, true, &scr, &dp);
    CHECK(scr == SCREEN_DASHBOARD && dp == 2);
    ui_step_carousel(scr, dp, +1, &empty_gal, &b, true, &scr, &dp);
    CHECK(scr == SCREEN_DASHBOARD && dp == 3);
    ui_step_carousel(scr, dp, +1, &empty_gal, &b, true, &scr, &dp);
    CHECK(scr == SCREEN_STATUS);
    ui_step_carousel(scr, dp, -1, &empty_gal, &b, false, &scr, &dp);
    CHECK(scr == SCREEN_DASHBOARD && dp == 3);

    /* 3. Remote alert trigger + dismiss + auto-clear + offline calendar trigger */
    AlertTracker tr;
    memset(&tr, 0, sizeof(tr));
    time_t now = 1795350000; /* 40m before Dentist (1795352400) */
    CHECK(dashboard_check_triggers(&b, &tr, now));
    CHECK(tr.current.active && tr.current.is_remote);
    CHECK(strcmp(tr.current.id, "extra:rel91") == 0);

    /* Clock strip should highlight upcoming event / waiting item */
    char strip[96];
    int target_p = -1;
    CHECK(dashboard_clock_strip(&b, now, strip, sizeof(strip), &target_p));
    CHECK(strstr(strip, "Dentist") != NULL);
    CHECK(target_p == 1); /* calendar page */

    /* Snooze remote alert for 30m -> inactive during snooze */
    dashboard_snooze_active(&tr, now, 30, NULL);
    CHECK(!tr.current.active);
    CHECK(!dashboard_check_triggers(&b, &tr, now + 600));

    /* Advance to 11m40s before Dentist (still inside 30m snooze) -> offline calendar trigger fires! */
    time_t trig_now = 1795352400 - 700;
    CHECK(dashboard_check_triggers(&b, &tr, trig_now));
    CHECK(tr.current.active && !tr.current.is_remote);
    CHECK(strstr(tr.current.title, "Dentist") != NULL);

    /* Dismiss offline trigger, then advance past 30m snooze -> remote alert re-fires! */
    dashboard_dismiss_active(&tr, NULL);
    CHECK(!tr.current.active);
    CHECK(dashboard_check_triggers(&b, &tr, now + 1805));
    CHECK(tr.current.active && tr.current.is_remote);

    /* SSE alerts update clears remote alerts -> auto-clears active overlay & waiting page shows ALL CLEAR */
    const char *clear_json = "{\"version\":1,\"updated\":\"11:05\",\"hash\":\"abc\",\"alerts\":[]}";
    CHECK(dashboard_parse_alerts_update(clear_json, &b));
    CHECK(b.alert_count == 0);
    CHECK(strcmp(b.pages[2].badge, "ALL CLEAR") == 0);
    CHECK(dashboard_check_triggers(&b, &tr, now + 1810));
    CHECK(!tr.current.active);

    /* Stream URL derivation */
    char surl[256];
    CHECK(net_derive_stream_url("https://dashboard.example.com/kobo/bundle", surl, sizeof(surl)));
    CHECK(strcmp(surl, "https://dashboard.example.com/kobo/stream") == 0);
    CHECK(!net_derive_stream_url("http://192.0.2.10/dash.png", surl, sizeof(surl)));

    /* Sleep screen 3-page flair rotation: Today (0) -> Calendar (1) -> Stats (3) -> Today (0) */
    CHECK(dashboard_pick_sleep_page(&b, SLEEP_STYLE_CYCLE, 0) == 0);
    CHECK(dashboard_pick_sleep_page(&b, SLEEP_STYLE_CYCLE, 1) == 1);
    CHECK(dashboard_pick_sleep_page(&b, SLEEP_STYLE_CYCLE, 2) == 3);
    CHECK(dashboard_pick_sleep_page(&b, SLEEP_STYLE_CYCLE, 3) == 0);
    CHECK(dashboard_pick_sleep_page(&b, SLEEP_STYLE_CALENDAR, 0) == 1);
    CHECK(dashboard_pick_sleep_page(&b, SLEEP_STYLE_STATS, 0) == 3);
    CHECK(dashboard_pick_sleep_page(&b, SLEEP_STYLE_CLOCK, 0) == -1);

    /* Auto-sleep & sleep-screen config helpers + Settings hits */
    CHECK(config_next_auto_sleep(15) == 30);
    CHECK(config_next_auto_sleep(60) == 0);
    CHECK(config_next_sleep_rotate(3) == 6);
    CHECK(config_next_sleep_rotate(6) == 0);
    CHECK(config_next_sleep_rotate(0) == 1);
    CHECK(strcmp(config_sleep_style_label(SLEEP_STYLE_CYCLE), "Cycle 3p") == 0);

    Config scfg;
    config_set_defaults(&scfg, "/tmp");
    scfg.auto_sleep_min = 30;
    scfg.sleep_style = SLEEP_STYLE_STATS;
    scfg.sleep_rotate_hrs = 6;
    CHECK(config_save(&scfg, "/tmp/test_kd_sleep_cfg.ini") == 0);
    Config scfg2;
    config_set_defaults(&scfg2, "/tmp");
    CHECK(config_load(&scfg2, "/tmp/test_kd_sleep_cfg.ini") == 0);
    unlink("/tmp/test_kd_sleep_cfg.ini");
    CHECK(scfg2.auto_sleep_min == 30 && scfg2.sleep_style == SLEEP_STYLE_STATS && scfg2.sleep_rotate_hrs == 6);

    /* Power slider short -> ACT_LOCK_OR_SLEEP, long -> ACT_POWEROFF */
    InputEvent pev = { INPUT_POWER_SHORT, 0, 0 };
    UiAction pa = ui_handle_input(SCREEN_CLOCK, SCREEN_CLOCK, &pev, &scfg, &empty_gal);
    CHECK(pa.type == ACT_LOCK_OR_SLEEP);
    pev.type = INPUT_POWER_LONG;
    pa = ui_handle_input(SCREEN_CLOCK, SCREEN_CLOCK, &pev, &scfg, &empty_gal);
    CHECK(pa.type == ACT_POWEROFF);

    /* Settings GENERAL row 4 right -> ACT_CYCLE_AUTO_SLEEP; DEVICE row 7 -> SLEEP STYLE / ROTATE */
    ui_set_settings_tab(SET_TAB_GENERAL);
    InputEvent tev = { INPUT_TAP, 480, 118 + 4 * 63 + 20 };
    pa = ui_handle_input(SCREEN_SETTINGS, SCREEN_CLOCK, &tev, &scfg, &empty_gal);
    CHECK(pa.type == ACT_CYCLE_AUTO_SLEEP);
    ui_set_settings_tab(SET_TAB_DEVICE);
    tev = (InputEvent){ INPUT_TAP, 340, 118 + 7 * 63 + 20 };
    pa = ui_handle_input(SCREEN_SETTINGS, SCREEN_CLOCK, &tev, &scfg, &empty_gal);
    CHECK(pa.type == ACT_CYCLE_SLEEP_STYLE);
    tev = (InputEvent){ INPUT_TAP, 490, 118 + 7 * 63 + 20 };
    pa = ui_handle_input(SCREEN_SETTINGS, SCREEN_CLOCK, &tev, &scfg, &empty_gal);
    CHECK(pa.type == ACT_CYCLE_SLEEP_ROTATE);
    ui_set_settings_tab(SET_TAB_GENERAL);
}

int main(int argc, char **argv) {
    test_config_roundtrip();
    test_day_night_schedule();
    test_touch_transform();
    test_net_parsers();
    test_http_client_loopback();
    test_gallery_scan();
    test_ui_hit_and_partial_refresh();
    test_dict_and_books();
    test_wifi_and_keyboard();
    test_book_decoding();
    test_dashboard_bundle_and_triggers();
    printf("All %d KoboDash C checks passed!\n", g_passed);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump-dir") == 0 && i + 1 < argc) {
            dump_previews(argv[++i]);
            printf("Dumped preview frames to %s\n", argv[i]);
        }
    }
    return 0;
}
