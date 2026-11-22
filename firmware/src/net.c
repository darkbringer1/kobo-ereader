#define _GNU_SOURCE
#include "kobodash.h"
#include "kd_tls.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define KD_MAX_BODY_BYTES (4 * 1024 * 1024) /* 4 MB cap protects 256 MB Kobo RAM */
#define KD_MAX_JSON_BYTES (128 * 1024)
#define KD_MAX_REDIRECTS  4

static int month_from_abbr(const char *m) {
    static const char *names[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    for (int i = 0; i < 12; i++) {
        if (strncasecmp(m, names[i], 3) == 0) return i;
    }
    return -1;
}

bool net_parse_http_date(const char *hdr, time_t *out_utc) {
    if (!hdr || !out_utc) return false;
    const char *comma = strchr(hdr, ',');
    const char *p = comma ? comma + 1 : hdr;
    while (*p && isspace((unsigned char)*p)) p++;

    int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
    char mon[8] = {0};
    if (sscanf(p, "%d %3s %d %d:%d:%d", &day, mon, &year, &hh, &mm, &ss) != 6) {
        return false;
    }
    int mo = month_from_abbr(mon);
    if (mo < 0 || year < 2020 || day < 1 || day > 31) return false;

    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = year - 1900;
    tmv.tm_mon = mo;
    tmv.tm_mday = day;
    tmv.tm_hour = hh;
    tmv.tm_min = mm;
    tmv.tm_sec = ss;
    time_t utc = timegm(&tmv);
    if (utc <= 0) return false;
    *out_utc = utc;
    return true;
}

bool net_extract_json_string(const char *json, const char *key, char *out, size_t out_sz) {
    if (!json || !key || !out || out_sz == 0) return false;
    char needle[96];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p && (isspace((unsigned char)*p) || *p == ':')) p++;
    if (*p != '"') return false;
    p++;

    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_sz) {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'n' || *p == 't') out[i++] = ' ';
            else out[i++] = *p;
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return (*p == '"');
}

bool net_parse_dashboard_json(const char *json, SyncState *st) {
    if (!json || !st) return false;
    char title[128] = {0};
    char sub[128] = {0};
    net_extract_json_string(json, "title", title, sizeof(title));
    net_extract_json_string(json, "subtitle", sub, sizeof(sub));

    st->json_line_count = 0;
    const char *lines_key = strstr(json, "\"lines\"");
    if (lines_key) {
        const char *arr = strchr(lines_key, '[');
        if (arr) {
            const char *p = arr + 1;
            while (*p && *p != ']' && st->json_line_count < KD_MAX_JSON_LINES) {
                while (*p && *p != '"' && *p != ']') p++;
                if (*p != '"') break;
                p++;
                size_t i = 0;
                char *dst = st->json_lines[st->json_line_count];
                while (*p && *p != '"' && i + 1 < sizeof(st->json_lines[0])) {
                    if (*p == '\\' && p[1]) {
                        p++;
                        dst[i++] = (*p == 'n') ? ' ' : *p;
                    } else {
                        dst[i++] = *p;
                    }
                    p++;
                }
                dst[i] = '\0';
                if (*p == '"') p++;
                st->json_line_count++;
            }
        }
    }

    if (!title[0] && st->json_line_count == 0) return false;
    snprintf(st->json_title, sizeof(st->json_title), "%s", title[0] ? title : "Dashboard");
    snprintf(st->json_subtitle, sizeof(st->json_subtitle), "%s", sub);
    st->is_json_dashboard = true;
    return true;
}

bool net_sntp_sync(const char *server, time_t *out_utc) {
    if (!server || !server[0] || !out_utc) return false;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(server, "123", &hints, &res) != 0 || !res) {
        return false;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return false;
    }

    struct timeval tv = { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    uint8_t pkt[48];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x1B; /* LI=0, VN=3, Mode=3 */

    bool ok = false;
    if (sendto(fd, pkt, sizeof(pkt), 0, res->ai_addr, res->ai_addrlen) == (ssize_t)sizeof(pkt)) {
        ssize_t r = recv(fd, pkt, sizeof(pkt), 0);
        if (r >= 48) {
            uint32_t secs = ((uint32_t)pkt[40] << 24) |
                            ((uint32_t)pkt[41] << 16) |
                            ((uint32_t)pkt[42] << 8)  |
                            ((uint32_t)pkt[43]);
            if (secs > 2208988800u + 1700000000u) {
                *out_utc = (time_t)(secs - 2208988800u);
                ok = true;
            }
        }
    }

    close(fd);
    freeaddrinfo(res);
    return ok;
}

