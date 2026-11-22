#define _GNU_SOURCE
#include "kobodash.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop_signal = 0;

static void handle_signal(int sig) {
    g_stop_signal = sig;
}

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

static void apply_timezone(const Config *cfg) {
    if (cfg && cfg->tz_posix[0]) {
        setenv("TZ", cfg->tz_posix, 1);
        tzset();
    }
}

static time_t compute_next_sync_time(const Config *cfg, time_t now) {
    struct tm tmv;
    localtime_r(&now, &tmv);
    int interval = config_active_interval_min(cfg, &tmv);
    if (interval > 0) {
        return now + (time_t)interval * 60;
    }
    struct tm wake = tmv;
    wake.tm_hour = cfg->day_start_hour;
    wake.tm_min = 0;
    wake.tm_sec = 0;
    time_t target = mktime(&wake);
    if (target <= now) {
        target += 86400;
    }
    return target;
}

static int ms_until_next_minute(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int sec_left = 60 - (int)(ts.tv_sec % 60);
    int ms_left = sec_left * 1000 - (int)(ts.tv_nsec / 1000000L) + 60;
    if (ms_left < 100) ms_left = 100;
    return ms_left;
}

static void render_any(Canvas *canvas, ScreenId screen, const Config *cfg,
                       const Telemetry *tel, const SyncState *sync,
                       const Gallery *gal, const Library *lib,
                       ReaderState *rs, const struct tm *now_tm,
                       KdRefreshMode mode) {
    if (screen == SCREEN_LIBRARY) {
        reader_render_library(canvas, lib, cfg, tel, sync, gal, mode);
    } else if (screen == SCREEN_READER) {
        reader_render_page(canvas, rs, cfg, tel, now_tm, mode);
    } else {
        ui_render_screen(canvas, screen, cfg, tel, sync, gal, now_tm, mode);
    }
}

static int run_self_test(const char *base_dir, const char *books_root) {
    Config cfg;
    config_set_defaults(&cfg, base_dir);
    char cfg_path[300];
    snprintf(cfg_path, sizeof(cfg_path), "%s/config.ini", cfg.base_dir);
    (void)config_load(&cfg, cfg_path);
    apply_timezone(&cfg);

    const char *fbv = display_backend_version();
    int font_ok = (font_init(&cfg) == 0) ? 1 : 0;

    char dict_dir[320];
    snprintf(dict_dir, sizeof(dict_dir), "%s/dict", cfg.base_dir);
    DictSet *dicts = dictset_open_dir(dict_dir);
    int ndicts = dicts ? dictset_count(dicts) : 0;
    int dict_hits = 0;
    if (dicts && ndicts > 0) {
        char mw[64], def[160];
        int used = -1;
        if (dictset_lookup(dicts, dictset_find_by_id(dicts, "en"), false, "book", &used, mw, sizeof(mw), def, sizeof(def))) dict_hits++;
        if (dictset_lookup(dicts, dictset_find_by_id(dicts, "en-de"), false, "house", &used, mw, sizeof(mw), def, sizeof(def))) dict_hits++;
        if (dictset_lookup(dicts, dictset_find_by_id(dicts, "de-en"), false, "Bücher", &used, mw, sizeof(mw), def, sizeof(def))) dict_hits++;
    }

    Telemetry tel;
    sysinfo_collect(&tel, &cfg);

    Gallery gal;
    memset(&gal, 0, sizeof(gal));
    gallery_scan(&gal, &cfg);

    static Library lib;
    library_init(&lib, books_root, &cfg);

    InputState *in = input_open(&cfg);
    int fds[6];
    int nfds = input_get_fds(in, fds, 6);

    char ca_path[320];
    snprintf(ca_path, sizeof(ca_path), "%s/certs/cacert.pem", cfg.base_dir);
    int tls_ca_ok = (access(ca_path, R_OK) == 0) ? 1 : 0;

    printf("SELFTEST OK v%s fbink='%s' font=%d dicts=%d(%d/3) tls_ca=%d books=%d gal=%d ev=%d bat=%d%% ip=%s\n",
           KD_VERSION, fbv ? fbv : "none", font_ok, ndicts, dict_hits, tls_ca_ok,
           lib.count, gal.count, nfds, tel.bat_pct, tel.wifi_ip);

    input_close(in);
    dictset_close(dicts);
    font_close();
    return 0;
}

static void stop_push_watcher(int *pipe_fd, pid_t *pid, SyncState *sync) {
    if (*pid > 0) {
        kill(*pid, SIGTERM);
        (void)waitpid(*pid, NULL, 0);
        *pid = -1;
    }
    if (*pipe_fd >= 0) {
        close(*pipe_fd);
        *pipe_fd = -1;
    }
    if (sync) sync->push_connected = false;
}

static void reconcile_push_watcher(const Config *cfg, const Telemetry *tel, SyncState *sync,
                                   const char *alerts_path, int *pipe_fd, pid_t *pid,
                                   bool force_restart) {
    char stream_url[320];
    bool has_stream = (cfg->server_url[0] && net_derive_stream_url(cfg->server_url, stream_url, sizeof(stream_url)));
    bool want_live = has_stream &&
                     (cfg->push_mode == 2 || (cfg->push_mode == 1 && tel && tel->charger_online));
    if (force_restart || !want_live) {
        stop_push_watcher(pipe_fd, pid, sync);
    }
    if (want_live && *pid < 0) {
        (void)net_start_push_watcher(cfg, alerts_path, pipe_fd, pid);
    }
}

static bool load_file_to_buf(const char *path, char *buf, size_t buf_sz) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    size_t n = fread(buf, 1, buf_sz - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    return n > 2;
}

static time_t compute_next_sleep_wake_utc(const Config *cfg, const DashboardBundle *b, time_t now) {
    time_t wake = 0;
    if (cfg && cfg->sleep_rotate_hrs > 0 && cfg->sleep_style == SLEEP_STYLE_CYCLE) {
        wake = now + (time_t)cfg->sleep_rotate_hrs * 3600;
    }
    if (b && b->has_bundle && now >= 1700000000) {
        for (int i = 0; i < b->event_count; i++) {
            const OfflineEvent *ev = &b->events[i];
            if (ev->start_ts <= 0) continue;
            time_t trig = ev->start_ts - (time_t)ev->remind_min * 60;
            if (trig > now + 30 && (wake == 0 || trig < wake)) {
                wake = trig;
            }
        }
    }
    return wake;
}

/* True hardware deep sleep loop with automatic RTC sleep-screen flair rotation
 * (Today & Progress -> Calendar & Agenda -> Stats & Sports every N hours)
 * and offline calendar reminder wakeups. */
static void run_deep_sleep_loop(Canvas *canvas, InputState *input,
                                const Config *cfg, Telemetry *tel,
                                SyncState *sync, DashboardBundle *bundle,
                                AlertTracker *alerts, int *sleep_cycle_idx) {
    sysinfo_collect(tel, cfg);
    bool had_wifi = (tel->wifi_up != 0);

    for (;;) {
        time_t now = time(NULL);
        struct tm now_tm;
        localtime_r(&now, &now_tm);

        /* 1. Draw crisp full-flash sleep screen & wait for EPDC to settle before suspending CPU */
        dashboard_render_sleep_screen(canvas, bundle, cfg->sleep_style, *sleep_cycle_idx,
                                      LOCK_DEEP_SLEEP, cfg, tel, sync, &now_tm, KD_REFRESH_FULL_FLASH);
        display_wait_complete();

        /* 2. Compute next RTC alarm (sleep-screen rotation or upcoming offline calendar event) */
        time_t wake_utc = compute_next_sleep_wake_utc(cfg, bundle, now);

        /* 3. Enter true hardware suspend-to-RAM (~1 mA standby, touchscreen off, Wi-Fi cleanly off) */
        int rc = sysinfo_enter_deep_sleep(cfg, wake_utc);
        if (rc != 0) {
            /* Host/unsupported environment: exit gracefully */
            break;
        }

        /* 4. Just resumed! Check whether user slid Power/Home or RTC wakealarm fired */
        bool user_key = input_drain_wakeup(input);
        now = time(NULL);
        localtime_r(&now, &now_tm);

        bool alarm_wake = (!user_key && wake_utc > 0 &&
                           llabs((long long)now - (long long)wake_utc) <= 45);
        if (!alarm_wake) {
            /* User woke the device via Power slider / Home! Drain any trailing switch bounce */
            usleep(120000);
            (void)input_drain_wakeup(input);
            break;
        }

        /* 5. RTC Wakealarm fired! First check if an offline calendar reminder is due right now */
        if (dashboard_check_triggers(bundle, alerts, now) && alerts->current.active) {
            break;
        }

        /* 6. Otherwise: rotate sleep art to next dashboard page (Today -> Calendar -> Stats) */
        *sleep_cycle_idx = (*sleep_cycle_idx + 1) % 3;

        /* Best-effort quick refresh if Wi-Fi was active before sleep so new sleep page is fresh */
        if (had_wifi && cfg->server_url[0]) {
            net_ensure_wifi(cfg);
            sysinfo_collect(tel, cfg);
            if (tel->wifi_up) {
                (void)net_sync_blocking(cfg, tel, sync);
                if (sync->has_bundle_json) {
                    (void)dashboard_load_cached(sync->cache_bundle_path, bundle);
                }
            }
        }
        /* Loop back to draw the newly rotated page and immediately re-enter deep sleep! */
    }
}

