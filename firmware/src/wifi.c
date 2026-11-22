#define _GNU_SOURCE
#include "kobodash.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int run_capture(char *const argv[], char *out, size_t out_sz) {
    if (out && out_sz > 0) out[0] = '\0';
    int pfd[2];
    if (pipe(pfd) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[1]);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            if (devnull > 2) close(devnull);
        }
        execv(argv[0], argv);
        _exit(127);
    }

    close(pfd[1]);
    size_t total = 0;
    char drain[256];
    for (;;) {
        ssize_t n;
        if (out && out_sz > 1 && total < out_sz - 1) {
            n = read(pfd[0], out + total, out_sz - 1 - total);
            if (n > 0) total += (size_t)n;
        } else {
            n = read(pfd[0], drain, sizeof(drain));
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
    }
    if (out && out_sz > 0) out[total] = '\0';
    close(pfd[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

static void trim_tail(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

static bool ssid_in_saved(const char *saved_raw, const char *ssid) {
    if (!saved_raw || !saved_raw[0] || !ssid || !ssid[0]) return false;
    size_t slen = strlen(ssid);
    const char *p = saved_raw;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        while (len > 0 && (p[len - 1] == '\r' || p[len - 1] == ' ')) len--;
        if (len == slen && memcmp(p, ssid, slen) == 0) return true;
        if (!nl) break;
        p = nl + 1;
    }
    return false;
}

static int cmp_wifi(const void *a, const void *b) {
    const WifiNetwork *wa = (const WifiNetwork *)a;
    const WifiNetwork *wb = (const WifiNetwork *)b;
    if (wa->connected != wb->connected) return wa->connected ? -1 : 1;
    if (wa->saved != wb->saved && wa->rssi - wb->rssi < 12 && wb->rssi - wa->rssi < 12) {
        return wa->saved ? -1 : 1;
    }
    if (wa->rssi != wb->rssi) return (wb->rssi > wa->rssi) ? 1 : -1;
    return strcmp(wa->ssid, wb->ssid);
}

int wifi_parse_scan_results(const char *scan_raw, const char *saved_raw,
                            const char *active_ssid, WifiState *ws) {
    if (!ws) return 0;
    ws->count = 0;
    ws->page = 0;

    if (scan_raw && scan_raw[0]) {
        const char *p = scan_raw;
        while (*p) {
            const char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            char line[256];
            if (len >= sizeof(line)) len = sizeof(line) - 1;
            memcpy(line, p, len);
            line[len] = '\0';
            trim_tail(line);
            p = nl ? (nl + 1) : (p + strlen(p));

            if (!line[0] || strncmp(line, "bssid", 5) == 0 || strncmp(line, "Selected", 8) == 0) {
                continue;
            }

            /* Format: bssid \t freq \t signal \t flags \t ssid */
            char *t1 = strchr(line, '\t');
            if (!t1) continue;
            char *t2 = strchr(t1 + 1, '\t');
            if (!t2) continue;
            char *t3 = strchr(t2 + 1, '\t');
            if (!t3) continue;
            char *t4 = strchr(t3 + 1, '\t');
            if (!t4) continue;

            *t3 = '\0';
            *t4 = '\0';
            int rssi = atoi(t2 + 1);
            if (rssi > 0 && rssi <= 120) rssi = -rssi;
            if (rssi > 0) rssi -= 256;
            const char *flags = t3 + 1;
            const char *ssid = t4 + 1;
            while (*ssid == ' ' || *ssid == '\t') ssid++;
            if (!ssid[0] || strncmp(ssid, "\\x00", 4) == 0) continue;

            bool secured = (strstr(flags, "WPA") != NULL ||
                            strstr(flags, "WEP") != NULL ||
                            strstr(flags, "RSN") != NULL);

            /* Deduplicate by SSID, keeping strongest RSSI */
            int existing = -1;
            for (int i = 0; i < ws->count; i++) {
                if (strcmp(ws->nets[i].ssid, ssid) == 0) {
                    existing = i;
                    break;
                }
            }
            if (existing >= 0) {
                if (rssi > ws->nets[existing].rssi) ws->nets[existing].rssi = rssi;
                if (secured) ws->nets[existing].secured = true;
                continue;
            }
            if (ws->count >= KD_MAX_WIFI_NETS) continue;

            WifiNetwork *n = &ws->nets[ws->count++];
            memset(n, 0, sizeof(*n));
            snprintf(n->ssid, sizeof(n->ssid), "%s", ssid);
            n->rssi = rssi;
            n->secured = secured;
            n->saved = ssid_in_saved(saved_raw, ssid);
            n->connected = (active_ssid && active_ssid[0] && strcmp(active_ssid, ssid) == 0);
        }
    }

    /* Also append any saved networks not currently seen in scan so user can forget/connect them */
    if (saved_raw && saved_raw[0]) {
        const char *p = saved_raw;
        while (*p && ws->count < KD_MAX_WIFI_NETS) {
            const char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            char ssid[64];
            if (len >= sizeof(ssid)) len = sizeof(ssid) - 1;
            memcpy(ssid, p, len);
            ssid[len] = '\0';
            trim_tail(ssid);
            p = nl ? (nl + 1) : (p + strlen(p));
            if (!ssid[0]) continue;

            bool dup = false;
            for (int i = 0; i < ws->count; i++) {
                if (strcmp(ws->nets[i].ssid, ssid) == 0) { dup = true; break; }
            }
            if (dup) continue;
            WifiNetwork *n = &ws->nets[ws->count++];
            memset(n, 0, sizeof(*n));
            snprintf(n->ssid, sizeof(n->ssid), "%s", ssid);
            n->rssi = -99;
            n->secured = true;
            n->saved = true;
            n->connected = (active_ssid && active_ssid[0] && strcmp(active_ssid, ssid) == 0);
        }
    }

    if (ws->count > 1) {
        qsort(ws->nets, (size_t)ws->count, sizeof(WifiNetwork), cmp_wifi);
    }
    return ws->count;
}

static void build_ctl_path(const Config *cfg, char *out, size_t sz) {
    const char *base = (cfg && cfg->base_dir[0]) ? cfg->base_dir : "/mnt/onboard/.adds/kobodash";
    snprintf(out, sz, "%s/wifi-ctl.sh", base);
}

static void extract_status_line(const char *raw, char *out, size_t out_sz, const char *fallback) {
    if (!out || out_sz == 0) return;
    if (!raw || !raw[0]) {
        snprintf(out, out_sz, "%s", fallback);
        return;
    }
    /* Take last non-empty line */
    const char *best = raw;
    const char *p = raw;
    while (*p) {
        while (*p == '\r' || *p == '\n') p++;
        if (!*p) break;
        best = p;
        while (*p && *p != '\n') p++;
    }
    size_t n = 0;
    while (best[n] && best[n] != '\n' && best[n] != '\r' && n + 1 < out_sz) {
        out[n] = best[n];
        n++;
    }
    out[n] = '\0';
    if (strncmp(out, "OK ", 3) == 0) memmove(out, out + 3, strlen(out + 3) + 1);
    else if (strncmp(out, "ERR ", 4) == 0) memmove(out, out + 4, strlen(out + 4) + 1);
    if (!out[0]) snprintf(out, out_sz, "%s", fallback);
}

int wifi_set_power(const Config *cfg, bool on, char *status_out, size_t status_sz) {
    char script[320];
    build_ctl_path(cfg, script, sizeof(script));
    if (access(script, R_OK) != 0) {
        if (status_out) snprintf(status_out, status_sz, "wifi-ctl.sh not found");
        return -1;
    }
    char *argv[] = { "/bin/sh", script, (char *)(on ? "on" : "off"), NULL };
    char raw[512];
    int rc = run_capture(argv, raw, sizeof(raw));
    extract_status_line(raw, status_out, status_sz, on ? "Wi-Fi ON" : "Wi-Fi OFF");
    return rc == 0 ? 0 : -1;
}

int wifi_hard_reset(const Config *cfg, char *status_out, size_t status_sz) {
    char script[320];
    build_ctl_path(cfg, script, sizeof(script));
    if (access(script, R_OK) != 0) {
        if (status_out) snprintf(status_out, status_sz, "wifi-ctl.sh not found");
        return -1;
    }
    char *argv[] = { "/bin/sh", script, "reset", NULL };
    char raw[512];
    int rc = run_capture(argv, raw, sizeof(raw));
    extract_status_line(raw, status_out, status_sz, "Wi-Fi chip reset");
    return rc == 0 ? 0 : -1;
}

int wifi_scan(const Config *cfg, const Telemetry *tel, WifiState *ws) {
    if (!ws) return -1;
    char script[320];
    build_ctl_path(cfg, script, sizeof(script));
    if (access(script, R_OK) != 0) {
        snprintf(ws->status_msg, sizeof(ws->status_msg), "wifi-ctl.sh not found");
        return -1;
    }
    char scan_raw[8192];
    char saved_raw[2048];
    char *argv_scan[] = { "/bin/sh", script, "scan", NULL };
    char *argv_saved[] = { "/bin/sh", script, "saved", NULL };
    (void)run_capture(argv_scan, scan_raw, sizeof(scan_raw));
    (void)run_capture(argv_saved, saved_raw, sizeof(saved_raw));

    const char *active = (tel && tel->wifi_up) ? tel->wifi_ssid : "";
    int n = wifi_parse_scan_results(scan_raw, saved_raw, active, ws);
    if (n > 0) {
        snprintf(ws->status_msg, sizeof(ws->status_msg), "Found %d network%s", n, n == 1 ? "" : "s");
    } else {
        snprintf(ws->status_msg, sizeof(ws->status_msg), "No Wi-Fi networks found");
    }
    return n;
}

int wifi_connect(const Config *cfg, const char *ssid, const char *psk,
                 char *status_out, size_t status_sz) {
    if (!ssid || !ssid[0]) return -1;
    char script[320];
    build_ctl_path(cfg, script, sizeof(script));
    if (access(script, R_OK) != 0) {
        if (status_out) snprintf(status_out, status_sz, "wifi-ctl.sh not found");
        return -1;
    }
    char *argv[] = {
        "/bin/sh", script, "connect",
        (char *)ssid, (char *)(psk ? psk : ""), NULL
    };
    char raw[512];
    int rc = run_capture(argv, raw, sizeof(raw));
    extract_status_line(raw, status_out, status_sz,
                        rc == 0 ? "Wi-Fi connected" : "Connection failed");
    return rc == 0 ? 0 : -1;
}

int wifi_forget(const Config *cfg, const char *ssid, char *status_out, size_t status_sz) {
    if (!ssid || !ssid[0]) return -1;
    char script[320];
    build_ctl_path(cfg, script, sizeof(script));
    if (access(script, R_OK) != 0) {
        if (status_out) snprintf(status_out, status_sz, "wifi-ctl.sh not found");
        return -1;
    }
    char *argv[] = { "/bin/sh", script, "forget", (char *)ssid, NULL };
    char raw[256];
    int rc = run_capture(argv, raw, sizeof(raw));
    extract_status_line(raw, status_out, status_sz, "Network removed");
    return rc == 0 ? 0 : -1;
}

int device_list_governors(char out[][24], int max_out) {
    if (!out || max_out <= 0) return 0;
    FILE *fp = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors", "r");
    int count = 0;
    if (fp) {
        char buf[256];
        if (fgets(buf, sizeof(buf), fp)) {
            char *p = buf;
            while (*p && count < max_out) {
                while (*p && isspace((unsigned char)*p)) p++;
                if (!*p) break;
                char *start = p;
                while (*p && !isspace((unsigned char)*p)) p++;
                size_t len = (size_t)(p - start);
                if (len > 0 && len < 24) {
                    memcpy(out[count], start, len);
                    out[count][len] = '\0';
                    count++;
                }
            }
        }
        fclose(fp);
    }
    if (count == 0) {
        const char *defaults[] = { "conservative", "ondemand", "performance" };
        for (int i = 0; i < 3 && count < max_out; i++) {
            snprintf(out[count++], 24, "%s", defaults[i]);
        }
    }
    return count;
}

int device_set_governor(const char *gov) {
    if (!gov || !gov[0]) return -1;
    FILE *fp = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "w");
    if (!fp) return -1;
    fprintf(fp, "%s\n", gov);
    int rc = (fclose(fp) == 0) ? 0 : -1;
    return rc;
}

int device_toggle_sd(const Config *cfg, bool mount_it, char *status_out, size_t status_sz) {
    char script[320];
    build_ctl_path(cfg, script, sizeof(script));
    if (access(script, R_OK) != 0) {
        if (status_out) snprintf(status_out, status_sz, "wifi-ctl.sh not found");
        return -1;
    }
    char *argv[] = { "/bin/sh", script, (char *)(mount_it ? "sd-mount" : "sd-unmount"), NULL };
    char raw[256];
    int rc = run_capture(argv, raw, sizeof(raw));
    extract_status_line(raw, status_out, status_sz, mount_it ? "SD mounted" : "SD unmounted");
    return rc == 0 ? 0 : -1;
}