static int run_argv_quiet(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        execv(argv[0], argv);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

static bool apply_system_time(time_t utc) {
    if (utc < 1700000000) return false;
    time_t cur = time(NULL);
    if (llabs((long long)utc - (long long)cur) <= 2) {
        return true;
    }
#ifdef __linux__
    struct timeval tv = { utc, 0 };
    if (settimeofday(&tv, NULL) == 0) {
        if (access("/mnt/onboard/.adds/dashboard/bin/busybox", X_OK) == 0) {
            char *argv[] = { "/mnt/onboard/.adds/dashboard/bin/busybox", "hwclock", "-w", "-u", NULL };
            (void)run_argv_quiet(argv);
        } else if (access("/sbin/hwclock", X_OK) == 0) {
            char *argv[] = { "/sbin/hwclock", "-w", "-u", NULL };
            (void)run_argv_quiet(argv);
        }
        return true;
    }
#endif
    return false;
}

/* ============================================================================
 * Unified Pure-C HTTP/1.1 + Native HTTPS (mbedTLS TLS 1.2/1.3) Stream Client
 * ============================================================================ */

typedef struct {
    int status_code;
    bool chunked;
    long content_length;
    time_t date_utc;
    char etag[128];
    char location[256];
    char err[96];
} HttpMeta;

typedef struct {
    bool is_tls;
    int fd;
    KdTlsConn *tls;
} NetStream;

static bool parse_url(const char *url, bool *is_https,
                      char *host, size_t host_sz,
                      char *port, size_t port_sz,
                      char *path, size_t path_sz) {
    if (!url) return false;
    const char *p = NULL;
    if (strncasecmp(url, "https://", 8) == 0) {
        *is_https = true;
        p = url + 8;
    } else if (strncasecmp(url, "http://", 7) == 0) {
        *is_https = false;
        p = url + 7;
    } else {
        return false;
    }

    const char *slash = strchr(p, '/');
    const char *hp_end = slash ? slash : (p + strlen(p));
    if (hp_end <= p) return false;

    const char *colon = NULL;
    if (*p == '[') {
        const char *rb = memchr(p, ']', (size_t)(hp_end - p));
        if (!rb) return false;
        size_t hlen = (size_t)(rb - (p + 1));
        if (hlen == 0 || hlen >= host_sz) return false;
        memcpy(host, p + 1, hlen);
        host[hlen] = '\0';
        if (rb + 1 < hp_end && rb[1] == ':') colon = rb + 1;
    } else {
        colon = memchr(p, ':', (size_t)(hp_end - p));
        size_t hlen = colon ? (size_t)(colon - p) : (size_t)(hp_end - p);
        if (hlen == 0 || hlen >= host_sz) return false;
        memcpy(host, p, hlen);
        host[hlen] = '\0';
    }

    if (colon && colon + 1 < hp_end) {
        size_t plen = (size_t)(hp_end - (colon + 1));
        if (plen == 0 || plen >= port_sz) return false;
        memcpy(port, colon + 1, plen);
        port[plen] = '\0';
    } else {
        snprintf(port, port_sz, "%s", *is_https ? "443" : "80");
    }

    if (slash) {
        snprintf(path, path_sz, "%s", slash);
    } else {
        snprintf(path, path_sz, "/");
    }
    return true;
}

static bool stream_connect(NetStream *ns, bool is_https, const char *host, const char *port,
                           const Config *cfg, int timeout_sec, char *err, size_t err_sz) {
    memset(ns, 0, sizeof(*ns));
    ns->is_tls = is_https;
    ns->fd = -1;

    if (is_https) {
        char ca_path[320] = {0};
        if (cfg && cfg->base_dir[0]) {
            snprintf(ca_path, sizeof(ca_path), "%s/certs/cacert.pem", cfg->base_dir);
        }
        ns->tls = kd_tls_connect(host, port, ca_path[0] ? ca_path : NULL, timeout_sec, err, err_sz);
        return (ns->tls != NULL);
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) {
        if (err && err_sz > 0) snprintf(err, err_sz, "DNS lookup failed");
        return false;
    }

    int fd = -1;
    for (struct addrinfo *rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        struct timeval tv = { timeout_sec > 0 ? timeout_sec : 10, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        if (err && err_sz > 0) snprintf(err, err_sz, "Server unreachable");
        return false;
    }
    ns->fd = fd;
    return true;
}

static bool stream_write_all(NetStream *ns, const void *buf, size_t len) {
    if (ns->is_tls) {
        return kd_tls_write(ns->tls, buf, len) == (ssize_t)len;
    }
    const unsigned char *p = buf;
    while (len > 0) {
        ssize_t n = send(ns->fd, p, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += (size_t)n;
        len -= (size_t)n;
    }
    return true;
}

static ssize_t stream_read(NetStream *ns, void *buf, size_t len, int timeout_ms) {
    if (ns->is_tls) {
        return kd_tls_read(ns->tls, buf, len, timeout_ms);
    }
    if (timeout_ms > 0) {
        struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        setsockopt(ns->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    for (;;) {
        ssize_t n = recv(ns->fd, buf, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return -2;
            return -1;
        }
        return n;
    }
}

static void stream_close(NetStream *ns) {
    if (ns->is_tls) {
        if (ns->tls) { kd_tls_close(ns->tls); ns->tls = NULL; }
    } else {
        if (ns->fd >= 0) { close(ns->fd); ns->fd = -1; }
    }
}

static void parse_headers(char *hdr_block, HttpMeta *meta) {
    char *line = hdr_block;
    bool first = true;
    while (line && *line) {
        char *eol = strstr(line, "\r\n");
        if (eol) *eol = '\0';
        if (first) {
            first = false;
            (void)sscanf(line, "HTTP/%*d.%*d %d", &meta->status_code);
        } else {
            char *colon = strchr(line, ':');
            if (colon) {
                *colon = '\0';
                char *val = colon + 1;
                while (*val == ' ' || *val == '\t') val++;
                if (strcasecmp(line, "Content-Length") == 0) {
                    meta->content_length = atol(val);
                } else if (strcasecmp(line, "Transfer-Encoding") == 0 &&
                           strstr(val, "chunked") != NULL) {
                    meta->chunked = true;
                } else if (strcasecmp(line, "Date") == 0) {
                    (void)net_parse_http_date(val, &meta->date_utc);
                } else if (strcasecmp(line, "ETag") == 0) {
                    snprintf(meta->etag, sizeof(meta->etag), "%s", val);
                } else if (strcasecmp(line, "Location") == 0) {
                    snprintf(meta->location, sizeof(meta->location), "%s", val);
                }
            }
        }
        line = eol ? (eol + 2) : NULL;
    }
}

static bool decode_chunked_to_file(const unsigned char *raw, size_t raw_len, FILE *out, size_t *out_bytes) {
    size_t pos = 0;
    size_t written = 0;
    while (pos < raw_len) {
        size_t eol = pos;
        while (eol + 1 < raw_len && !(raw[eol] == '\r' && raw[eol + 1] == '\n')) eol++;
        if (eol + 1 >= raw_len) return false;

        char hex[32];
        size_t hlen = eol - pos;
        if (hlen >= sizeof(hex)) hlen = sizeof(hex) - 1;
        memcpy(hex, raw + pos, hlen);
        hex[hlen] = '\0';
        unsigned long chunk_sz = strtoul(hex, NULL, 16);
        pos = eol + 2;
        if (chunk_sz == 0) {
            *out_bytes = written;
            return true;
        }
        if (pos + chunk_sz > raw_len || written + chunk_sz > KD_MAX_BODY_BYTES) return false;
        if (fwrite(raw + pos, 1, chunk_sz, out) != chunk_sz) return false;
        written += chunk_sz;
        pos += chunk_sz;
        if (pos + 2 <= raw_len && raw[pos] == '\r' && raw[pos + 1] == '\n') pos += 2;
    }
    *out_bytes = written;
    return written > 0;
}

static int http_get_native(const char *url, const Config *cfg, const Telemetry *tel,
                           const char *if_none_match, const char *out_path, HttpMeta *meta) {
    memset(meta, 0, sizeof(*meta));
    meta->content_length = -1;

    bool is_https = false;
    char host[128], port[16], path[256];
    if (!parse_url(url, &is_https, host, sizeof(host), port, sizeof(port), path, sizeof(path))) {
        snprintf(meta->err, sizeof(meta->err), "Invalid URL (use http:// or https://)");
        return -2;
    }

    NetStream ns;
    if (!stream_connect(&ns, is_https, host, port, cfg, 12, meta->err, sizeof(meta->err))) {
        return -1;
    }

    char req[1024];
    int bat = tel ? tel->bat_pct : -1;
    int chg = tel ? tel->charger_online : 0;
    int rssi = tel ? tel->wifi_rssi : 0;
    bool default_port = (!is_https && strcmp(port, "80") == 0) ||
                        (is_https && strcmp(port, "443") == 0);

    int off = snprintf(req, sizeof(req),
                       "GET %s HTTP/1.1\r\n"
                       "Host: %s%s%s\r\n"
                       "User-Agent: KoboDash/%s\r\n"
                       "Accept: application/json, image/png, image/jpeg, image/bmp, */*\r\n"
                       "Accept-Encoding: identity\r\n"
                       "X-Kobo-Battery: %d\r\n"
                       "X-Kobo-Charging: %d\r\n"
                       "X-Kobo-RSSI: %d\r\n"
                       "X-Kobo-Width: %d\r\n"
                       "X-Kobo-Height: %d\r\n"
                       "Connection: close\r\n",
                       path,
                       host, default_port ? "" : ":", default_port ? "" : port,
                       KD_VERSION, bat, chg, rssi, KD_WIDTH, KD_HEIGHT);
    if (cfg && cfg->server_token[0] && off > 0 && (size_t)off < sizeof(req)) {
        off += snprintf(req + off, sizeof(req) - (size_t)off,
                        "Authorization: Bearer %s\r\n", cfg->server_token);
    }
    if (if_none_match && if_none_match[0] && off > 0 && (size_t)off < sizeof(req)) {
        off += snprintf(req + off, sizeof(req) - (size_t)off,
                        "If-None-Match: %s\r\n", if_none_match);
    }
    if (off > 0 && (size_t)off + 3 < sizeof(req)) {
        off += snprintf(req + off, sizeof(req) - (size_t)off, "\r\n");
    }

    if (!stream_write_all(&ns, req, (size_t)off)) {
        snprintf(meta->err, sizeof(meta->err), "Write failed");
        stream_close(&ns);
        return -1;
    }

    /* Read headers (up to 16 KB) */
    char hdr_buf[16384];
    size_t hdr_len = 0;
    char *body_start = NULL;
    while (hdr_len < sizeof(hdr_buf) - 1) {
        ssize_t n = stream_read(&ns, hdr_buf + hdr_len, sizeof(hdr_buf) - 1 - hdr_len, 12000);
        if (n <= 0) break;
        hdr_len += (size_t)n;
        hdr_buf[hdr_len] = '\0';
        body_start = strstr(hdr_buf, "\r\n\r\n");
        if (body_start) break;
    }
    if (!body_start) {
        snprintf(meta->err, sizeof(meta->err), "Empty server response");
        stream_close(&ns);
        return -1;
    }

    *body_start = '\0';
    parse_headers(hdr_buf, meta);

    if (meta->status_code == 304 ||
        (meta->status_code >= 300 && meta->status_code < 400 && meta->location[0])) {
        stream_close(&ns);
        return 0;
    }
    if (meta->status_code != 200) {
        stream_close(&ns);
        return -1;
    }

    const unsigned char *initial_body = (const unsigned char *)(body_start + 4);
    size_t initial_len = hdr_len - (size_t)((body_start + 4) - hdr_buf);

    FILE *out = fopen(out_path, "wb");
    if (!out) {
        stream_close(&ns);
        return -1;
    }

    bool ok = true;
    if (meta->chunked) {
        size_t cap = initial_len + 16384;
        unsigned char *raw = malloc(cap);
        if (!raw) {
            fclose(out);
            stream_close(&ns);
            return -1;
        }
        size_t raw_len = initial_len;
        if (initial_len > 0) memcpy(raw, initial_body, initial_len);
        for (;;) {
            if (raw_len >= KD_MAX_BODY_BYTES) { ok = false; break; }
            if (raw_len + 8192 > cap) {
                cap *= 2;
                if (cap > KD_MAX_BODY_BYTES + 16384) cap = KD_MAX_BODY_BYTES + 16384;
                unsigned char *nr = realloc(raw, cap);
                if (!nr) { ok = false; break; }
                raw = nr;
            }
            ssize_t n = stream_read(&ns, raw + raw_len, cap - raw_len, 12000);
            if (n < 0) { ok = false; break; }
            if (n == 0) break;
            raw_len += (size_t)n;
        }
        size_t decoded = 0;
        if (ok) ok = decode_chunked_to_file(raw, raw_len, out, &decoded);
        free(raw);
    } else {
        size_t total = 0;
        if (initial_len > 0) {
            if (fwrite(initial_body, 1, initial_len, out) != initial_len) ok = false;
            total += initial_len;
        }
        unsigned char buf[8192];
        while (ok) {
            if (meta->content_length >= 0 && (long)total >= meta->content_length) break;
            if (total >= KD_MAX_BODY_BYTES) { ok = false; break; }
            ssize_t n = stream_read(&ns, buf, sizeof(buf), 12000);
            if (n < 0) { ok = false; break; }
            if (n == 0) break;
            if (fwrite(buf, 1, (size_t)n, out) != (size_t)n) { ok = false; break; }
            total += (size_t)n;
        }
        if (meta->content_length >= 0 && (long)total < meta->content_length) ok = false;
    }

    fclose(out);
    stream_close(&ns);
    return ok ? 0 : -1;
}

static bool copy_file_atomic(const char *src_path, const char *dst_path) {
    char tmp_dst[320];
    snprintf(tmp_dst, sizeof(tmp_dst), "%s.tmp", dst_path);
    FILE *src = fopen(src_path, "rb");
    FILE *dst = fopen(tmp_dst, "wb");
    if (!src || !dst) {
        if (src) fclose(src);
        if (dst) fclose(dst);
        return false;
    }
    unsigned char buf[8192];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, n, dst) != n) { ok = false; break; }
    }
    fclose(src);
    fflush(dst);
    fsync(fileno(dst));
    if (fclose(dst) != 0) ok = false;
    if (!ok || rename(tmp_dst, dst_path) != 0) {
        unlink(tmp_dst);
        return false;
    }
    return true;
}

static bool is_valid_image_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    unsigned char magic[8] = {0};
    size_t n = fread(magic, 1, sizeof(magic), fp);
    fclose(fp);
    if (n < 4) return false;
    if (magic[0] == 0x89 && magic[1] == 'P' && magic[2] == 'N' && magic[3] == 'G') return true;
    if (magic[0] == 0xFF && magic[1] == 0xD8 && magic[2] == 0xFF) return true;
    if (magic[0] == 'B' && magic[1] == 'M') return true;
    if (magic[0] == 'G' && magic[1] == 'I' && magic[2] == 'F') return true;
    return false;
}

static char *read_json_file_alloc(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz <= 2 || sz > KD_MAX_JSON_BYTES) { fclose(fp); return NULL; }
    rewind(fp);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[n] = '\0';
    const char *p = buf;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '{') {
        free(buf);
        return NULL;
    }
    return buf;
}