int main(int argc, char **argv) {
    const char *base_dir = "/mnt/onboard/.adds/kobodash";
    const char *books_root = "/mnt/onboard";
    bool self_test = false;
    bool splash_mode = false;
    const char *splash_l1 = "Starting KoboDash...";
    const char *splash_l2 = "Loading e-ink shell";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--base-dir") == 0 && i + 1 < argc) {
            base_dir = argv[++i];
        } else if (strcmp(argv[i], "--books-dir") == 0 && i + 1 < argc) {
            books_root = argv[++i];
        } else if (strcmp(argv[i], "--self-test") == 0) {
            self_test = true;
        } else if (strcmp(argv[i], "--splash") == 0) {
            splash_mode = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') splash_l1 = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-') splash_l2 = argv[++i];
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("kobodash %s\n", KD_VERSION);
            return 0;
        }
    }
    if (self_test) {
        return run_self_test(base_dir, books_root);
    }
    if (splash_mode) {
        Config scfg;
        config_set_defaults(&scfg, base_dir);
        char spath[300];
        snprintf(spath, sizeof(spath), "%s/config.ini", scfg.base_dir);
        (void)config_load(&scfg, spath);
        if (display_init(&scfg) == 0) {
            display_show_banner(splash_l1, splash_l2, scfg.invert_mode != 0);
            display_close();
            return 0;
        }
        return 1;
    }

    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    Config cfg;
    config_set_defaults(&cfg, base_dir);

    char cfg_path[300];
    snprintf(cfg_path, sizeof(cfg_path), "%s/config.ini", cfg.base_dir);
    if (config_load(&cfg, cfg_path) != 0) {
        (void)config_save(&cfg, cfg_path);
    }
    apply_timezone(&cfg);
    if (cfg.cpu_governor[0]) {
        (void)device_set_governor(cfg.cpu_governor);
    }

    if (display_init(&cfg) != 0) {
        fprintf(stderr, "kobodash: failed to initialize display\n");
        return 1;
    }

    (void)font_init(&cfg);

    char dict_dir[320];
    snprintf(dict_dir, sizeof(dict_dir), "%s/dict", cfg.base_dir);
    DictSet *dicts = dictset_open_dir(dict_dir);
    ReaderState *rs = reader_create(&cfg, dicts);

    InputState *input = input_open(&cfg);
    static Canvas canvas;
    static Library lib;
    static WifiState wifi_state;
    static KeyboardState kbd;
    static DashboardBundle bundle;
    static AlertTracker alerts;
    memset(&wifi_state, 0, sizeof(wifi_state));
    memset(&kbd, 0, sizeof(kbd));
    memset(&bundle, 0, sizeof(bundle));
    memset(&alerts, 0, sizeof(alerts));
    ui_set_wifi_state(&wifi_state);
    Telemetry tel;
    Gallery gal;
    memset(&gal, 0, sizeof(gal));
    SyncState sync;
    memset(&sync, 0, sizeof(sync));
    snprintf(sync.cache_image_path, sizeof(sync.cache_image_path),
             "%s/cache/dashboard.img", cfg.base_dir);
    snprintf(sync.cache_bundle_path, sizeof(sync.cache_bundle_path),
             "%s/cache/bundle.json", cfg.base_dir);
    char dismissed_path[300];
    snprintf(dismissed_path, sizeof(dismissed_path), "%s/cache/dismissed.dat", cfg.base_dir);
    dashboard_load_dismissed(dismissed_path, &alerts);

    struct stat stbuf;
    if (stat(sync.cache_image_path, &stbuf) == 0 && stbuf.st_size > 0) {
        sync.has_cached_image = true;
    }
    if (dashboard_load_cached(sync.cache_bundle_path, &bundle)) {
        sync.has_bundle_json = true;
        sync.is_json_dashboard = true;
    }
    int dash_page = 0;
    ui_set_dashboard_bundle(&bundle, dash_page);

    sysinfo_collect(&tel, &cfg);
    gallery_scan(&gal, &cfg);
    library_init(&lib, books_root, &cfg);

    ScreenId current = (ScreenId)cfg.start_screen;
    if (current == SCREEN_GALLERY && gal.count == 0) current = SCREEN_CLOCK;
    if (current >= SCREEN_SETTINGS) current = SCREEN_CLOCK;
    ScreenId prev_non_settings = current;

    time_t now = time(NULL);
    struct tm now_tm;
    localtime_r(&now, &now_tm);
    int last_drawn_min = now_tm.tm_min;
    int last_drawn_mday = now_tm.tm_mday;
    int partial_count = 0;

    render_any(&canvas, current, &cfg, &tel, &sync, &gal, &lib, rs, &now_tm, KD_REFRESH_FULL_FLASH);

    /* Immediate background sync on boot (NTP clock + server dashboard) */
    int sync_pipe_fd = -1;
    pid_t sync_pid = -1;
    const char *sync_res_path = "/tmp/kobodash-sync.bin";
    sync.next_sync_time = compute_next_sync_time(&cfg, now);

    if (net_start_async_sync(&cfg, &tel, &sync, sync_res_path, &sync_pipe_fd, &sync_pid) == 0) {
        sync.in_progress = true;
    }

    /* Optional real-time SSE remote push watcher (active when charging or Always Live) */
    int push_pipe_fd = -1;
    pid_t push_pid = -1;
    const char *push_alerts_path = "/tmp/kobodash-alerts.json";
    reconcile_push_watcher(&cfg, &tel, &sync, push_alerts_path, &push_pipe_fd, &push_pid, false);

    uint64_t boot_start_ms = monotonic_ms();
    uint64_t now_ms = boot_start_ms;
    uint64_t last_activity_ms = boot_start_ms;
    LockMode lock_mode = LOCK_NONE;
    int sleep_cycle_idx = 0;
    uint64_t next_sleep_rot_ms = 0;
    bool boot_marker_cleared = false;
    char boot_pending_path[300];
    snprintf(boot_pending_path, sizeof(boot_pending_path), "%s/.boot_pending", cfg.base_dir);
    uint64_t next_rotate_ms = (cfg.auto_rotate_min > 0)
                                  ? (now_ms + (uint64_t)cfg.auto_rotate_min * 60000ULL)
                                  : 0;
    uint64_t next_status_ms = now_ms + 60000ULL;

    int exit_code = KD_EXIT_STOCK_UI;
    bool running = true;

    while (running && !g_stop_signal) {
        now_ms = monotonic_ms();
        now = time(NULL);
        localtime_r(&now, &now_tm);

        /* Clear autoboot crash-guard marker once KoboDash has run healthily for 15 seconds */
        if (!boot_marker_cleared && now_ms >= boot_start_ms + 15000ULL) {
            unlink(boot_pending_path);
            boot_marker_cleared = true;
        }

        int timeout_ms = 60000;

        int in_to = input_poll_timeout_ms(input, now_ms);
        if (in_to >= 0 && in_to < timeout_ms) timeout_ms = in_to;

        if ((current == SCREEN_CLOCK && cfg.minute_clock && lock_mode == LOCK_NONE) ||
            (current == SCREEN_GALLERY && cfg.gallery_clock_overlay && cfg.minute_clock && lock_mode == LOCK_NONE) ||
            (lock_mode == LOCK_TOUCH && cfg.sleep_style == SLEEP_STYLE_CLOCK) ||
            bundle.event_count > 0) {
            int mt = ms_until_next_minute();
            if (mt < timeout_ms) timeout_ms = mt;
        }

        if (sync.next_sync_time > 0 && !sync.in_progress) {
            long diff_s = (long)(sync.next_sync_time - now);
            int diff_ms = (diff_s <= 0) ? 50 : (diff_s > 60 ? 60000 : (int)(diff_s * 1000));
            if (diff_ms < timeout_ms) timeout_ms = diff_ms;
        }

        bool can_rotate = (lock_mode == LOCK_NONE && cfg.auto_rotate_min > 0 &&
                           current != SCREEN_SETTINGS && current != SCREEN_READER &&
                           current != SCREEN_LIBRARY && !alerts.current.active && !kbd.active);
        if (can_rotate && next_rotate_ms > 0) {
            int rot_ms = (now_ms >= next_rotate_ms) ? 50
                         : (int)((next_rotate_ms - now_ms) > 60000ULL ? 60000ULL : (next_rotate_ms - now_ms));
            if (rot_ms < timeout_ms) timeout_ms = rot_ms;
        }

        if (lock_mode == LOCK_NONE && cfg.auto_sleep_min > 0 && !kbd.active) {
            uint64_t sleep_at = last_activity_ms + (uint64_t)cfg.auto_sleep_min * 60000ULL;
            int sl_ms = (now_ms >= sleep_at) ? 50
                        : (int)((sleep_at - now_ms) > 60000ULL ? 60000ULL : (sleep_at - now_ms));
            if (sl_ms < timeout_ms) timeout_ms = sl_ms;
        }

        if (lock_mode == LOCK_TOUCH && next_sleep_rot_ms > 0) {
            int sr_ms = (now_ms >= next_sleep_rot_ms) ? 50
                        : (int)((next_sleep_rot_ms - now_ms) > 60000ULL ? 60000ULL : (next_sleep_rot_ms - now_ms));
            if (sr_ms < timeout_ms) timeout_ms = sr_ms;
        }

        if (lock_mode == LOCK_NONE && current == SCREEN_STATUS) {
            int st_ms = (now_ms >= next_status_ms) ? 50
                        : (int)((next_status_ms - now_ms) > 60000ULL ? 60000ULL : (next_status_ms - now_ms));
            if (st_ms < timeout_ms) timeout_ms = st_ms;
        }

        int in_fds[6];
        int n_in = input_get_fds(input, in_fds, 6);
        struct pollfd pfds[10];
        int nfds = 0;
        for (int i = 0; i < n_in; i++) {
            pfds[nfds].fd = in_fds[i];
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }
        int sync_pfd_idx = -1;
        if (sync_pipe_fd >= 0) {
            sync_pfd_idx = nfds;
            pfds[nfds].fd = sync_pipe_fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }
        int push_pfd_idx = -1;
        if (push_pipe_fd >= 0) {
            push_pfd_idx = nfds;
            pfds[nfds].fd = push_pipe_fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }

        int pr = poll(pfds, (nfds_t)nfds, timeout_ms);
        now_ms = monotonic_ms();
        now = time(NULL);
        localtime_r(&now, &now_tm);

        /* 0. Real-time SSE Remote Push notification from server */
        if (push_pfd_idx >= 0 && (pfds[push_pfd_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
            char evbuf[32];
            ssize_t rn = read(push_pipe_fd, evbuf, sizeof(evbuf));
            if (rn <= 0) {
                stop_push_watcher(&push_pipe_fd, &push_pid, &sync);
            } else {
                bool got_alert_frame = false;
                for (ssize_t k = 0; k < rn; k++) {
                    if (evbuf[k] == 'C') sync.push_connected = true;
                    else if (evbuf[k] == 'D') sync.push_connected = false;
                    else if (evbuf[k] == 'A') got_alert_frame = true;
                }
                if (got_alert_frame) {
                    static char abuf[16384];
                    if (load_file_to_buf(push_alerts_path, abuf, sizeof(abuf)) &&
                        dashboard_parse_alerts_update(abuf, &bundle)) {
                        ui_set_dashboard_bundle(&bundle, dash_page);
                        bool was_active = alerts.current.active;
                        bool trig_changed = dashboard_check_triggers(&bundle, &alerts, now);
                        if (!kbd.active && current != SCREEN_READER) {
                            if (alerts.current.active && (!was_active || trig_changed)) {
                                dashboard_render_alert_overlay(&canvas, &alerts.current, &cfg, KD_REFRESH_FULL_FLASH);
                                partial_count = 0;
                            } else if ((was_active && !alerts.current.active) ||
                                       current == SCREEN_DASHBOARD || current == SCREEN_CLOCK) {
                                render_any(&canvas, current, &cfg, &tel, &sync, &gal, &lib, rs,
                                           &now_tm, KD_REFRESH_FULL_FLASH);
                                partial_count = 0;
                            }
                        }
                    }
                }
            }
            continue;
        }

        /* 1. Async sync completion */
        if (sync_pfd_idx >= 0 && (pfds[sync_pfd_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
            char drain[16];
            (void)read(sync_pipe_fd, drain, sizeof(drain));
            close(sync_pipe_fd);
            sync_pipe_fd = -1;
            if (sync_pid > 0) {
                (void)waitpid(sync_pid, NULL, 0);
                sync_pid = -1;
            }
            (void)net_read_sync_result(sync_res_path, &sync);
            now = time(NULL);
            localtime_r(&now, &now_tm);
            sync.next_sync_time = compute_next_sync_time(&cfg, now);
            sysinfo_collect(&tel, &cfg);
            gallery_scan(&gal, &cfg);

            if (sync.has_bundle_json && dashboard_load_cached(sync.cache_bundle_path, &bundle)) {
                if (dash_page >= bundle.page_count) dash_page = 0;
                ui_set_dashboard_bundle(&bundle, dash_page);
            }
            bool was_alert = alerts.current.active;
            (void)dashboard_check_triggers(&bundle, &alerts, now);
            reconcile_push_watcher(&cfg, &tel, &sync, push_alerts_path, &push_pipe_fd, &push_pid, false);

            /* Never disturb active keyboard, book reading, or unchanged 304 dashboard */
            if (kbd.active || current == SCREEN_READER || current == SCREEN_LIBRARY) {
                continue;
            }
            if (alerts.current.active) {
                if (!was_alert || sync.content_changed) {
                    dashboard_render_alert_overlay(&canvas, &alerts.current, &cfg, KD_REFRESH_FULL_FLASH);
                    partial_count = 0;
                }
                continue;
            }
            if (lock_mode == LOCK_TOUCH) {
                if (sync.content_changed || was_alert) {
                    dashboard_render_sleep_screen(&canvas, &bundle, cfg.sleep_style, sleep_cycle_idx,
                                                  LOCK_TOUCH, &cfg, &tel, &sync, &now_tm, KD_REFRESH_FULL_FLASH);
                    partial_count = 0;
                }
                continue;
            }
            if (current == SCREEN_DASHBOARD && !sync.content_changed && sync.last_ok == 1 &&
                (sync.has_cached_image || sync.has_bundle_json) && !was_alert) {
                continue;
            }
            KdRefreshMode rm = (current == SCREEN_DASHBOARD && sync.content_changed) || was_alert
                                   ? KD_REFRESH_FULL_FLASH
                                   : KD_REFRESH_GRAY_PARTIAL;
            render_any(&canvas, current, &cfg, &tel, &sync, &gal, &lib, rs, &now_tm, rm);
            last_drawn_min = now_tm.tm_min;
            last_drawn_mday = now_tm.tm_mday;
            if (rm == KD_REFRESH_FULL_FLASH) partial_count = 0;
            continue;
        }

        /* 2. Hardware timers (Home 1.8s hold, Touch 380ms word hold) + input events */
        InputEvent iev;
        memset(&iev, 0, sizeof(iev));
        bool have_event = input_check_timers(input, now_ms, &iev);

        if (!have_event && pr > 0) {
            for (int i = 0; i < n_in; i++) {
                if (pfds[i].revents & POLLIN) {
                    if (input_read_event(input, pfds[i].fd, now_ms, &iev)) {
                        have_event = true;
                        break;
                    }
                }
            }
        }

        if (have_event) {
            /* Global Power Slider Long-Hold (~2.4s) -> Clean Power Off from ANY screen/lock state */
            if (iev.type == INPUT_POWER_LONG) {
                display_show_banner("Powering Off...", "KoboDash", cfg.invert_mode != 0);
                exit_code = KD_EXIT_POWEROFF;
                running = false;
                continue;
            }

            /* Touch-Lock Mode: ignore ALL taps/swipes; unlock on Power slider or short Home */
            if (lock_mode == LOCK_TOUCH) {
                if (iev.type == INPUT_HOME_LONG) {
                    lock_mode = LOCK_NONE;
                    display_show_banner("Returning to Stock UI...", "Starting Kobo Nickel", cfg.invert_mode != 0);
                    exit_code = KD_EXIT_STOCK_UI;
                    running = false;
                    continue;
                }
                if (iev.type == INPUT_POWER_SHORT || iev.type == INPUT_HOME_SHORT) {
                    lock_mode = LOCK_NONE;
                    last_activity_ms = now_ms;
                    if (cfg.auto_rotate_min > 0) {
                        next_rotate_ms = now_ms + (uint64_t)cfg.auto_rotate_min * 60000ULL;
                    }
                    sysinfo_collect(&tel, &cfg);
                    if (alerts.current.active) {
                        dashboard_render_alert_overlay(&canvas, &alerts.current, &cfg, KD_REFRESH_FULL_FLASH);
                    } else {
                        render_any(&canvas, current, &cfg, &tel, &sync, &gal, &lib, rs, &now_tm, KD_REFRESH_FULL_FLASH);
                    }
                    last_drawn_min = now_tm.tm_min;
                    last_drawn_mday = now_tm.tm_mday;
                    partial_count = 0;
                }
                continue;
            }

            /* User activity resets auto-sleep inactivity timer */
            last_activity_ms = now_ms;

            /* Global Power Slider Short Slide -> True Deep Sleep (unplugged) or Touch-Lock (plugged in) */
            if (iev.type == INPUT_POWER_SHORT) {
                kbd.active = false;
                sysinfo_collect(&tel, &cfg);
                if (tel.charger_online) {
                    /* Desk / Plugged-In Mode: Touch-Lock with rotating sleep screen */
                    lock_mode = LOCK_TOUCH;
                    next_sleep_rot_ms = (cfg.sleep_rotate_hrs > 0 && cfg.sleep_style == SLEEP_STYLE_CYCLE)
                                            ? (now_ms + (uint64_t)cfg.sleep_rotate_hrs * 3600000ULL)
                                            : 0;
                    dashboard_render_sleep_screen(&canvas, &bundle, cfg.sleep_style, sleep_cycle_idx,
                                                  LOCK_TOUCH, &cfg, &tel, &sync, &now_tm, KD_REFRESH_FULL_FLASH);
                    partial_count = 0;
                } else {
                    /* Battery Mode: True Hardware Deep Sleep with RTC rotation & calendar wakeups */
                    stop_push_watcher(&push_pipe_fd, &push_pid, &sync);
                    if (sync_pid > 0) {
                        kill(sync_pid, SIGTERM);
                        (void)waitpid(sync_pid, NULL, 0);
                        sync_pid = -1;
                        if (sync_pipe_fd >= 0) { close(sync_pipe_fd); sync_pipe_fd = -1; }
                        sync.in_progress = false;
                    }
                    run_deep_sleep_loop(&canvas, input, &cfg, &tel, &sync, &bundle, &alerts, &sleep_cycle_idx);
                    now = time(NULL);
                    localtime_r(&now, &now_tm);
                    now_ms = monotonic_ms();
                    last_activity_ms = now_ms;
                    if (cfg.auto_rotate_min > 0) {
                        next_rotate_ms = now_ms + (uint64_t)cfg.auto_rotate_min * 60000ULL;
                    }
                    sysinfo_collect(&tel, &cfg);
                    if (alerts.current.active) {
                        dashboard_render_alert_overlay(&canvas, &alerts.current, &cfg, KD_REFRESH_FULL_FLASH);
                    } else {
                        render_any(&canvas, current, &cfg, &tel, &sync, &gal, &lib, rs, &now_tm, KD_REFRESH_FULL_FLASH);
                    }
                    last_drawn_min = now_tm.tm_min;
                    last_drawn_mday = now_tm.tm_mday;
                    partial_count = 0;
                    /* Reconnect Wi-Fi & sync quietly in background after instant wake */
                    if (!sync.in_progress &&
                        net_start_async_sync(&cfg, &tel, &sync, sync_res_path, &sync_pipe_fd, &sync_pid) == 0) {
                        sync.in_progress = true;
                    }
                    reconcile_push_watcher(&cfg, &tel, &sync, push_alerts_path, &push_pipe_fd, &push_pid, false);
                }
                continue;
            }

            /* Active on-screen keyboard modal */
            if (kbd.active) {
                if (iev.type == INPUT_HOME_LONG) {
                    kbd.active = false;
                    display_show_banner("Returning to Stock UI...", "Starting Kobo Nickel", cfg.invert_mode != 0);
                    exit_code = KD_EXIT_STOCK_UI;
                    running = false;
                    continue;
                }
                KbdResult kr = kbd_handle_input(&kbd, &iev);
                if (kr == KBD_RESULT_REDRAW_INPUT) {
                    kbd_render(&canvas, &kbd, &cfg, true, KD_REFRESH_FAST_DU);
                } else if (kr == KBD_RESULT_REDRAW_ALL) {
                    kbd_render(&canvas, &kbd, &cfg, false, KD_REFRESH_FAST_DU);
                } else if (kr == KBD_RESULT_CANCEL) {
                    render_any(&canvas, current, &cfg, &tel, &sync, &gal, &lib, rs, &now_tm, KD_REFRESH_FULL_FLASH);
                    partial_count = 0;
                } else if (kr == KBD_RESULT_SUBMIT) {
                    bool reopen_kbd = false;
                    if (kbd.purpose == KBD_WIFI_SSID) {
                        if (kbd.buf[0]) {
                            snprintf(wifi_state.pending_ssid, sizeof(wifi_state.pending_ssid), "%s", kbd.buf);
                            wifi_state.pending_secured = true;
                            char title[80];
                            snprintf(title, sizeof(title), "Password: %.52s", wifi_state.pending_ssid);
                            kbd_open(&kbd, KBD_WIFI_PASS, title,
                                     "Leave blank for an Open network", "", 64, true);
                            reopen_kbd = true;
                        }
                    } else if (kbd.purpose == KBD_WIFI_PASS) {
                        char bmsg[96];
                        snprintf(bmsg, sizeof(bmsg), "Joining %.40s...", wifi_state.pending_ssid);
                        display_show_banner("Connecting Wi-Fi...", bmsg, cfg.invert_mode != 0);
                        (void)wifi_connect(&cfg, wifi_state.pending_ssid, kbd.buf,
                                           wifi_state.status_msg, sizeof(wifi_state.status_msg));
                        sysinfo_collect(&tel, &cfg);
                        (void)wifi_scan(&cfg, &tel, &wifi_state);
                    } else if (kbd.purpose == KBD_SERVER_URL) {
                        snprintf(cfg.server_url, sizeof(cfg.server_url), "%s", kbd.buf);
                        sync.last_etag[0] = '\0';
                        (void)config_save(&cfg, cfg_path);
                        reconcile_push_watcher(&cfg, &tel, &sync, push_alerts_path, &push_pipe_fd, &push_pid, true);
                        if (cfg.server_url[0] && !sync.in_progress) {
                            if (net_start_async_sync(&cfg, &tel, &sync, sync_res_path, &sync_pipe_fd, &sync_pid) == 0) {
                                sync.in_progress = true;
                            }
                        }
                    } else if (kbd.purpose == KBD_SERVER_TOKEN) {
                        snprintf(cfg.server_token, sizeof(cfg.server_token), "%s", kbd.buf);
                        sync.last_etag[0] = '\0';
                        (void)config_save(&cfg, cfg_path);
                        reconcile_push_watcher(&cfg, &tel, &sync, push_alerts_path, &push_pipe_fd, &push_pid, true);
                    } else if (kbd.purpose == KBD_NTP_SERVER) {
                        snprintf(cfg.ntp_server, sizeof(cfg.ntp_server), "%s", kbd.buf);
                        (void)config_save(&cfg, cfg_path);
                    } else if (kbd.purpose == KBD_SCREENSAVER_DIR) {
                        snprintf(cfg.screensaver_dir, sizeof(cfg.screensaver_dir), "%s", kbd.buf);
                        (void)config_save(&cfg, cfg_path);
                        gallery_scan(&gal, &cfg);
                    }
                    if (reopen_kbd) {
                        kbd_render(&canvas, &kbd, &cfg, false, KD_REFRESH_FULL_FLASH);
                    } else {
                        sysinfo_collect(&tel, &cfg);
                        render_any(&canvas, current, &cfg, &tel, &sync, &gal, &lib, rs, &now_tm, KD_REFRESH_FULL_FLASH);
                        partial_count = 0;
                    }
                }
                continue;
            }

            /* Active Attention Alert Overlay (dismiss / snooze / view all) */
            if (alerts.current.active && current != SCREEN_READER) {
                int ar = dashboard_handle_alert_input(&alerts.current, &iev);
                if (ar == 4) {
                    display_show_banner("Returning to Stock UI...", "Starting Kobo Nickel", cfg.invert_mode != 0);
                    exit_code = KD_EXIT_STOCK_UI;
                    running = false;
                } else if (ar == 1 || ar == 2 || ar == 3) {
                    bool was_remote = alerts.current.is_remote;
                    if (ar == 2) {
                        dashboard_snooze_active(&alerts, now, 30, dismissed_path);
                    } else {
                        dashboard_dismiss_active(&alerts, dismissed_path);
                    }
                    if (ar == 3 && bundle.has_bundle && bundle.page_count > 0) {
                        int target_p = 0;
                        DashPageKind want_k = was_remote ? DASH_PAGE_LIST : DASH_PAGE_CALENDAR;
                        for (int i = 0; i < bundle.page_count; i++) {
                            if (bundle.pages[i].kind == want_k) { target_p = i; break; }
                        }
                        dash_page = target_p;
                        ui_set_dashboard_bundle(&bundle, dash_page);
                        current = SCREEN_DASHBOARD;
                        prev_non_settings = SCREEN_DASHBOARD;
                    }
                    (void)dashboard_check_triggers(&bundle, &alerts, now);
                    if (alerts.current.active && ar != 3) {
                        dashboard_render_alert_overlay(&canvas, &alerts.current, &cfg, KD_REFRESH_FULL_FLASH);
                    } else {
                        sysinfo_collect(&tel, &cfg);
                        render_any(&canvas, current, &cfg, &tel, &sync, &gal, &lib, rs, &now_tm, KD_REFRESH_FULL_FLASH);
                    }
                    partial_count = 0;
                }
                continue;
            }

            /* Book Reader screen input */
            if (current == SCREEN_READER) {
                bool cfg_dirty = false, exit_stock = false;
                ScreenId next_scr = SCREEN_READER;
                KdRefreshMode rm = KD_REFRESH_FAST_DU;
                if (reader_handle_book_input(rs, &lib, &iev, &cfg, &cfg_dirty,
                                             &next_scr, &exit_stock, &rm)) {
                    if (cfg_dirty) (void)config_save(&cfg, cfg_path);
                    if (exit_stock) {
                        display_show_banner("Returning to Stock UI...", "Starting Kobo Nickel", cfg.invert_mode != 0);
                        exit_code = KD_EXIT_STOCK_UI;
                        running = false;
                    } else {
                        current = next_scr;
                        if (current != SCREEN_READER) prev_non_settings = current;
                        sysinfo_collect(&tel, &cfg);
                        render_any(&canvas, current, &cfg, &tel, &sync, &gal, &lib, rs, &now_tm, rm);
                        last_drawn_min = now_tm.tm_min;
                        last_drawn_mday = now_tm.tm_mday;
                    }
                }
                continue;
            }

            /* Library screen input */
            if (current == SCREEN_LIBRARY) {
                ScreenId next_scr = SCREEN_LIBRARY;
                bool open_set = false, exit_stock = false, prev_s = false, next_s = false;
                bool do_sync = false, need_full = false;
                if (reader_handle_library_input(&lib, rs, &iev, &cfg, &next_scr,
                                                &open_set, &exit_stock, &prev_s, &next_s,
                                                &do_sync, &need_full)) {
                    if (exit_stock) {
                        display_show_banner("Returning to Stock UI...", "Starting Kobo Nickel", cfg.invert_mode != 0);
                        exit_code = KD_EXIT_STOCK_UI;
                        running = false;
                    } else {
                        if (open_set) {
                            prev_non_settings = SCREEN_LIBRARY;
                            current = SCREEN_SETTINGS;
                            need_full = true;
                        } else if (prev_s) {
                            ui_step_carousel(SCREEN_LIBRARY, dash_page, -1, &gal, &bundle, false, &current, &dash_page);
                            ui_set_dashboard_bundle(&bundle, dash_page);
                            prev_non_settings = current;
                            need_full = true;
                        } else if (next_s) {
                            ui_step_carousel(SCREEN_LIBRARY, dash_page, +1, &gal, &bundle, false, &current, &dash_page);
                            ui_set_dashboard_bundle(&bundle, dash_page);
                            prev_non_settings = current;
                            need_full = true;
                        } else {
                            current = next_scr;
                        }
                        if (do_sync && !sync.in_progress) {
                            sync.last_etag[0] = '\0';
                            if (net_start_async_sync(&cfg, &tel, &sync, sync_res_path, &sync_pipe_fd, &sync_pid) == 0) {
                                sync.in_progress = true;
                            }
                        }
                        sysinfo_collect(&tel, &cfg);
                        render_any(&canvas, current, &cfg, &tel, &sync, &gal, &lib, rs, &now_tm,
                                   need_full ? KD_REFRESH_FULL_FLASH : KD_REFRESH_FAST_DU);
                        last_drawn_min = now_tm.tm_min;
                        last_drawn_mday = now_tm.tm_mday;
                        if (need_full) partial_count = 0;
                    }
                }
                continue;
            }

            /* Standard UI screens (Clock, Dashboard, Status, Gallery, Settings) */
            UiAction act = ui_handle_input(current, prev_non_settings, &iev, &cfg, &gal);
            bool need_full = false;
            bool need_partial = false;
            bool cfg_dirty = false;

            switch (act.type) {
                case ACT_NEXT_SCREEN:
                    ui_step_carousel(current, dash_page, +1, &gal, &bundle, false, &current, &dash_page);
                    ui_set_dashboard_bundle(&bundle, dash_page);
                    prev_non_settings = current;
                    if (current == SCREEN_LIBRARY) library_scan(&lib, &cfg);
                    sysinfo_collect(&tel, &cfg);
                    need_full = true;
                    break;

                case ACT_PREV_SCREEN:
                    ui_step_carousel(current, dash_page, -1, &gal, &bundle, false, &current, &dash_page);
                    ui_set_dashboard_bundle(&bundle, dash_page);
                    prev_non_settings = current;
                    if (current == SCREEN_LIBRARY) library_scan(&lib, &cfg);
                    sysinfo_collect(&tel, &cfg);
                    need_full = true;
                    break;

                case ACT_GOTO_SCREEN:
                    current = (ScreenId)act.arg;
                    prev_non_settings = current;
                    if (current == SCREEN_LIBRARY) library_scan(&lib, &cfg);
                    sysinfo_collect(&tel, &cfg);
                    need_full = true;
                    break;

                case ACT_GOTO_DASH_PAGE:
                    dash_page = act.arg;
                    ui_set_dashboard_bundle(&bundle, dash_page);
                    current = SCREEN_DASHBOARD;
                    prev_non_settings = SCREEN_DASHBOARD;
                    sysinfo_collect(&tel, &cfg);
                    need_full = true;
                    break;

                case ACT_CYCLE_PUSH_MODE:
                    cfg.push_mode = (cfg.push_mode + 1) % 3;
                    cfg_dirty = true;
                    reconcile_push_watcher(&cfg, &tel, &sync, push_alerts_path, &push_pipe_fd, &push_pid, true);
                    need_partial = true;
                    break;

                case ACT_OPEN_SETTINGS:
                    if (current != SCREEN_SETTINGS) prev_non_settings = current;
                    current = SCREEN_SETTINGS;
                    ui_set_power_confirm(0);
                    sysinfo_collect(&tel, &cfg);
                    need_full = true;
                    break;

                case ACT_OPEN_SETTINGS_TAB:
                    if (current != SCREEN_SETTINGS) prev_non_settings = current;
                    current = SCREEN_SETTINGS;
                    ui_set_settings_tab((SettingsTab)act.arg);
                    ui_set_power_confirm(0);
                    sysinfo_collect(&tel, &cfg);
                    if ((SettingsTab)act.arg == SET_TAB_WIFI && wifi_state.count == 0) {
                        display_show_banner("Scanning Wi-Fi...", "Searching nearby 2.4 GHz networks", cfg.invert_mode != 0);
                        (void)wifi_scan(&cfg, &tel, &wifi_state);
                        sysinfo_collect(&tel, &cfg);
                    }
                    need_full = true;
                    break;

                case ACT_SET_TAB:
                    ui_set_settings_tab((SettingsTab)act.arg);
                    ui_set_power_confirm(0);
                    ui_set_device_notice("");
                    sysinfo_collect(&tel, &cfg);
                    if ((SettingsTab)act.arg == SET_TAB_WIFI && wifi_state.count == 0) {
                        display_show_banner("Scanning Wi-Fi...", "Searching nearby 2.4 GHz networks", cfg.invert_mode != 0);
                        (void)wifi_scan(&cfg, &tel, &wifi_state);
                        sysinfo_collect(&tel, &cfg);
                        need_full = true;
                    } else {
                        need_partial = true;
                    }
                    break;

                case ACT_CLOSE_SETTINGS:
                    ui_set_power_confirm(0);
                    current = (ScreenId)act.arg;
                    if (current == SCREEN_GALLERY && gal.count == 0) current = SCREEN_CLOCK;
                    if (current == SCREEN_LIBRARY) library_scan(&lib, &cfg);
                    sysinfo_collect(&tel, &cfg);
                    need_full = true;
                    break;

                case ACT_GALLERY_NEXT:
                    gallery_advance(&gal, +1);
                    need_full = true;
                    break;

                case ACT_TOGGLE_MINUTE_CLOCK:
                    cfg.minute_clock = !cfg.minute_clock;
                    cfg_dirty = true;
                    need_partial = true;
                    break;

                case ACT_CYCLE_DAY_INTERVAL:
                    cfg.day_interval_min = config_next_day_interval(cfg.day_interval_min);
                    sync.next_sync_time = compute_next_sync_time(&cfg, now);
                    cfg_dirty = true;
                    need_partial = true;
                    break;

                case ACT_CYCLE_NIGHT_INTERVAL:
                    cfg.night_interval_min = config_next_night_interval(cfg.night_interval_min);
                    sync.next_sync_time = compute_next_sync_time(&cfg, now);
                    cfg_dirty = true;
                    need_partial = true;
                    break;

                case ACT_CYCLE_DAY_START:
                    cfg.day_start_hour = (cfg.day_start_hour < 5 || cfg.day_start_hour >= 10) ? 5 : (cfg.day_start_hour + 1);
                    sync.next_sync_time = compute_next_sync_time(&cfg, now);
                    cfg_dirty = true;
                    need_partial = true;
                    break;

                case ACT_CYCLE_NIGHT_START:
                    cfg.night_start_hour = (cfg.night_start_hour + 1) % 24;
                    if (cfg.night_start_hour > 1 && cfg.night_start_hour < 20) cfg.night_start_hour = 20;
                    sync.next_sync_time = compute_next_sync_time(&cfg, now);
                    cfg_dirty = true;
                    need_partial = true;
                    break;

                case ACT_CYCLE_AUTO_ROTATE:
                    cfg.auto_rotate_min = config_next_rotate_interval(cfg.auto_rotate_min);
                    next_rotate_ms = (cfg.auto_rotate_min > 0)
                                         ? (now_ms + (uint64_t)cfg.auto_rotate_min * 60000ULL)
                                         : 0;
                    cfg_dirty = true;
                    need_partial = true;
                    break;

                case ACT_CYCLE_AUTO_SLEEP:
                    cfg.auto_sleep_min = config_next_auto_sleep(cfg.auto_sleep_min);
                    last_activity_ms = now_ms;
                    cfg_dirty = true;
                    need_partial = true;
                    break;

                case ACT_CYCLE_SLEEP_STYLE:
                    cfg.sleep_style = config_next_sleep_style(cfg.sleep_style);
                    ui_set_device_notice("Sleep screen style updated");
                    cfg_dirty = true;
                    need_partial = true;
                    break;

                case ACT_CYCLE_SLEEP_ROTATE:
                    cfg.sleep_rotate_hrs = config_next_sleep_rotate(cfg.sleep_rotate_hrs);
                    ui_set_device_notice(cfg.sleep_rotate_hrs > 0
                                             ? "Sleep screen rotates automatically"
                                             : "Sleep screen fixed (no rotation)");
                    cfg_dirty = true;
                    need_partial = true;
                    break;

                case ACT_CYCLE_TIMEZONE:
                    config_next_timezone(cfg.tz_posix, cfg.tz_posix, sizeof(cfg.tz_posix));
                    apply_timezone(&cfg);
                    now = time(NULL);
                    localtime_r(&now, &now_tm);
                    sync.next_sync_time = compute_next_sync_time(&cfg, now);
                    cfg_dirty = true;
                    need_partial = true;
                    break;

                case ACT_CYCLE_START_SCREEN:
                    cfg.start_screen = config_next_start_screen(cfg.start_screen);
                    cfg_dirty = true;
                    need_partial = true;
                    break;

                case ACT_CYCLE_FULL_REFRESH:
                    cfg.full_refresh_every = config_next_full_refresh(cfg.full_refresh_every);
                    cfg_dirty = true;
                    need_partial = true;
                    break;

                case ACT_CYCLE_READER_FONT:
                    cfg.reader_font_px = config_next_reader_font(cfg.reader_font_px);
                    cfg_dirty = true;
                    need_partial = true;
                    break;

                case ACT_CYCLE_DICT_LANG:
                    snprintf(cfg.dict_lang, sizeof(cfg.dict_lang), "%s", config_next_dict_lang(cfg.dict_lang));
                    cfg_dirty = true;
                    need_partial = true;
                    break;

                case ACT_CYCLE_READER_FLASH:
                    cfg.reader_flash_every = config_next_reader_flash(cfg.reader_flash_every);
                    cfg_dirty = true;
                    need_partial = true;
                    break;

                case ACT_TOGGLE_GALLERY_CLOCK:
                    cfg.gallery_clock_overlay = !cfg.gallery_clock_overlay;
                    cfg_dirty = true;
                    need_partial = true;
                    break;

                case ACT_TOGGLE_THEME:
                    cfg.invert_mode = !cfg.invert_mode;
                    cfg_dirty = true;
                    need_full = true;
                    break;

                case ACT_TOGGLE_24H:
                    cfg.clock_24h = !cfg.clock_24h;
                    cfg_dirty = true;
                    need_partial = true;
                    break;

                case ACT_TOGGLE_SSH:
                    (void)sysinfo_toggle_ssh(&cfg, !tel.ssh_running);
                    sysinfo_collect(&tel, &cfg);
                    ui_set_device_notice(tel.ssh_running ? "SSH active on port 2222" : "SSH stopped");
                    need_partial = true;
                    break;

                case ACT_WIFI_TOGGLE: {
                    bool turn_on = !tel.wifi_up;
                    display_show_banner(turn_on ? "Enabling Wi-Fi..." : "Disabling Wi-Fi...",
                                        "Updating wlan0 radio", cfg.invert_mode != 0);
                    (void)wifi_set_power(&cfg, turn_on, wifi_state.status_msg, sizeof(wifi_state.status_msg));
                    sysinfo_collect(&tel, &cfg);
                    if (turn_on) (void)wifi_scan(&cfg, &tel, &wifi_state);
                    need_full = true;
                    break;
                }

                case ACT_WIFI_SCAN:
                    display_show_banner("Scanning Wi-Fi...", "Searching nearby 2.4 GHz networks", cfg.invert_mode != 0);
                    (void)wifi_scan(&cfg, &tel, &wifi_state);
                    sysinfo_collect(&tel, &cfg);
                    need_full = true;
                    break;

                case ACT_WIFI_RECONNECT:
                    display_show_banner("Resetting Wi-Fi Chip...", "Power-cycling ar6000 & DHCP", cfg.invert_mode != 0);
                    (void)wifi_hard_reset(&cfg, wifi_state.status_msg, sizeof(wifi_state.status_msg));
                    sysinfo_collect(&tel, &cfg);
                    (void)wifi_scan(&cfg, &tel, &wifi_state);
                    need_full = true;
                    break;

                case ACT_WIFI_HIDDEN:
                    kbd_open(&kbd, KBD_WIFI_SSID, "Hidden Wi-Fi Network",
                             "Enter exact network name (SSID)", "", 64, false);
                    kbd_render(&canvas, &kbd, &cfg, false, KD_REFRESH_FULL_FLASH);
                    break;

                case ACT_WIFI_SELECT_NET:
                    if (act.arg >= 0 && act.arg < wifi_state.count) {
                        const WifiNetwork *wn = &wifi_state.nets[act.arg];
                        snprintf(wifi_state.pending_ssid, sizeof(wifi_state.pending_ssid), "%s", wn->ssid);
                        wifi_state.pending_secured = wn->secured;
                        if (!wn->secured) {
                            char bmsg[96];
                            snprintf(bmsg, sizeof(bmsg), "Joining open %.38s...", wn->ssid);
                            display_show_banner("Connecting Wi-Fi...", bmsg, cfg.invert_mode != 0);
                            (void)wifi_connect(&cfg, wn->ssid, "", wifi_state.status_msg, sizeof(wifi_state.status_msg));
                            sysinfo_collect(&tel, &cfg);
                            (void)wifi_scan(&cfg, &tel, &wifi_state);
                            need_full = true;
                        } else {
                            char title[80];
                            snprintf(title, sizeof(title), "Wi-Fi: %.55s", wn->ssid);
                            kbd_open(&kbd, KBD_WIFI_PASS, title,
                                     wn->saved ? "Enter new password (or Cancel)" : "Enter WPA/WPA2 password",
                                     "", 64, true);
                            kbd_render(&canvas, &kbd, &cfg, false, KD_REFRESH_FULL_FLASH);
                        }
                    }
                    break;

                case ACT_WIFI_FORGET_NET:
                    if (act.arg >= 0 && act.arg < wifi_state.count) {
                        (void)wifi_forget(&cfg, wifi_state.nets[act.arg].ssid,
                                          wifi_state.status_msg, sizeof(wifi_state.status_msg));
                        sysinfo_collect(&tel, &cfg);
                        (void)wifi_scan(&cfg, &tel, &wifi_state);
                        need_full = true;
                    }
                    break;

                case ACT_WIFI_PAGE_PREV:
                    if (wifi_state.page > 0) {
                        wifi_state.page--;
                        need_partial = true;
                    }
                    break;

                case ACT_WIFI_PAGE_NEXT:
                    if ((wifi_state.page + 1) * 5 < wifi_state.count) {
                        wifi_state.page++;
                        need_partial = true;
                    }
                    break;

                case ACT_EDIT_SERVER_URL:
                    kbd_open(&kbd, KBD_SERVER_URL, "Dashboard Server URL",
                             "https:// or http:// (JSON bundle or PNG/JPG)",
                             cfg.server_url, sizeof(cfg.server_url) - 1, false);
                    kbd_render(&canvas, &kbd, &cfg, false, KD_REFRESH_FULL_FLASH);
                    break;

                case ACT_EDIT_SERVER_TOKEN:
                    kbd_open(&kbd, KBD_SERVER_TOKEN, "Auth Bearer Token",
                             "Sent as Authorization: Bearer <token> (optional)",
                             cfg.server_token, sizeof(cfg.server_token) - 1, true);
                    kbd_render(&canvas, &kbd, &cfg, false, KD_REFRESH_FULL_FLASH);
                    break;

                case ACT_EDIT_NTP_SERVER:
                    kbd_open(&kbd, KBD_NTP_SERVER, "NTP Time Server",
                             "Hostname or LAN router IP (e.g. pool.ntp.org)",
                             cfg.ntp_server, sizeof(cfg.ntp_server) - 1, false);
                    kbd_render(&canvas, &kbd, &cfg, false, KD_REFRESH_FULL_FLASH);
                    break;

                case ACT_EDIT_SCREENSAVER_DIR:
                    kbd_open(&kbd, KBD_SCREENSAVER_DIR, "Screensaver Folder",
                             "Leave empty for auto (/mnt/sd/screensavers)",
                             cfg.screensaver_dir, sizeof(cfg.screensaver_dir) - 1, false);
                    kbd_render(&canvas, &kbd, &cfg, false, KD_REFRESH_FULL_FLASH);
                    break;

                case ACT_CYCLE_CPU_GOV: {
                    char govs[8][24];
                    int ng = device_list_governors(govs, 8);
                    const char *cur_g = cfg.cpu_governor[0] ? cfg.cpu_governor : tel.cpu_gov;
                    int next_i = 0;
                    for (int i = 0; i < ng; i++) {
                        if (strcmp(govs[i], cur_g) == 0) { next_i = (i + 1) % ng; break; }
                    }
                    if (ng > 0) {
                        snprintf(cfg.cpu_governor, sizeof(cfg.cpu_governor), "%s", govs[next_i]);
                        int rc = device_set_governor(cfg.cpu_governor);
                        sysinfo_collect(&tel, &cfg);
                        ui_set_device_notice(rc == 0 ? "CPU governor updated" : "Governor saved");
                        cfg_dirty = true;
                        need_partial = true;
                    }
                    break;
                }

                case ACT_TOGGLE_TOUCH_SWAP:
                    cfg.touch_swap_xy = !cfg.touch_swap_xy;
                    input_update_config(input, &cfg);
                    ui_set_device_notice("Touch Swap XY updated");
                    cfg_dirty = true;
                    need_partial = true;
                    break;

                case ACT_TOGGLE_TOUCH_MIRROR_X:
                    cfg.touch_mirror_x = !cfg.touch_mirror_x;
                    input_update_config(input, &cfg);
                    ui_set_device_notice("Touch Mirror X updated");
                    cfg_dirty = true;
                    need_partial = true;
                    break;

                case ACT_TOGGLE_TOUCH_MIRROR_Y:
                    cfg.touch_mirror_y = !cfg.touch_mirror_y;
                    input_update_config(input, &cfg);
                    ui_set_device_notice("Touch Mirror Y updated");
                    cfg_dirty = true;
                    need_partial = true;
                    break;

                case ACT_TOGGLE_SD_MOUNT: {
                    char msg[96];
                    (void)device_toggle_sd(&cfg, !tel.sd_mounted, msg, sizeof(msg));
                    sysinfo_collect(&tel, &cfg);
                    gallery_scan(&gal, &cfg);
                    ui_set_device_notice(msg);
                    need_partial = true;
                    break;
                }

                case ACT_TOGGLE_AUTO_BOOT:
                    cfg.auto_boot = !cfg.auto_boot;
                    if (cfg.auto_boot) {
                        unlink(boot_pending_path);
                    }
                    ui_set_device_notice(cfg.auto_boot
                                             ? "Auto-boot ON (self-test + crash guard)"
                                             : "Auto-boot OFF (boots to Stock Kobo UI)");
                    cfg_dirty = true;
                    need_partial = true;
                    break;

                case ACT_CLEAR_ETAG_SYNC:
                    sync.last_etag[0] = '\0';
                    /* fall through */
                case ACT_SYNC_NOW:
                    if (!sync.in_progress) {
                        if (act.type == ACT_SYNC_NOW) sync.last_etag[0] = '\0';
                        if (net_start_async_sync(&cfg, &tel, &sync, sync_res_path, &sync_pipe_fd, &sync_pid) == 0) {
                            sync.in_progress = true;
                            need_partial = true;
                        }
                    }
                    break;

                case ACT_FULL_REFRESH:
                    ui_set_power_confirm(0);
                    sysinfo_collect(&tel, &cfg);
                    gallery_scan(&gal, &cfg);
                    need_full = true;
                    break;

                case ACT_EXIT_STOCK_UI:
                    display_show_banner("Returning to Stock UI...", "Starting Kobo Nickel", cfg.invert_mode != 0);
                    exit_code = KD_EXIT_STOCK_UI;
                    running = false;
                    break;

                case ACT_REBOOT:
                    if (ui_get_power_confirm() != 1) {
                        ui_set_power_confirm(1);
                        ui_set_device_notice("Tap Confirm? again to reboot Kobo");
                        need_partial = true;
                    } else {
                        display_show_banner("Rebooting Kobo...", "Please wait (do not hold Home)", cfg.invert_mode != 0);
                        exit_code = KD_EXIT_REBOOT;
                        running = false;
                    }
                    break;

                case ACT_POWEROFF:
                    if (ui_get_power_confirm() != 2) {
                        ui_set_power_confirm(2);
                        ui_set_device_notice("Tap Confirm? again to power off Kobo");
                        need_partial = true;
                    } else {
                        display_show_banner("Powering Off...", "KoboDash", cfg.invert_mode != 0);
                        exit_code = KD_EXIT_POWEROFF;
                        running = false;
                    }
                    break;

                default:
                    break;
            }

            if (cfg_dirty) (void)config_save(&cfg, cfg_path);
            if (running && (need_full || need_partial)) {
                if (cfg.auto_rotate_min > 0 && current != SCREEN_SETTINGS && need_full) {
                    next_rotate_ms = now_ms + (uint64_t)cfg.auto_rotate_min * 60000ULL;
                }
                KdRefreshMode rm = need_full ? KD_REFRESH_FULL_FLASH : KD_REFRESH_FAST_DU;
                render_any(&canvas, current, &cfg, &tel, &sync, &gal, &lib, rs, &now_tm, rm);
                last_drawn_min = now_tm.tm_min;
                last_drawn_mday = now_tm.tm_mday;
                if (need_full) partial_count = 0;
                else partial_count++;
            }
            continue;
        }

        /* 3. Minute clock tick + offline calendar event triggers + power-state push watcher */
        if (now_tm.tm_min != last_drawn_min) {
            bool trig_hit = dashboard_check_triggers(&bundle, &alerts, now);
            if (trig_hit && alerts.current.active && !kbd.active && current != SCREEN_READER) {
                dashboard_render_alert_overlay(&canvas, &alerts.current, &cfg, KD_REFRESH_FULL_FLASH);
                last_drawn_min = now_tm.tm_min;
                last_drawn_mday = now_tm.tm_mday;
                partial_count = 0;
                continue;
            }
            if (cfg.push_mode == 1) {
                sysinfo_collect(&tel, &cfg);
                reconcile_push_watcher(&cfg, &tel, &sync, push_alerts_path, &push_pipe_fd, &push_pid, false);
            }
            if (!alerts.current.active && lock_mode == LOCK_TOUCH && cfg.sleep_style == SLEEP_STYLE_CLOCK) {
                dashboard_render_sleep_screen(&canvas, &bundle, cfg.sleep_style, sleep_cycle_idx,
                                              LOCK_TOUCH, &cfg, &tel, &sync, &now_tm, KD_REFRESH_FAST_DU);
                last_drawn_min = now_tm.tm_min;
                last_drawn_mday = now_tm.tm_mday;
            } else if (!alerts.current.active && lock_mode == LOCK_NONE && current == SCREEN_CLOCK && cfg.minute_clock) {
                bool day_changed = (now_tm.tm_mday != last_drawn_mday);
                partial_count++;
                if (day_changed || partial_count >= cfg.full_refresh_every) {
                    sysinfo_collect(&tel, &cfg);
                    ui_render_screen(&canvas, SCREEN_CLOCK, &cfg, &tel, &sync, &gal,
                                     &now_tm, KD_REFRESH_FULL_FLASH);
                    partial_count = 0;
                } else {
                    ui_render_clock_digits_only(&canvas, &cfg, &now_tm, KD_REFRESH_FAST_DU);
                }
                last_drawn_min = now_tm.tm_min;
                last_drawn_mday = now_tm.tm_mday;
            } else if (!alerts.current.active && current == SCREEN_GALLERY && cfg.gallery_clock_overlay && cfg.minute_clock) {
                partial_count++;
                ui_render_screen(&canvas, SCREEN_GALLERY, &cfg, &tel, &sync, &gal, &now_tm,
                                 (partial_count >= cfg.full_refresh_every)
                                     ? KD_REFRESH_FULL_FLASH : KD_REFRESH_FAST_DU);
                if (partial_count >= cfg.full_refresh_every) partial_count = 0;
                last_drawn_min = now_tm.tm_min;
                last_drawn_mday = now_tm.tm_mday;
            } else {
                last_drawn_min = now_tm.tm_min;
                last_drawn_mday = now_tm.tm_mday;
            }
        }

        /* 4. Live Status refresh */
        if (current == SCREEN_STATUS && now_ms >= next_status_ms) {
            next_status_ms = now_ms + 60000ULL;
            sysinfo_collect(&tel, &cfg);
            gallery_scan(&gal, &cfg);
            partial_count++;
            KdRefreshMode rm = (partial_count >= cfg.full_refresh_every)
                                   ? KD_REFRESH_FULL_FLASH : KD_REFRESH_GRAY_PARTIAL;
            ui_render_screen(&canvas, SCREEN_STATUS, &cfg, &tel, &sync, &gal, &now_tm, rm);
            if (rm == KD_REFRESH_FULL_FLASH) partial_count = 0;
        }

        /* 5. Scheduled Day/Night Server Sync */
        if (!sync.in_progress && sync.next_sync_time > 0 && now >= sync.next_sync_time) {
            sysinfo_collect(&tel, &cfg);
            sync.next_sync_time = compute_next_sync_time(&cfg, now);
            if (net_start_async_sync(&cfg, &tel, &sync, sync_res_path, &sync_pipe_fd, &sync_pid) == 0) {
                sync.in_progress = true;
            }
        }

        /* 6. Auto-rotate screens (Clock -> Dash Pages 1..N -> Status -> [Gallery] -> Clock) */
        if (can_rotate && next_rotate_ms > 0 && now_ms >= next_rotate_ms) {
            next_rotate_ms = now_ms + (uint64_t)cfg.auto_rotate_min * 60000ULL;
            if (current == SCREEN_GALLERY && gal.count > 1) {
                gallery_advance(&gal, +1);
            }
            ui_step_carousel(current, dash_page, +1, &gal, &bundle, true, &current, &dash_page);
            ui_set_dashboard_bundle(&bundle, dash_page);
            prev_non_settings = current;
            sysinfo_collect(&tel, &cfg);
            render_any(&canvas, current, &cfg, &tel, &sync, &gal, &lib, rs, &now_tm, KD_REFRESH_FULL_FLASH);
            last_drawn_min = now_tm.tm_min;
            last_drawn_mday = now_tm.tm_mday;
            partial_count = 0;
        }
        /* 6b. Touch-Lock periodic sleep screen flair rotation (Today -> Calendar -> Stats) */
        if (lock_mode == LOCK_TOUCH && cfg.sleep_rotate_hrs > 0 && cfg.sleep_style == SLEEP_STYLE_CYCLE &&
            next_sleep_rot_ms > 0 && now_ms >= next_sleep_rot_ms && !alerts.current.active) {
            next_sleep_rot_ms = now_ms + (uint64_t)cfg.sleep_rotate_hrs * 3600000ULL;
            sleep_cycle_idx = (sleep_cycle_idx + 1) % 3;
            sysinfo_collect(&tel, &cfg);
            dashboard_render_sleep_screen(&canvas, &bundle, cfg.sleep_style, sleep_cycle_idx,
                                          LOCK_TOUCH, &cfg, &tel, &sync, &now_tm, KD_REFRESH_FULL_FLASH);
            partial_count = 0;
        }

        /* 7. Auto-Sleep Inactivity Timeout (Unplugged -> True Deep Sleep, Plugged In -> Touch-Lock) */
        if (lock_mode == LOCK_NONE && cfg.auto_sleep_min > 0 && !kbd.active && !alerts.current.active &&
            now_ms >= last_activity_ms + (uint64_t)cfg.auto_sleep_min * 60000ULL) {
            sysinfo_collect(&tel, &cfg);
            if (tel.charger_online) {
                lock_mode = LOCK_TOUCH;
                next_sleep_rot_ms = (cfg.sleep_rotate_hrs > 0 && cfg.sleep_style == SLEEP_STYLE_CYCLE)
                                        ? (now_ms + (uint64_t)cfg.sleep_rotate_hrs * 3600000ULL)
                                        : 0;
                dashboard_render_sleep_screen(&canvas, &bundle, cfg.sleep_style, sleep_cycle_idx,
                                              LOCK_TOUCH, &cfg, &tel, &sync, &now_tm, KD_REFRESH_FULL_FLASH);
                partial_count = 0;
            } else {
                stop_push_watcher(&push_pipe_fd, &push_pid, &sync);
                if (sync_pid > 0) {
                    kill(sync_pid, SIGTERM);
                    (void)waitpid(sync_pid, NULL, 0);
                    sync_pid = -1;
                    if (sync_pipe_fd >= 0) { close(sync_pipe_fd); sync_pipe_fd = -1; }
                    sync.in_progress = false;
                }
                run_deep_sleep_loop(&canvas, input, &cfg, &tel, &sync, &bundle, &alerts, &sleep_cycle_idx);
                now = time(NULL);
                localtime_r(&now, &now_tm);
                now_ms = monotonic_ms();
                last_activity_ms = now_ms;
                if (cfg.auto_rotate_min > 0) {
                    next_rotate_ms = now_ms + (uint64_t)cfg.auto_rotate_min * 60000ULL;
                }
                sysinfo_collect(&tel, &cfg);
                if (alerts.current.active) {
                    dashboard_render_alert_overlay(&canvas, &alerts.current, &cfg, KD_REFRESH_FULL_FLASH);
                } else {
                    render_any(&canvas, current, &cfg, &tel, &sync, &gal, &lib, rs, &now_tm, KD_REFRESH_FULL_FLASH);
                }
                last_drawn_min = now_tm.tm_min;
                last_drawn_mday = now_tm.tm_mday;
                partial_count = 0;
                if (!sync.in_progress &&
                    net_start_async_sync(&cfg, &tel, &sync, sync_res_path, &sync_pipe_fd, &sync_pid) == 0) {
                    sync.in_progress = true;
                }
                reconcile_push_watcher(&cfg, &tel, &sync, push_alerts_path, &push_pipe_fd, &push_pid, false);
            }
        }
    }

    /* Clean exit: always clear boot crash-guard marker */
    unlink(boot_pending_path);
    stop_push_watcher(&push_pipe_fd, &push_pid, &sync);
    if (sync_pipe_fd >= 0) close(sync_pipe_fd);
    if (sync_pid > 0) {
        kill(sync_pid, SIGTERM);
        (void)waitpid(sync_pid, NULL, 0);
    }
    reader_close_book(rs, &cfg);
    reader_destroy(rs);
    dictset_close(dicts);
    font_close();
    input_close(input);
    display_close();
    return exit_code;
}