void net_ensure_wifi(const Config *cfg) {
    char script[300];
    snprintf(script, sizeof(script), "%s/wifi-ensure.sh", cfg->base_dir);
    if (access(script, R_OK) == 0) {
        char *argv[] = { "/bin/sh", script, NULL };
        (void)run_argv_quiet(argv);
    }
}

int net_sync_blocking(const Config *cfg, const Telemetry *tel, SyncState *st) {
    st->content_changed = false;

    /* 1. Built-in UDP SNTP clock sync (only when ntp_server is configured) */
    time_t utc = 0;
    if (cfg->ntp_server[0]) {
        if (net_sntp_sync(cfg->ntp_server, &utc)) {
            if (apply_system_time(utc)) st->time_synced = true;
        } else if (strcmp(cfg->ntp_server, "pool.ntp.org") != 0 &&
                   net_sntp_sync("pool.ntp.org", &utc)) {
            if (apply_system_time(utc)) st->time_synced = true;
        }
    }

    st->last_sync_time = time(NULL);

    if (!cfg->server_url[0]) {
        st->last_ok = st->time_synced ? 1 : 0;
        snprintf(st->last_status, sizeof(st->last_status),
                 st->time_synced ? "Clock synced (set server_url)" : "No server_url configured");
        return st->time_synced ? 0 : -1;
    }

    char cache_dir[280];
    snprintf(cache_dir, sizeof(cache_dir), "%s/cache", cfg->base_dir);
    mkdir(cache_dir, 0755);

    if (!st->cache_bundle_path[0]) {
        snprintf(st->cache_bundle_path, sizeof(st->cache_bundle_path),
                 "%s/cache/bundle.json", cfg->base_dir);
    }

    const char *tmp_body = "/tmp/kobodash-body.tmp";
    unlink(tmp_body);

    char cur_url[320];
    snprintf(cur_url, sizeof(cur_url), "%s", cfg->server_url);

    HttpMeta meta;
    int rc = -1;
    for (int hop = 0; hop <= KD_MAX_REDIRECTS; hop++) {
        rc = http_get_native(cur_url, cfg, tel, st->last_etag, tmp_body, &meta);
        if (rc != 0) break;
        if (meta.date_utc > 0 && apply_system_time(meta.date_utc)) {
            st->time_synced = true;
        }
        if (meta.status_code == 304) {
            st->last_sync_time = time(NULL);
            st->last_ok = 1;
            st->content_changed = false;
            snprintf(st->last_status, sizeof(st->last_status), "Up to date (304)");
            unlink(tmp_body);
            return 0;
        }
        if (meta.status_code >= 300 && meta.status_code < 400 && meta.location[0]) {
            if (strncasecmp(meta.location, "http://", 7) == 0 ||
                strncasecmp(meta.location, "https://", 8) == 0) {
                snprintf(cur_url, sizeof(cur_url), "%s", meta.location);
            } else if (meta.location[0] == '/') {
                bool is_https = false;
                char host[128], port[16], old_path[256];
                if (!parse_url(cur_url, &is_https, host, sizeof(host), port, sizeof(port),
                               old_path, sizeof(old_path))) {
                    rc = -1;
                    break;
                }
                snprintf(cur_url, sizeof(cur_url), "%s://%s:%s%s",
                         is_https ? "https" : "http", host, port, meta.location);
            } else {
                rc = -1;
                break;
            }
            continue;
        }
        break;
    }

    st->last_sync_time = time(NULL);
    if (rc != 0 || meta.status_code != 200) {
        unlink(tmp_body);
        st->last_ok = -1;
        if (meta.status_code > 0) {
            snprintf(st->last_status, sizeof(st->last_status), "HTTP %d from server", meta.status_code);
        } else if (meta.err[0]) {
            snprintf(st->last_status, sizeof(st->last_status), "%.90s", meta.err);
        } else {
            snprintf(st->last_status, sizeof(st->last_status), "Server unreachable");
        }
        return -1;
    }

    if (meta.etag[0]) {
        snprintf(st->last_etag, sizeof(st->last_etag), "%s", meta.etag);
    }

    /* Case A: Image response (PNG / JPEG / BMP / GIF) */
    if (is_valid_image_file(tmp_body)) {
        if (!copy_file_atomic(tmp_body, st->cache_image_path)) {
            unlink(tmp_body);
            st->last_ok = -1;
            snprintf(st->last_status, sizeof(st->last_status), "Failed to save image");
            return -1;
        }
        unlink(tmp_body);
        st->has_cached_image = true;
        st->has_bundle_json = false;
        st->is_json_dashboard = false;
        st->content_changed = true;
        st->last_ok = 1;
        snprintf(st->last_status, sizeof(st->last_status), "Image updated");
        return 0;
    }

    /* Case B: Multi-page KoboDash Bundle JSON OR Legacy single-page JSON */
    char *jbuf = read_json_file_alloc(tmp_body);
    if (jbuf) {
        DashboardBundle probe;
        bool bundle_ok = dashboard_parse_bundle(jbuf, &probe);
        if (bundle_ok) {
            if (copy_file_atomic(tmp_body, st->cache_bundle_path)) {
                st->has_bundle_json = true;
            }
            /* Also populate legacy fields if single-page JSON for backward compat */
            (void)net_parse_dashboard_json(jbuf, st);
            st->is_json_dashboard = true;
            st->content_changed = true;
            st->last_ok = 1;
            snprintf(st->last_status, sizeof(st->last_status),
                     "Synced %d page%s, %d alert%s",
                     probe.page_count, probe.page_count == 1 ? "" : "s",
                     probe.alert_count, probe.alert_count == 1 ? "" : "s");
            free(jbuf);
            unlink(tmp_body);
            return 0;
        }
        free(jbuf);
    }

    unlink(tmp_body);
    st->last_ok = -1;
    snprintf(st->last_status, sizeof(st->last_status), "Expected PNG/JPG/BMP or JSON");
    return -1;
}

static void write_sync_result(const char *path, const SyncState *st) {
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *fp = fopen(tmp, "wb");
    if (!fp) return;
    fwrite(st, sizeof(*st), 1, fp);
    fclose(fp);
    rename(tmp, path);
}

bool net_read_sync_result(const char *result_path, SyncState *st) {
    FILE *fp = fopen(result_path, "rb");
    if (!fp) return false;
    SyncState tmp;
    bool ok = (fread(&tmp, sizeof(tmp), 1, fp) == 1);
    fclose(fp);
    if (ok) {
        time_t saved_next = st->next_sync_time;
        bool saved_push = st->push_connected;
        *st = tmp;
        st->in_progress = false;
        st->next_sync_time = saved_next;
        st->push_connected = saved_push;
    }
    return ok;
}

int net_start_async_sync(const Config *cfg, const Telemetry *tel, const SyncState *prev_st,
                         const char *result_path, int *out_pipe_fd, pid_t *out_pid) {
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
        SyncState child_st;
        if (prev_st) {
            child_st = *prev_st;
        } else {
            memset(&child_st, 0, sizeof(child_st));
        }
        snprintf(child_st.cache_image_path, sizeof(child_st.cache_image_path),
                 "%s/cache/dashboard.img", cfg->base_dir);
        snprintf(child_st.cache_bundle_path, sizeof(child_st.cache_bundle_path),
                 "%s/cache/bundle.json", cfg->base_dir);
        struct stat stbuf;
        if (stat(child_st.cache_image_path, &stbuf) == 0 && stbuf.st_size > 0) {
            child_st.has_cached_image = true;
        }
        if (stat(child_st.cache_bundle_path, &stbuf) == 0 && stbuf.st_size > 0) {
            child_st.has_bundle_json = true;
        }
        Telemetry fresh_tel = tel ? *tel : (Telemetry){0};
        if (!fresh_tel.wifi_up) {
            net_ensure_wifi(cfg);
            sysinfo_collect(&fresh_tel, cfg);
        }
        (void)net_sync_blocking(cfg, &fresh_tel, &child_st);
        write_sync_result(result_path, &child_st);
        char done = '1';
        (void)write(pfd[1], &done, 1);
        close(pfd[1]);
        _exit(0);
    }

    close(pfd[1]);
    *out_pipe_fd = pfd[0];
    *out_pid = pid;
    return 0;
}

/* ============================================================================
 * Real-Time Remote Push Watcher (Lightweight HTTPS/HTTP SSE Client)
 * ============================================================================ */

bool net_derive_stream_url(const char *server_url, char *out_url, size_t out_sz) {
    if (!server_url || !server_url[0] || !out_url || out_sz == 0) return false;
    const char *b = strstr(server_url, "/bundle");
    if (!b || (b[7] != '\0' && b[7] != '?')) return false;
    size_t prefix_len = (size_t)(b - server_url);
    int n = snprintf(out_url, out_sz, "%.*s/stream%s", (int)prefix_len, server_url, b + 7);
    return (n > 0 && (size_t)n < out_sz);
}

static void write_file_atomic_str(const char *dst_path, const char *data, size_t len) {
    char tmp[320];
    snprintf(tmp, sizeof(tmp), "%s.tmp", dst_path);
    FILE *fp = fopen(tmp, "wb");
    if (!fp) return;
    if (fwrite(data, 1, len, fp) == len) {
        fclose(fp);
        rename(tmp, dst_path);
    } else {
        fclose(fp);
        unlink(tmp);
    }
}

static void run_sse_watcher_loop(const Config *cfg, const char *stream_url,
                                 const char *alerts_out_path, int notify_fd) {
    bool is_https = false;
    char host[128], port[16], path[256];
    if (!parse_url(stream_url, &is_https, host, sizeof(host), port, sizeof(port), path, sizeof(path))) {
        return;
    }

    int backoff_s = 5;
    for (;;) {
        NetStream ns;
        char err[96];
        if (!stream_connect(&ns, is_https, host, port, cfg, 15, err, sizeof(err))) {
            char st_ch = 'D';
            (void)write(notify_fd, &st_ch, 1);
            sleep((unsigned)backoff_s);
            if (backoff_s < 60) backoff_s *= 2;
            continue;
        }

        bool default_port = (!is_https && strcmp(port, "80") == 0) ||
                            (is_https && strcmp(port, "443") == 0);
        char req[1024];
        int off = snprintf(req, sizeof(req),
                           "GET %s HTTP/1.1\r\n"
                           "Host: %s%s%s\r\n"
                           "User-Agent: KoboDash/%s\r\n"
                           "Accept: text/event-stream\r\n"
                           "Accept-Encoding: identity\r\n"
                           "Cache-Control: no-cache\r\n"
                           "Connection: close\r\n",
                           path,
                           host, default_port ? "" : ":", default_port ? "" : port,
                           KD_VERSION);
        if (cfg->server_token[0] && off > 0 && (size_t)off < sizeof(req)) {
            off += snprintf(req + off, sizeof(req) - (size_t)off,
                            "Authorization: Bearer %s\r\n", cfg->server_token);
        }
        if (off > 0 && (size_t)off + 3 < sizeof(req)) {
            off += snprintf(req + off, sizeof(req) - (size_t)off, "\r\n");
        }

        if (!stream_write_all(&ns, req, (size_t)off)) {
            stream_close(&ns);
            sleep((unsigned)backoff_s);
            if (backoff_s < 60) backoff_s *= 2;
            continue;
        }

        /* Read HTTP response headers */
        char hdr[4096];
        size_t hlen = 0;
        char *body = NULL;
        while (hlen < sizeof(hdr) - 1) {
            ssize_t n = stream_read(&ns, hdr + hlen, sizeof(hdr) - 1 - hlen, 15000);
            if (n <= 0) break;
            hlen += (size_t)n;
            hdr[hlen] = '\0';
            body = strstr(hdr, "\r\n\r\n");
            if (body) break;
        }

        HttpMeta meta;
        memset(&meta, 0, sizeof(meta));
        if (body) {
            *body = '\0';
            parse_headers(hdr, &meta);
        }
        if (!body || meta.status_code != 200) {
            stream_close(&ns);
            char st_ch = 'D';
            (void)write(notify_fd, &st_ch, 1);
            sleep((unsigned)backoff_s);
            if (backoff_s < 60) backoff_s *= 2;
            continue;
        }

        /* Connected! Notify parent */
        backoff_s = 5;
        char c_ch = 'C';
        (void)write(notify_fd, &c_ch, 1);

        /* Line buffer for SSE frames ("data: {...}\n") */
        char linebuf[16384];
        size_t llen = 0;

        /* Seed any bytes received after \r\n\r\n */
        const char *rem = body + 4;
        size_t rem_len = hlen - (size_t)(rem - hdr);
        char chunk[4096];
        for (;;) {
            const char *src = NULL;
            size_t slen = 0;
            if (rem_len > 0) {
                src = rem;
                slen = rem_len;
                rem_len = 0;
            } else {
                ssize_t n = stream_read(&ns, chunk, sizeof(chunk), 45000);
                if (n == -2) continue; /* timeout waiting for keepalive: keep waiting */
                if (n <= 0) break;     /* clean reconnect on EOF/error */
                src = chunk;
                slen = (size_t)n;
            }

            for (size_t i = 0; i < slen; i++) {
                char ch = src[i];
                if (ch == '\r') continue;
                if (ch == '\n') {
                    linebuf[llen] = '\0';
                    if (strncmp(linebuf, "data: ", 6) == 0 && llen > 8) {
                        const char *payload = linebuf + 6;
                        if (payload[0] == '{') {
                            write_file_atomic_str(alerts_out_path, payload, strlen(payload));
                            char a_ch = 'A';
                            if (write(notify_fd, &a_ch, 1) <= 0) {
                                stream_close(&ns);
                                return;
                            }
                        }
                    }
                    llen = 0;
                } else if (llen + 1 < sizeof(linebuf)) {
                    linebuf[llen++] = ch;
                }
            }
        }

        stream_close(&ns);
        char st_ch = 'D';
        if (write(notify_fd, &st_ch, 1) <= 0) return;
        sleep(3);
    }
}

int net_start_push_watcher(const Config *cfg, const char *alerts_out_path,
                           int *out_pipe_fd, pid_t *out_pid) {
    char stream_url[320];
    if (!cfg || !net_derive_stream_url(cfg->server_url, stream_url, sizeof(stream_url))) {
        return -1;
    }

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
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGPIPE, SIG_IGN);
        run_sse_watcher_loop(cfg, stream_url, alerts_out_path, pfd[1]);
        close(pfd[1]);
        _exit(0);
    }

    close(pfd[1]);
    *out_pipe_fd = pfd[0];
    *out_pid = pid;
    return 0;
}
