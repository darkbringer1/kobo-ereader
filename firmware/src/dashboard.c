#define _GNU_SOURCE
#include "kobodash.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* ============================================================================
 * Minimal Recursive-Descent JSON Reader (zero external dependencies)
 * ============================================================================ */

typedef struct {
    const char *p;
} JsonCur;

static void j_ws(JsonCur *j) {
    while (*j->p && (*j->p == ' ' || *j->p == '\t' || *j->p == '\r' || *j->p == '\n')) {
        j->p++;
    }
}

static bool j_consume(JsonCur *j, char ch) {
    j_ws(j);
    if (*j->p == ch) {
        j->p++;
        return true;
    }
    return false;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t encode_utf8(uint32_t cp, char *out, size_t avail) {
    if (cp < 0x80) {
        if (avail < 1) return 0;
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        if (avail < 2) return 0;
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        if (avail < 3) return 0;
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else if (cp <= 0x10FFFF) {
        if (avail < 4) return 0;
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

static bool j_string(JsonCur *j, char *out, size_t out_sz) {
    j_ws(j);
    if (*j->p != '"') return false;
    j->p++;
    size_t n = 0;
    while (*j->p && *j->p != '"') {
        if (*j->p == '\\' && j->p[1]) {
            j->p++;
            char esc = *j->p++;
            if (esc == '"' || esc == '\\' || esc == '/') {
                if (out && n + 1 < out_sz) out[n++] = esc;
            } else if (esc == 'n' || esc == 'r' || esc == 't') {
                if (out && n + 1 < out_sz) out[n++] = ' ';
            } else if (esc == 'u') {
                int h0 = hex_val(j->p[0]), h1 = hex_val(j->p[1]);
                int h2 = hex_val(j->p[2]), h3 = hex_val(j->p[3]);
                if (h0 >= 0 && h1 >= 0 && h2 >= 0 && h3 >= 0) {
                    uint32_t cp = (uint32_t)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                    j->p += 4;
                    /* Surrogate pair handling */
                    if (cp >= 0xD800 && cp <= 0xDBFF && j->p[0] == '\\' && j->p[1] == 'u') {
                        int l0 = hex_val(j->p[2]), l1 = hex_val(j->p[3]);
                        int l2 = hex_val(j->p[4]), l3 = hex_val(j->p[5]);
                        if (l0 >= 0 && l1 >= 0 && l2 >= 0 && l3 >= 0) {
                            uint32_t lo = (uint32_t)((l0 << 12) | (l1 << 8) | (l2 << 4) | l3);
                            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                cp = 0x10000 + (((cp - 0xD800) << 10) | (lo - 0xDC00));
                                j->p += 6;
                            }
                        }
                    }
                    if (out && n + 1 < out_sz) {
                        n += encode_utf8(cp, out + n, out_sz - 1 - n);
                    }
                }
            }
        } else {
            if (out && n + 1 < out_sz) out[n++] = *j->p;
            j->p++;
        }
    }
    if (out && out_sz > 0) out[n] = '\0';
    if (*j->p == '"') {
        j->p++;
        return true;
    }
    return false;
}

static bool j_int64(JsonCur *j, long long *out) {
    j_ws(j);
    const char *start = j->p;
    if (*j->p == '-') j->p++;
    if (!isdigit((unsigned char)*j->p)) {
        j->p = start;
        return false;
    }
    while (isdigit((unsigned char)*j->p)) j->p++;
    if (*j->p == '.') {
        j->p++;
        while (isdigit((unsigned char)*j->p)) j->p++;
    }
    if (*j->p == 'e' || *j->p == 'E') {
        j->p++;
        if (*j->p == '+' || *j->p == '-') j->p++;
        while (isdigit((unsigned char)*j->p)) j->p++;
    }
    if (out) *out = strtoll(start, NULL, 10);
    return true;
}

static bool j_bool(JsonCur *j, bool *out) {
    j_ws(j);
    if (strncmp(j->p, "true", 4) == 0) {
        j->p += 4;
        if (out) *out = true;
        return true;
    }
    if (strncmp(j->p, "false", 5) == 0) {
        j->p += 5;
        if (out) *out = false;
        return true;
    }
    return false;
}

static bool j_skip(JsonCur *j, int depth) {
    if (depth > 24) return false;
    j_ws(j);
    char c = *j->p;
    if (c == '"') return j_string(j, NULL, 0);
    if (c == '{') {
        j->p++;
        if (j_consume(j, '}')) return true;
        for (;;) {
            if (!j_string(j, NULL, 0)) return false;
            if (!j_consume(j, ':')) return false;
            if (!j_skip(j, depth + 1)) return false;
            if (j_consume(j, ',')) continue;
            return j_consume(j, '}');
        }
    }
    if (c == '[') {
        j->p++;
        if (j_consume(j, ']')) return true;
        for (;;) {
            if (!j_skip(j, depth + 1)) return false;
            if (j_consume(j, ',')) continue;
            return j_consume(j, ']');
        }
    }
    if (c == 't' && strncmp(j->p, "true", 4) == 0) { j->p += 4; return true; }
    if (c == 'f' && strncmp(j->p, "false", 5) == 0) { j->p += 5; return true; }
    if (c == 'n' && strncmp(j->p, "null", 4) == 0) { j->p += 4; return true; }
    return j_int64(j, NULL);
}

/* ============================================================================
 * Bundle / Alerts JSON Parsers
 * ============================================================================ */

static DashPageKind parse_page_kind(const char *s) {
    if (!s) return DASH_PAGE_GENERIC;
    if (strcmp(s, "overview") == 0) return DASH_PAGE_OVERVIEW;
    if (strcmp(s, "calendar") == 0) return DASH_PAGE_CALENDAR;
    if (strcmp(s, "list") == 0) return DASH_PAGE_LIST;
    if (strcmp(s, "stats") == 0) return DASH_PAGE_STATS;
    return DASH_PAGE_GENERIC;
}

static bool parse_bar(JsonCur *j, DashBar *b) {
    memset(b, 0, sizeof(*b));
    if (!j_consume(j, '{')) return false;
    if (j_consume(j, '}')) return true;
    for (;;) {
        char k[32];
        if (!j_string(j, k, sizeof(k)) || !j_consume(j, ':')) return false;
        if (strcmp(k, "label") == 0) { if (!j_string(j, b->label, sizeof(b->label))) return false; }
        else if (strcmp(k, "detail") == 0) { if (!j_string(j, b->detail, sizeof(b->detail))) return false; }
        else if (strcmp(k, "pct") == 0) {
            long long v = 0;
            if (!j_int64(j, &v)) return false;
            b->pct = (v < 0) ? 0 : (v > 100 ? 100 : (int)v);
        } else {
            if (!j_skip(j, 0)) return false;
        }
        if (j_consume(j, ',')) continue;
        return j_consume(j, '}');
    }
}

static bool parse_metric(JsonCur *j, DashMetric *m) {
    memset(m, 0, sizeof(*m));
    if (!j_consume(j, '{')) return false;
    if (j_consume(j, '}')) return true;
    for (;;) {
        char k[32];
        if (!j_string(j, k, sizeof(k)) || !j_consume(j, ':')) return false;
        if (strcmp(k, "label") == 0) { if (!j_string(j, m->label, sizeof(m->label))) return false; }
        else if (strcmp(k, "value") == 0) { if (!j_string(j, m->value, sizeof(m->value))) return false; }
        else if (strcmp(k, "detail") == 0) { if (!j_string(j, m->detail, sizeof(m->detail))) return false; }
        else { if (!j_skip(j, 0)) return false; }
        if (j_consume(j, ',')) continue;
        return j_consume(j, '}');
    }
}

static bool parse_item(JsonCur *j, DashItem *it) {
    memset(it, 0, sizeof(*it));
    if (!j_consume(j, '{')) return false;
    if (j_consume(j, '}')) return true;
    for (;;) {
        char k[32];
        if (!j_string(j, k, sizeof(k)) || !j_consume(j, ':')) return false;
        if (strcmp(k, "label") == 0) { if (!j_string(j, it->label, sizeof(it->label))) return false; }
        else if (strcmp(k, "value") == 0) { if (!j_string(j, it->value, sizeof(it->value))) return false; }
        else if (strcmp(k, "detail") == 0) { if (!j_string(j, it->detail, sizeof(it->detail))) return false; }
        else if (strcmp(k, "urgent") == 0) { if (!j_bool(j, &it->urgent)) return false; }
        else { if (!j_skip(j, 0)) return false; }
        if (j_consume(j, ',')) continue;
        return j_consume(j, '}');
    }
}

static bool parse_page(JsonCur *j, DashPage *pg) {
    memset(pg, 0, sizeof(*pg));
    if (!j_consume(j, '{')) return false;
    if (j_consume(j, '}')) return true;
    for (;;) {
        char k[32];
        if (!j_string(j, k, sizeof(k)) || !j_consume(j, ':')) return false;
        if (strcmp(k, "id") == 0) { if (!j_string(j, pg->id, sizeof(pg->id))) return false; }
        else if (strcmp(k, "kind") == 0) {
            char ks[24];
            if (!j_string(j, ks, sizeof(ks))) return false;
            pg->kind = parse_page_kind(ks);
        } else if (strcmp(k, "title") == 0) { if (!j_string(j, pg->title, sizeof(pg->title))) return false; }
        else if (strcmp(k, "subtitle") == 0) { if (!j_string(j, pg->subtitle, sizeof(pg->subtitle))) return false; }
        else if (strcmp(k, "badge") == 0) { if (!j_string(j, pg->badge, sizeof(pg->badge))) return false; }
        else if (strcmp(k, "hero") == 0) { if (!j_string(j, pg->hero, sizeof(pg->hero))) return false; }
        else if (strcmp(k, "hero_sub") == 0) { if (!j_string(j, pg->hero_sub, sizeof(pg->hero_sub))) return false; }
        else if (strcmp(k, "footer") == 0) { if (!j_string(j, pg->footer, sizeof(pg->footer))) return false; }
        else if (strcmp(k, "cal_year") == 0) { long long v = 0; if (!j_int64(j, &v)) return false; pg->cal_year = (int)v; }
        else if (strcmp(k, "cal_month") == 0) { long long v = 0; if (!j_int64(j, &v)) return false; pg->cal_month = (int)v; }
        else if (strcmp(k, "cal_today") == 0) { long long v = 0; if (!j_int64(j, &v)) return false; pg->cal_today = (int)v; }
        else if (strcmp(k, "cal_marks") == 0) {
            if (!j_consume(j, '[')) return false;
            if (!j_consume(j, ']')) {
                for (;;) {
                    long long d = 0;
                    if (!j_int64(j, &d)) return false;
                    if (d >= 1 && d <= 31) pg->cal_marks_mask |= (1u << (unsigned)(d - 1));
                    if (j_consume(j, ',')) continue;
                    if (!j_consume(j, ']')) return false;
                    break;
                }
            }
        } else if (strcmp(k, "bars") == 0) {
            if (!j_consume(j, '[')) return false;
            if (!j_consume(j, ']')) {
                for (;;) {
                    if (pg->bar_count < KD_MAX_PAGE_BARS) {
                        if (!parse_bar(j, &pg->bars[pg->bar_count])) return false;
                        pg->bar_count++;
                    } else {
                        if (!j_skip(j, 0)) return false;
                    }
                    if (j_consume(j, ',')) continue;
                    if (!j_consume(j, ']')) return false;
                    break;
                }
            }
        } else if (strcmp(k, "metrics") == 0) {
            if (!j_consume(j, '[')) return false;
            if (!j_consume(j, ']')) {
                for (;;) {
                    if (pg->metric_count < KD_MAX_PAGE_METRICS) {
                        if (!parse_metric(j, &pg->metrics[pg->metric_count])) return false;
                        pg->metric_count++;
                    } else {
                        if (!j_skip(j, 0)) return false;
                    }
                    if (j_consume(j, ',')) continue;
                    if (!j_consume(j, ']')) return false;
                    break;
                }
            }
        } else if (strcmp(k, "items") == 0) {
            if (!j_consume(j, '[')) return false;
            if (!j_consume(j, ']')) {
                for (;;) {
                    if (pg->item_count < KD_MAX_PAGE_ITEMS) {
                        if (!parse_item(j, &pg->items[pg->item_count])) return false;
                        pg->item_count++;
                    } else {
                        if (!j_skip(j, 0)) return false;
                    }
                    if (j_consume(j, ',')) continue;
                    if (!j_consume(j, ']')) return false;
                    break;
                }
            }
        } else {
            if (!j_skip(j, 0)) return false;
        }
        if (j_consume(j, ',')) continue;
        return j_consume(j, '}');
    }
}

static bool parse_event(JsonCur *j, OfflineEvent *ev) {
    memset(ev, 0, sizeof(*ev));
    if (!j_consume(j, '{')) return false;
    if (j_consume(j, '}')) return true;
    for (;;) {
        char k[32];
        if (!j_string(j, k, sizeof(k)) || !j_consume(j, ':')) return false;
        if (strcmp(k, "id") == 0) { if (!j_string(j, ev->id, sizeof(ev->id))) return false; }
        else if (strcmp(k, "start_ts") == 0) { long long v = 0; if (!j_int64(j, &v)) return false; ev->start_ts = (time_t)v; }
        else if (strcmp(k, "all_day") == 0) { if (!j_bool(j, &ev->all_day)) return false; }
        else if (strcmp(k, "remind_min") == 0) { long long v = 0; if (!j_int64(j, &v)) return false; ev->remind_min = (int)v; }
        else if (strcmp(k, "title") == 0) { if (!j_string(j, ev->title, sizeof(ev->title))) return false; }
        else if (strcmp(k, "tag") == 0) { if (!j_string(j, ev->tag, sizeof(ev->tag))) return false; }
        else if (strcmp(k, "when_label") == 0) { if (!j_string(j, ev->when_label, sizeof(ev->when_label))) return false; }
        else { if (!j_skip(j, 0)) return false; }
        if (j_consume(j, ',')) continue;
        return j_consume(j, '}');
    }
}

static bool parse_alert(JsonCur *j, RemoteAlert *al) {
    memset(al, 0, sizeof(*al));
    if (!j_consume(j, '{')) return false;
    if (j_consume(j, '}')) return true;
    for (;;) {
        char k[32];
        if (!j_string(j, k, sizeof(k)) || !j_consume(j, ':')) return false;
        if (strcmp(k, "id") == 0) { if (!j_string(j, al->id, sizeof(al->id))) return false; }
        else if (strcmp(k, "title") == 0) { if (!j_string(j, al->title, sizeof(al->title))) return false; }
        else if (strcmp(k, "body") == 0) { if (!j_string(j, al->body, sizeof(al->body))) return false; }
        else if (strcmp(k, "source") == 0) { if (!j_string(j, al->source, sizeof(al->source))) return false; }
        else if (strcmp(k, "due") == 0) { if (!j_string(j, al->due, sizeof(al->due))) return false; }
        else if (strcmp(k, "urgent") == 0) { if (!j_bool(j, &al->urgent)) return false; }
        else if (strcmp(k, "ts") == 0) { long long v = 0; if (!j_int64(j, &v)) return false; al->ts = (time_t)v; }
        else { if (!j_skip(j, 0)) return false; }
        if (j_consume(j, ',')) continue;
        return j_consume(j, '}');
    }
}

bool dashboard_parse_bundle(const char *json, DashboardBundle *out) {
    if (!json || !out) return false;
    DashboardBundle tmp;
    memset(&tmp, 0, sizeof(tmp));

    /* Legacy single-page support fields */
    char leg_title[48] = {0}, leg_sub[80] = {0};
    char leg_lines[KD_MAX_PAGE_ITEMS][104];
    int leg_n = 0;

    JsonCur j = { json };
    if (!j_consume(&j, '{')) return false;
    if (!j_consume(&j, '}')) {
        for (;;) {
            char k[32];
            if (!j_string(&j, k, sizeof(k)) || !j_consume(&j, ':')) return false;
            if (strcmp(k, "generated_ts") == 0) {
                long long v = 0;
                if (!j_int64(&j, &v)) return false;
                tmp.generated_ts = (time_t)v;
            } else if (strcmp(k, "updated") == 0) {
                if (!j_string(&j, tmp.updated, sizeof(tmp.updated))) return false;
            } else if (strcmp(k, "hash") == 0) {
                if (!j_string(&j, tmp.alerts_hash, sizeof(tmp.alerts_hash))) return false;
            } else if (strcmp(k, "pages") == 0) {
                if (!j_consume(&j, '[')) return false;
                if (!j_consume(&j, ']')) {
                    for (;;) {
                        if (tmp.page_count < KD_MAX_DASH_PAGES) {
                            if (!parse_page(&j, &tmp.pages[tmp.page_count])) return false;
                            tmp.page_count++;
                        } else {
                            if (!j_skip(&j, 0)) return false;
                        }
                        if (j_consume(&j, ',')) continue;
                        if (!j_consume(&j, ']')) return false;
                        break;
                    }
                }
            } else if (strcmp(k, "events") == 0) {
                if (!j_consume(&j, '[')) return false;
                if (!j_consume(&j, ']')) {
                    for (;;) {
                        if (tmp.event_count < KD_MAX_EVENTS) {
                            if (!parse_event(&j, &tmp.events[tmp.event_count])) return false;
                            tmp.event_count++;
                        } else {
                            if (!j_skip(&j, 0)) return false;
                        }
                        if (j_consume(&j, ',')) continue;
                        if (!j_consume(&j, ']')) return false;
                        break;
                    }
                }
            } else if (strcmp(k, "alerts") == 0) {
                if (!j_consume(&j, '[')) return false;
                if (!j_consume(&j, ']')) {
                    for (;;) {
                        if (tmp.alert_count < KD_MAX_ALERTS) {
                            if (!parse_alert(&j, &tmp.alerts[tmp.alert_count])) return false;
                            tmp.alert_count++;
                        } else {
                            if (!j_skip(&j, 0)) return false;
                        }
                        if (j_consume(&j, ',')) continue;
                        if (!j_consume(&j, ']')) return false;
                        break;
                    }
                }
            } else if (strcmp(k, "title") == 0) {
                if (!j_string(&j, leg_title, sizeof(leg_title))) return false;
            } else if (strcmp(k, "subtitle") == 0) {
                if (!j_string(&j, leg_sub, sizeof(leg_sub))) return false;
            } else if (strcmp(k, "lines") == 0) {
                if (!j_consume(&j, '[')) return false;
                if (!j_consume(&j, ']')) {
                    for (;;) {
                        if (leg_n < KD_MAX_PAGE_ITEMS) {
                            if (!j_string(&j, leg_lines[leg_n], sizeof(leg_lines[0]))) return false;
                            leg_n++;
                        } else {
                            if (!j_skip(&j, 0)) return false;
                        }
                        if (j_consume(&j, ',')) continue;
                        if (!j_consume(&j, ']')) return false;
                        break;
                    }
                }
            } else {
                if (!j_skip(&j, 0)) return false;
            }
            if (j_consume(&j, ',')) continue;
            if (!j_consume(&j, '}')) return false;
            break;
        }
    }

    if (tmp.page_count == 0 && (leg_title[0] || leg_n > 0)) {
        DashPage *pg = &tmp.pages[0];
        snprintf(pg->id, sizeof(pg->id), "legacy");
        pg->kind = DASH_PAGE_GENERIC;
        snprintf(pg->title, sizeof(pg->title), "%s", leg_title[0] ? leg_title : "DASHBOARD");
        snprintf(pg->subtitle, sizeof(pg->subtitle), "%s", leg_sub);
        for (int i = 0; i < leg_n; i++) {
            snprintf(pg->items[i].value, sizeof(pg->items[i].value), "%s", leg_lines[i]);
        }
        pg->item_count = leg_n;
        tmp.page_count = 1;
    }

    if (tmp.page_count == 0 && tmp.event_count == 0 && tmp.alert_count == 0) {
        return false;
    }
    tmp.has_bundle = true;
    *out = tmp;
    return true;
}

bool dashboard_parse_alerts_update(const char *json, DashboardBundle *bundle) {
    if (!json || !bundle) return false;
    RemoteAlert new_alerts[KD_MAX_ALERTS];
    int new_count = 0;
    char new_hash[24] = {0};
    char new_upd[24] = {0};
    bool saw_alerts = false;

    JsonCur j = { json };
    if (!j_consume(&j, '{')) return false;
    if (!j_consume(&j, '}')) {
        for (;;) {
            char k[32];
            if (!j_string(&j, k, sizeof(k)) || !j_consume(&j, ':')) return false;
            if (strcmp(k, "hash") == 0) {
                if (!j_string(&j, new_hash, sizeof(new_hash))) return false;
            } else if (strcmp(k, "updated") == 0) {
                if (!j_string(&j, new_upd, sizeof(new_upd))) return false;
            } else if (strcmp(k, "alerts") == 0) {
                saw_alerts = true;
                if (!j_consume(&j, '[')) return false;
                if (!j_consume(&j, ']')) {
                    for (;;) {
                        if (new_count < KD_MAX_ALERTS) {
                            if (!parse_alert(&j, &new_alerts[new_count])) return false;
                            new_count++;
                        } else {
                            if (!j_skip(&j, 0)) return false;
                        }
                        if (j_consume(&j, ',')) continue;
                        if (!j_consume(&j, ']')) return false;
                        break;
                    }
                }
            } else {
                if (!j_skip(&j, 0)) return false;
            }
            if (j_consume(&j, ',')) continue;
            if (!j_consume(&j, '}')) return false;
            break;
        }
    }
    if (!saw_alerts) return false;

    bundle->alert_count = new_count;
    for (int i = 0; i < new_count; i++) {
        bundle->alerts[i] = new_alerts[i];
    }
    if (new_hash[0]) snprintf(bundle->alerts_hash, sizeof(bundle->alerts_hash), "%s", new_hash);
    if (new_upd[0]) snprintf(bundle->updated, sizeof(bundle->updated), "%s", new_upd);

    /* Also refresh the "waiting" page in-place if present */
    for (int p = 0; p < bundle->page_count; p++) {
        if (strcmp(bundle->pages[p].id, "waiting") == 0 || bundle->pages[p].kind == DASH_PAGE_LIST) {
            DashPage *pg = &bundle->pages[p];
            if (new_count > 0) {
                snprintf(pg->badge, sizeof(pg->badge), "%d OPEN", new_count);
                snprintf(pg->subtitle, sizeof(pg->subtitle), "%d open items  •  Updated %s",
                         new_count, new_upd[0] ? new_upd : "now");
            } else {
                snprintf(pg->badge, sizeof(pg->badge), "ALL CLEAR");
                snprintf(pg->subtitle, sizeof(pg->subtitle), "All clear  •  Updated %s",
                         new_upd[0] ? new_upd : "now");
            }
            pg->item_count = 0;
            for (int i = 0; i < new_count && i < KD_MAX_PAGE_ITEMS; i++) {
                DashItem *it = &pg->items[pg->item_count++];
                memset(it, 0, sizeof(*it));
                snprintf(it->label, sizeof(it->label), "%s",
                         new_alerts[i].due[0] ? new_alerts[i].due : "OPEN");
                snprintf(it->value, sizeof(it->value), "%s", new_alerts[i].title);
                if (new_alerts[i].body[0]) {
                    snprintf(it->detail, sizeof(it->detail), "From %s  •  %s",
                             new_alerts[i].source[0] ? new_alerts[i].source : "Dashboard",
                             new_alerts[i].body);
                } else {
                    snprintf(it->detail, sizeof(it->detail), "From %s",
                             new_alerts[i].source[0] ? new_alerts[i].source : "Dashboard");
                }
                it->urgent = new_alerts[i].urgent;
            }
            break;
        }
    }
    return true;
}

bool dashboard_load_cached(const char *path, DashboardBundle *out) {
    if (!path || !path[0] || !out) return false;
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return false; }
    long sz = ftell(fp);
    if (sz <= 2 || sz > 128 * 1024) { fclose(fp); return false; }
    rewind(fp);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return false; }
    size_t n = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[n] = '\0';
    bool ok = dashboard_parse_bundle(buf, out);
    free(buf);
    return ok;
}

/* ============================================================================
 * Offline Event + Remote Alert Trigger Engine
 * ============================================================================ */

void dashboard_load_dismissed(const char *path, AlertTracker *tr) {
    if (!path || !tr) return;
    tr->record_count = 0;
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    char line[128];
    while (fgets(line, sizeof(line), fp) && tr->record_count < KD_MAX_DISMISSED) {
        long long until_ts = 0;
        char id[80] = {0};
        if (sscanf(line, "%lld %79[^\r\n]", &until_ts, id) == 2 && id[0]) {
            snprintf(tr->records[tr->record_count].id, sizeof(tr->records[0].id), "%s", id);
            tr->records[tr->record_count].until_ts = (time_t)until_ts;
            tr->record_count++;
        }
    }
    fclose(fp);
}

void dashboard_save_dismissed(const char *path, const AlertTracker *tr) {
    if (!path || !path[0] || !tr) return;
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *fp = fopen(tmp, "w");
    if (!fp) return;
    for (int i = 0; i < tr->record_count; i++) {
        fprintf(fp, "%lld %s\n", (long long)tr->records[i].until_ts, tr->records[i].id);
    }
    fclose(fp);
    rename(tmp, path);
}

static bool is_suppressed(const AlertTracker *tr, const char *id, time_t now) {
    if (!tr || !id || !id[0]) return false;
    for (int i = 0; i < tr->record_count; i++) {
        if (strcmp(tr->records[i].id, id) == 0) {
            if (tr->records[i].until_ts == 0) return true; /* permanently dismissed */
            if (now < tr->records[i].until_ts) return true; /* still snoozed */
        }
    }
    return false;
}

static void record_suppression(AlertTracker *tr, const char *id, time_t until_ts) {
    if (!tr || !id || !id[0]) return;
    for (int i = 0; i < tr->record_count; i++) {
        if (strcmp(tr->records[i].id, id) == 0) {
            tr->records[i].until_ts = until_ts;
            return;
        }
    }
    if (tr->record_count >= KD_MAX_DISMISSED) {
        memmove(&tr->records[0], &tr->records[1], sizeof(tr->records[0]) * (KD_MAX_DISMISSED - 1));
        tr->record_count = KD_MAX_DISMISSED - 1;
    }
    snprintf(tr->records[tr->record_count].id, sizeof(tr->records[0].id), "%s", id);
    tr->records[tr->record_count].until_ts = until_ts;
    tr->record_count++;
}

void dashboard_dismiss_active(AlertTracker *tr, const char *persist_path) {
    if (!tr || !tr->current.active) return;
    record_suppression(tr, tr->current.id, 0);
    tr->current.active = false;
    if (persist_path) dashboard_save_dismissed(persist_path, tr);
}

void dashboard_snooze_active(AlertTracker *tr, time_t now, int snooze_min, const char *persist_path) {
    if (!tr || !tr->current.active) return;
    record_suppression(tr, tr->current.id, now + (time_t)snooze_min * 60);
    tr->current.active = false;
    if (persist_path) dashboard_save_dismissed(persist_path, tr);
}

bool dashboard_check_triggers(const DashboardBundle *b, AlertTracker *tr, time_t now) {
    if (!b || !tr) return false;
    bool changed = false;

    /* 1. If a remote alert is currently shown, auto-clear it if resolved on server! */
    if (tr->current.active && tr->current.is_remote) {
        bool still_open = false;
        for (int i = 0; i < b->alert_count; i++) {
            if (strcmp(b->alerts[i].id, tr->current.id) == 0) {
                still_open = true;
                break;
            }
        }
        if (!still_open) {
            tr->current.active = false;
            changed = true;
        }
    }

    if (tr->current.active) return changed;

    /* 2. Check remote 'Waiting on You' (Senden bekleyen) alerts first */
    for (int i = 0; i < b->alert_count; i++) {
        const RemoteAlert *a = &b->alerts[i];
        if (!a->id[0] || is_suppressed(tr, a->id, now)) continue;
        memset(&tr->current, 0, sizeof(tr->current));
        tr->current.active = true;
        tr->current.is_remote = true;
        tr->current.urgent = a->urgent;
        snprintf(tr->current.id, sizeof(tr->current.id), "%s", a->id);
        snprintf(tr->current.badge, sizeof(tr->current.badge), "%s",
                 a->due[0] ? a->due : "OPEN");
        snprintf(tr->current.title, sizeof(tr->current.title), "%s", a->title);
        snprintf(tr->current.body, sizeof(tr->current.body), "%s",
                 a->body[0] ? a->body : "Needs your attention on your dashboard.");
        snprintf(tr->current.meta, sizeof(tr->current.meta), "From %s  •  Remote dashboard alert",
                 a->source[0] ? a->source : "Dashboard");
        return true;
    }

    /* 3. Check offline calendar event triggers (works 100% without Wi-Fi!) */
    if (now >= 1700000000) {
        for (int i = 0; i < b->event_count; i++) {
            const OfflineEvent *ev = &b->events[i];
            if (!ev->id[0] || ev->start_ts <= 0) continue;
            time_t fire_at = ev->start_ts - (time_t)ev->remind_min * 60;
            if (now >= fire_at && now <= ev->start_ts + 1800) {
                char eid[80];
                snprintf(eid, sizeof(eid), "ev:%s", ev->id);
                if (is_suppressed(tr, eid, now)) continue;
                memset(&tr->current, 0, sizeof(tr->current));
                tr->current.active = true;
                tr->current.is_remote = false;
                tr->current.urgent = true;
                snprintf(tr->current.id, sizeof(tr->current.id), "%s", eid);
                long mins_left = (long)((ev->start_ts - now + 30) / 60);
                if (mins_left <= 0) {
                    snprintf(tr->current.badge, sizeof(tr->current.badge), "NOW");
                } else {
                    snprintf(tr->current.badge, sizeof(tr->current.badge), "IN %ld MIN", mins_left);
                }
                snprintf(tr->current.title, sizeof(tr->current.title), "%s", ev->title);
                snprintf(tr->current.body, sizeof(tr->current.body), "Scheduled: %s%s%s",
                         ev->when_label[0] ? ev->when_label : "Today",
                         ev->tag[0] ? "  •  " : "",
                         ev->tag[0] ? ev->tag : "");
                snprintf(tr->current.meta, sizeof(tr->current.meta), "Offline Calendar Reminder");
                return true;
            }
        }
    }

    return changed;
}

bool dashboard_clock_strip(const DashboardBundle *b, time_t now,
                           char *out_text, size_t out_sz, int *out_target_page) {
    if (!b || !b->has_bundle || !out_text || out_sz == 0) return false;
    out_text[0] = '\0';
    if (out_target_page) *out_target_page = 0;

    int waiting_page = 0;
    int cal_page = 0;
    for (int i = 0; i < b->page_count; i++) {
        if (b->pages[i].kind == DASH_PAGE_LIST) waiting_page = i;
        if (b->pages[i].kind == DASH_PAGE_CALENDAR) cal_page = i;
    }

    /* Check nearest upcoming event within 12 hours */
    const OfflineEvent *next_ev = NULL;
    long next_min = 999999;
    if (now >= 1700000000) {
        for (int i = 0; i < b->event_count; i++) {
            const OfflineEvent *ev = &b->events[i];
            if (ev->start_ts > now - 300 && ev->start_ts <= now + 12 * 3600) {
                long m = (long)((ev->start_ts - now + 30) / 60);
                if (m < next_min) {
                    next_min = m;
                    next_ev = ev;
                }
            }
        }
    }

    if (next_ev && next_min <= 90) {
        if (next_min <= 0) {
            snprintf(out_text, out_sz, "NOW: %.42s", next_ev->title);
        } else {
            snprintf(out_text, out_sz, "In %ldm: %.40s", next_min, next_ev->title);
        }
        if (out_target_page) *out_target_page = cal_page;
        return true;
    }

    if (b->alert_count > 0 && next_ev) {
        snprintf(out_text, out_sz, "%d Waiting  •  Next: %.28s", b->alert_count, next_ev->title);
        if (out_target_page) *out_target_page = waiting_page;
        return true;
    }

    if (b->alert_count > 0) {
        snprintf(out_text, out_sz, "%d Waiting on You: %.30s",
                 b->alert_count, b->alerts[0].title);
        if (out_target_page) *out_target_page = waiting_page;
        return true;
    }

    if (next_ev) {
        snprintf(out_text, out_sz, "%s: %.36s", next_ev->when_label, next_ev->title);
        if (out_target_page) *out_target_page = cal_page;
        return true;
    }

    return false;
}

/* ============================================================================
 * Live Local Progress Bar Computation (always fresh even offline!)
 * ============================================================================ */

static int days_in_month_calc(int year, int month) {
    static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month < 1 || month > 12) return 30;
    int d = mdays[month - 1];
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))) d = 29;
    return d;
}

void dashboard_compute_local_bars(const struct tm *now_tm, DashBar out[4]) {
    memset(out, 0, sizeof(DashBar) * 4);
    if (!now_tm) return;
    double frac_day = (now_tm->tm_hour * 3600 + now_tm->tm_min * 60 + now_tm->tm_sec) / 86400.0;
    int day_pct = (int)(frac_day * 100.0 + 0.5);

    int wk_idx = (now_tm->tm_wday + 6) % 7; /* Mon=0 .. Sun=6 */
    int wk_pct = (int)(((wk_idx + frac_day) / 7.0) * 100.0 + 0.5);

    int year = now_tm->tm_year + 1900;
    int dim = days_in_month_calc(year, now_tm->tm_mon + 1);
    int mo_pct = (int)((((now_tm->tm_mday - 1) + frac_day) / (double)dim) * 100.0 + 0.5);

    int diy = ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)) ? 366 : 365;
    int yr_pct = (int)(((now_tm->tm_yday + frac_day) / (double)diy) * 100.0 + 0.5);

    snprintf(out[0].label, sizeof(out[0].label), "DAY");
    out[0].pct = day_pct;
    snprintf(out[0].detail, sizeof(out[0].detail), "%d%%", day_pct);

    snprintf(out[1].label, sizeof(out[1].label), "WEEK");
    out[1].pct = wk_pct;
    snprintf(out[1].detail, sizeof(out[1].detail), "Day %d/7", wk_idx + 1);

    snprintf(out[2].label, sizeof(out[2].label), "MONTH");
    out[2].pct = mo_pct;
    snprintf(out[2].detail, sizeof(out[2].detail), "%d/%d", now_tm->tm_mday, dim);

    snprintf(out[3].label, sizeof(out[3].label), "YEAR");
    out[3].pct = yr_pct;
    snprintf(out[3].detail, sizeof(out[3].detail), "Day %d/%d", now_tm->tm_yday + 1, diy);
}

/* ============================================================================
 * High-Contrast E-Ink Native Multi-Page Renderers
 * ============================================================================ */

typedef struct {
    uint8_t bg;
    uint8_t fg;
    uint8_t muted;
    uint8_t card_bg;
    uint8_t border;
    uint8_t subtle;
    uint8_t pill_bg;
    uint8_t pill_fg;
} DashTheme;

static DashTheme dash_theme(const Config *cfg) {
    DashTheme t;
    if (cfg && cfg->invert_mode) {
        t.bg = 0x00; t.fg = 0xFF; t.muted = 0xAA; t.card_bg = 0x18;
        t.border = 0xBB; t.subtle = 0x44; t.pill_bg = 0xEE; t.pill_fg = 0x00;
    } else {
        t.bg = 0xFF; t.fg = 0x00; t.muted = 0x55; t.card_bg = 0xF5;
        t.border = 0x22; t.subtle = 0xCC; t.pill_bg = 0x11; t.pill_fg = 0xFF;
    }
    return t;
}

static LockMode g_lock_render_mode = LOCK_NONE;
static int g_lock_rotate_hrs = 3;

static void draw_dash_chrome_shapes(Canvas *c, const DashboardBundle *b, int page_idx,
                                    const Gallery *gal, const DashTheme *th) {
    /* Top bar */
    canvas_hline(c, 24, 58, 552, 2, th->subtle);
    if (g_lock_render_mode != LOCK_NONE) {
        canvas_fill_rrect(c, 428, 13, 108, 34, 9, th->card_bg);
        canvas_stroke_rrect(c, 428, 13, 108, 34, 9, 1, th->border);
        canvas_draw_lock(c, 558, 30, th->fg, th->bg);
        canvas_hline(c, 24, 734, 552, 1, th->subtle);
        return;
    }

    canvas_draw_refresh_icon(c, 504, 30, 11, th->fg, th->bg);
    canvas_draw_gear(c, 558, 30, 10, th->fg, th->bg);

    /* Bottom nav with dots representing Clock, Library, DashPages..., Status, [Gallery] */
    canvas_hline(c, 24, 734, 552, 1, th->subtle);
    int npages = (b && b->page_count > 0) ? b->page_count : 1;
    int has_gal = (gal && gal->count > 0) ? 1 : 0;
    int total_dots = 3 + npages + has_gal; /* Clock, Library, npages, Status, [Gallery] */
    int active_dot = 2 + page_idx;
    int spacing = (total_dots > 7) ? 18 : 22;
    int total_w = (total_dots - 1) * spacing;
    int start_x = (KD_WIDTH - total_w) / 2;
    int cy = 758;
    for (int i = 0; i < total_dots; i++) {
        if (i == active_dot) {
            canvas_fill_circle(c, start_x + i * spacing, cy, 5, th->fg);
        } else {
            canvas_stroke_circle(c, start_x + i * spacing, cy, 5, 2, th->muted);
        }
    }
}

static void draw_dash_chrome_text(const DashPage *pg, int page_idx, int page_count,
                                  const SyncState *sync, const DashTheme *th) {
    char hdr[96];
    if (g_lock_render_mode != LOCK_NONE) {
        snprintf(hdr, sizeof(hdr), "%s", pg->title[0] ? pg->title : "KOBODASH");
        display_text(hdr, KD_FONT_BOLD, 20, KD_ALIGN_LEFT,
                     24, 10, 396, 40, th->fg, true, th->bg);
        display_text(g_lock_render_mode == LOCK_DEEP_SLEEP ? "SLEEPING" : "LOCKED",
                     KD_FONT_BOLD, 14, KD_ALIGN_CENTER,
                     428, 13, 108, 34, th->fg, true, th->card_bg);
        char l1[96], l2[96];
        if (g_lock_render_mode == LOCK_DEEP_SLEEP) {
            snprintf(l1, sizeof(l1), "SLEEPING  •  SLIDE POWER TO WAKE");
            if (g_lock_rotate_hrs > 0) {
                snprintf(l2, sizeof(l2), "Low-power standby  •  Screen rotates every %dh", g_lock_rotate_hrs);
            } else {
                snprintf(l2, sizeof(l2), "Low-power standby  •  Touch disabled");
            }
        } else {
            snprintf(l1, sizeof(l1), "TOUCH LOCKED  •  SLIDE POWER OR HOME TO UNLOCK");
            if (g_lock_rotate_hrs > 0) {
                snprintf(l2, sizeof(l2), "Desk mode active  •  Screen rotates every %dh", g_lock_rotate_hrs);
            } else {
                snprintf(l2, sizeof(l2), "Desk mode active  •  Accidental touches ignored");
            }
        }
        display_text(l1, KD_FONT_BOLD, 15, KD_ALIGN_CENTER,
                     24, 742, 552, 24, th->fg, true, th->bg);
        display_text(l2, KD_FONT_REGULAR, 14, KD_ALIGN_CENTER,
                     24, 768, 552, 24, th->muted, true, th->bg);
        return;
    }

    if (sync && sync->in_progress) {
        snprintf(hdr, sizeof(hdr), "%s  •  SYNCING...", pg->title[0] ? pg->title : "DASHBOARD");
    } else if (page_count > 1) {
        snprintf(hdr, sizeof(hdr), "%s  (%d/%d)", pg->title[0] ? pg->title : "DASHBOARD",
                 page_idx + 1, page_count);
    } else {
        snprintf(hdr, sizeof(hdr), "%s", pg->title[0] ? pg->title : "DASHBOARD");
    }
    display_text(hdr, KD_FONT_BOLD, 20, KD_ALIGN_LEFT,
                 24, 10, 444, 40, th->fg, true, th->bg);

    display_text("< PREV", KD_FONT_BOLD, 17, KD_ALIGN_LEFT,
                 24, 744, 120, 44, th->muted, true, th->bg);
    display_text("NEXT >", KD_FONT_BOLD, 17, KD_ALIGN_RIGHT,
                 456, 744, 120, 44, th->muted, true, th->bg);
    display_text("Hold HOME 2s for Stock UI", KD_FONT_REGULAR, 14, KD_ALIGN_CENTER,
                 150, 770, 300, 24, th->muted, true, th->bg);
}

static void draw_footer(const DashPage *pg, const SyncState *sync, const DashTheme *th) {
    char fbuf[112];
    if (sync && sync->last_ok == -1) {
        snprintf(fbuf, sizeof(fbuf), "%s  •  Offline cache", pg->footer[0] ? pg->footer : "Cached snapshot");
    } else if (sync && sync->push_connected) {
        snprintf(fbuf, sizeof(fbuf), "%s  •  Live push ON", pg->footer[0] ? pg->footer : "Synced");
    } else {
        snprintf(fbuf, sizeof(fbuf), "%s", pg->footer);
    }
    if (fbuf[0]) {
        display_text(fbuf, KD_FONT_REGULAR, 14, KD_ALIGN_CENTER,
                     24, 706, 552, 24, th->muted, true, th->bg);
    }
}

/* --- Page Kind 1: OVERVIEW (Today, Weather & Day/Week/Month/Year Progress) --- */
static void render_page_overview(Canvas *c, const DashboardBundle *b, int page_idx,
                                 const SyncState *sync, const Gallery *gal,
                                 const struct tm *now_tm, const DashTheme *th,
                                 KdRefreshMode mode) {
    const DashPage *pg = &b->pages[page_idx];
    DashBar live_bars[4];
    const DashBar *bars = pg->bars;
    int nbars = pg->bar_count;
    if (now_tm && now_tm->tm_year + 1900 >= 2020) {
        dashboard_compute_local_bars(now_tm, live_bars);
        bars = live_bars;
        nbars = 4;
    }

    canvas_clear(c, th->bg);
    draw_dash_chrome_shapes(c, b, page_idx, gal, th);

    /* Hero Weather Card */
    canvas_fill_rrect(c, 24, 68, 552, 102, 14, th->card_bg);
    canvas_stroke_rrect(c, 24, 68, 552, 102, 14, 2, th->border);
    canvas_vline(c, 190, 84, 70, 1, th->subtle);

    /* 2x2 Progress Cards */
    const int cx[2] = { 24, 308 };
    const int cy[2] = { 182, 276 };
    for (int i = 0; i < 4 && i < nbars; i++) {
        int x = cx[i & 1], y = cy[i >> 1];
        canvas_fill_rrect(c, x, y, 268, 84, 12, th->card_bg);
        canvas_stroke_rrect(c, x, y, 268, 84, 12, 2, th->border);
        canvas_draw_progress(c, x + 16, y + 48, 236, 18, bars[i].pct, th->fg, th->bg, th->border);
    }

    /* Bottom Details Card */
    canvas_fill_rrect(c, 24, 372, 552, 324, 14, th->card_bg);
    canvas_stroke_rrect(c, 24, 372, 552, 324, 14, 2, th->border);
    int nitems = pg->item_count > 5 ? 5 : pg->item_count;
    for (int i = 0; i < nitems; i++) {
        int ry = 412 + i * 56;
        if (i > 0) canvas_hline(c, 40, ry - 4, 520, 1, th->subtle);
        if (pg->items[i].urgent) {
            canvas_fill_rrect(c, 40, ry + 6, 102, 34, 8, th->pill_bg);
        }
    }

    display_blit_canvas(c, NULL);
    draw_dash_chrome_text(pg, page_idx, b->page_count, sync, th);

    /* Hero Weather Text */
    display_text(pg->hero[0] ? pg->hero : "--°C", KD_FONT_CLOCK, 54, KD_ALIGN_CENTER,
                 30, 84, 152, 68, th->fg, true, th->card_bg);
    display_text(pg->subtitle[0] ? pg->subtitle : "Overview", KD_FONT_BOLD, 20, KD_ALIGN_LEFT,
                 208, 82, 352, 30, th->fg, true, th->card_bg);
    display_text(pg->hero_sub[0] ? pg->hero_sub : "Weather telemetry", KD_FONT_REGULAR, 17, KD_ALIGN_LEFT,
                 208, 118, 352, 32, th->muted, true, th->card_bg);

    /* 2x2 Progress Labels */
    for (int i = 0; i < 4 && i < nbars; i++) {
        int x = cx[i & 1], y = cy[i >> 1];
        display_text(bars[i].label, KD_FONT_BOLD, 16, KD_ALIGN_LEFT,
                     x + 16, y + 12, 110, 26, th->muted, true, th->card_bg);
        display_text(bars[i].detail, KD_FONT_BOLD, 18, KD_ALIGN_RIGHT,
                     x + 120, y + 10, 132, 28, th->fg, true, th->card_bg);
    }

    /* Bottom Details Text */
    display_text("DAYLIGHT, FORECAST & TRANSIT", KD_FONT_BOLD, 15, KD_ALIGN_LEFT,
                 40, 382, 520, 24, th->muted, true, th->card_bg);
    for (int i = 0; i < nitems; i++) {
        int ry = 412 + i * 56;
        const DashItem *it = &pg->items[i];
        if (it->urgent) {
            display_text(it->label, KD_FONT_BOLD, 16, KD_ALIGN_CENTER,
                         40, ry + 6, 102, 34, th->pill_fg, true, th->pill_bg);
        } else {
            display_text(it->label, KD_FONT_BOLD, 17, KD_ALIGN_LEFT,
                         40, ry + 10, 106, 28, th->muted, true, th->card_bg);
        }
        display_text(it->value, KD_FONT_BOLD, 18, KD_ALIGN_LEFT,
                     154, ry + 8, 212, 30, th->fg, true, th->card_bg);
        display_text(it->detail, KD_FONT_REGULAR, 15, KD_ALIGN_RIGHT,
                     368, ry + 10, 192, 28, th->muted, true, th->card_bg);
    }

    draw_footer(pg, sync, th);
    display_refresh(NULL, mode);
}

/* --- Page Kind 2: CALENDAR & AGENDA --- */
static void render_page_calendar(Canvas *c, const DashboardBundle *b, int page_idx,
                                 const SyncState *sync, const Gallery *gal,
                                 const struct tm *now_tm, const DashTheme *th,
                                 KdRefreshMode mode) {
    const DashPage *pg = &b->pages[page_idx];
    int year = (pg->cal_year >= 2020) ? pg->cal_year : (now_tm ? now_tm->tm_year + 1900 : 2026);
    int month = (pg->cal_month >= 1 && pg->cal_month <= 12) ? pg->cal_month : (now_tm ? now_tm->tm_mon + 1 : 1);
    int today = (pg->cal_today >= 1 && pg->cal_today <= 31) ? pg->cal_today : (now_tm ? now_tm->tm_mday : 1);
    if (now_tm && now_tm->tm_year + 1900 == year && now_tm->tm_mon + 1 == month) {
        today = now_tm->tm_mday;
    }

    /* Compute weekday of 1st of month (Mon=0..Sun=6) */
    struct tm first;
    memset(&first, 0, sizeof(first));
    first.tm_year = year - 1900;
    first.tm_mon = month - 1;
    first.tm_mday = 1;
    mktime(&first);
    int start_col = (first.tm_wday + 6) % 7;
    int dim = days_in_month_calc(year, month);

    canvas_clear(c, th->bg);
    draw_dash_chrome_shapes(c, b, page_idx, gal, th);

    /* Month Calendar Card */
    canvas_fill_rrect(c, 24, 68, 552, 286, 14, th->card_bg);
    canvas_stroke_rrect(c, 24, 68, 552, 286, 14, 2, th->border);
    canvas_hline(c, 40, 134, 520, 1, th->subtle);

    const int col_x0 = 40;
    const int col_w = 74;
    const int row_y0 = 142;
    const int row_h = 33;
    for (int d = 1; d <= dim; d++) {
        int slot = start_col + (d - 1);
        int col = slot % 7;
        int row = slot / 7;
        if (row >= 6) break;
        int rx = col_x0 + col * col_w;
        int ry = row_y0 + row * row_h;
        if (d == today) {
            canvas_fill_rrect(c, rx + 13, ry + 1, 48, 28, 8, th->pill_bg);
        }
        if (pg->cal_marks_mask & (1u << (unsigned)(d - 1))) {
            uint8_t dot_c = (d == today) ? th->pill_fg : th->fg;
            canvas_fill_circle(c, rx + col_w / 2, ry + 26, 2, dot_c);
        }
    }

    /* Upcoming Events Card */
    canvas_fill_rrect(c, 24, 366, 552, 330, 14, th->card_bg);
    canvas_stroke_rrect(c, 24, 366, 552, 330, 14, 2, th->border);
    int nitems = pg->item_count > 5 ? 5 : pg->item_count;
    for (int i = 0; i < nitems; i++) {
        int ry = 406 + i * 56;
        if (i > 0) canvas_hline(c, 40, ry - 4, 520, 1, th->subtle);
        if (pg->items[i].urgent) {
            canvas_fill_rrect(c, 40, ry + 6, 148, 36, 8, th->pill_bg);
        } else {
            canvas_fill_rrect(c, 40, ry + 6, 148, 36, 8, th->bg);
            canvas_stroke_rrect(c, 40, ry + 6, 148, 36, 8, 1, th->border);
        }
    }

    display_blit_canvas(c, NULL);
    draw_dash_chrome_text(pg, page_idx, b->page_count, sync, th);

    /* Calendar Header & Weekday Names */
    display_text(pg->subtitle[0] ? pg->subtitle : "Calendar", KD_FONT_BOLD, 19, KD_ALIGN_LEFT,
                 40, 78, 520, 26, th->fg, true, th->card_bg);
    static const char *wdays[7] = { "MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN" };
    for (int col = 0; col < 7; col++) {
        display_text(wdays[col], KD_FONT_BOLD, 14, KD_ALIGN_CENTER,
                     col_x0 + col * col_w, 108, col_w, 22, th->muted, true, th->card_bg);
    }

    /* Calendar Day Numbers */
    for (int d = 1; d <= dim; d++) {
        int slot = start_col + (d - 1);
        int col = slot % 7;
        int row = slot / 7;
        if (row >= 6) break;
        int rx = col_x0 + col * col_w;
        int ry = row_y0 + row * row_h;
        char dbuf[8];
        snprintf(dbuf, sizeof(dbuf), "%d", d);
        bool is_td = (d == today);
        display_text(dbuf, is_td ? KD_FONT_BOLD : KD_FONT_REGULAR, 17, KD_ALIGN_CENTER,
                     rx, ry + 1, col_w, 24,
                     is_td ? th->pill_fg : th->fg, true, is_td ? th->pill_bg : th->card_bg);
    }

    /* Upcoming Events Text */
    display_text("UPCOMING AGENDA", KD_FONT_BOLD, 15, KD_ALIGN_LEFT,
                 40, 376, 520, 24, th->muted, true, th->card_bg);
    if (nitems == 0) {
        display_text("No upcoming events scheduled", KD_FONT_BOLD, 20, KD_ALIGN_CENTER,
                     40, 496, 520, 36, th->muted, true, th->card_bg);
    } else {
        for (int i = 0; i < nitems; i++) {
            int ry = 406 + i * 56;
            const DashItem *it = &pg->items[i];
            display_text(it->label, KD_FONT_BOLD, 15, KD_ALIGN_CENTER,
                         40, ry + 6, 148, 36,
                         it->urgent ? th->pill_fg : th->fg, true,
                         it->urgent ? th->pill_bg : th->bg);
            display_text(it->value, KD_FONT_BOLD, 18, KD_ALIGN_LEFT,
                         200, ry + 2, 360, 26, th->fg, true, th->card_bg);
            if (it->detail[0]) {
                display_text(it->detail, KD_FONT_REGULAR, 14, KD_ALIGN_LEFT,
                             200, ry + 28, 360, 20, th->muted, true, th->card_bg);
            }
        }
    }

    draw_footer(pg, sync, th);
    display_refresh(NULL, mode);
}

/* --- Page Kind 3: LIST (Waiting on You / Senden bekleyen) --- */
static void render_page_list(Canvas *c, const DashboardBundle *b, int page_idx,
                             const SyncState *sync, const Gallery *gal,
                             const DashTheme *th, KdRefreshMode mode) {
    const DashPage *pg = &b->pages[page_idx];
    canvas_clear(c, th->bg);
    draw_dash_chrome_shapes(c, b, page_idx, gal, th);

    /* Summary bar */
    canvas_fill_rrect(c, 24, 68, 552, 50, 12, th->card_bg);
    canvas_stroke_rrect(c, 24, 68, 552, 50, 12, 2, th->border);
    bool has_open = (pg->item_count > 0);
    if (has_open) {
        canvas_fill_rrect(c, 444, 75, 118, 36, 9, th->pill_bg);
    } else {
        canvas_fill_rrect(c, 444, 75, 118, 36, 9, th->bg);
        canvas_stroke_rrect(c, 444, 75, 118, 36, 9, 2, th->border);
    }

    int nitems = pg->item_count > 4 ? 4 : pg->item_count;
    if (nitems == 0) {
        canvas_fill_rrect(c, 24, 150, 552, 280, 14, th->card_bg);
        canvas_stroke_rrect(c, 24, 150, 552, 280, 14, 2, th->border);
    } else {
        for (int i = 0; i < nitems; i++) {
            int ry = 130 + i * 142;
            const DashItem *it = &pg->items[i];
            canvas_fill_rrect(c, 24, ry, 552, 130, 12, th->card_bg);
            canvas_stroke_rrect(c, 24, ry, 552, 130, 12, it->urgent ? 2 : 1, th->border);
            if (it->urgent) {
                canvas_fill_rrect(c, 40, ry + 14, 108, 30, 8, th->pill_bg);
            } else {
                canvas_fill_rrect(c, 40, ry + 14, 108, 30, 8, th->bg);
                canvas_stroke_rrect(c, 40, ry + 14, 108, 30, 8, 1, th->border);
            }
            if (font_is_ready() && it->detail[0]) {
                font_draw_wrapped(c, FONT_STYLE_UI_REG, 17, 40, ry + 56, 520, 66, 3,
                                  it->detail, th->muted);
            }
        }
    }

    display_blit_canvas(c, NULL);
    draw_dash_chrome_text(pg, page_idx, b->page_count, sync, th);

    display_text(pg->subtitle[0] ? pg->subtitle : "Open attention items", KD_FONT_BOLD, 17, KD_ALIGN_LEFT,
                 40, 74, 396, 38, th->fg, true, th->card_bg);
    display_text(pg->badge[0] ? pg->badge : (has_open ? "OPEN" : "CLEAR"),
                 KD_FONT_BOLD, 15, KD_ALIGN_CENTER,
                 444, 75, 118, 36,
                 has_open ? th->pill_fg : th->fg, true, has_open ? th->pill_bg : th->bg);

    if (nitems == 0) {
        display_text("All Clear", KD_FONT_BOLD, 34, KD_ALIGN_CENTER,
                     40, 224, 520, 48, th->fg, true, th->card_bg);
        display_text("Nothing is waiting on your decision right now.",
                     KD_FONT_REGULAR, 19, KD_ALIGN_CENTER,
                     40, 290, 520, 38, th->muted, true, th->card_bg);
    } else {
        for (int i = 0; i < nitems; i++) {
            int ry = 130 + i * 142;
            const DashItem *it = &pg->items[i];
            display_text(it->label[0] ? it->label : "OPEN", KD_FONT_BOLD, 14, KD_ALIGN_CENTER,
                         40, ry + 14, 108, 30,
                         it->urgent ? th->pill_fg : th->fg, true,
                         it->urgent ? th->pill_bg : th->bg);
            display_text(it->value, KD_FONT_BOLD, 19, KD_ALIGN_LEFT,
                         160, ry + 14, 400, 32, th->fg, true, th->card_bg);
            if (!font_is_ready() && it->detail[0]) {
                display_text(it->detail, KD_FONT_REGULAR, 16, KD_ALIGN_LEFT,
                             40, ry + 56, 520, 62, th->muted, true, th->card_bg);
            }
        }
    }

    draw_footer(pg, sync, th);
    display_refresh(NULL, mode);
}

/* --- Page Kind 4: STATS & SPORTS --- */
static void render_page_stats(Canvas *c, const DashboardBundle *b, int page_idx,
                              const SyncState *sync, const Gallery *gal,
                              const DashTheme *th, KdRefreshMode mode) {
    const DashPage *pg = &b->pages[page_idx];
    canvas_clear(c, th->bg);
    draw_dash_chrome_shapes(c, b, page_idx, gal, th);

    /* 2x2 KPI Metric Cards */
    const int mx[2] = { 24, 308 };
    const int my[2] = { 68, 212 };
    for (int i = 0; i < 4 && i < pg->metric_count; i++) {
        int x = mx[i & 1], y = my[i >> 1];
        canvas_fill_rrect(c, x, y, 268, 132, 12, th->card_bg);
        canvas_stroke_rrect(c, x, y, 268, 132, 12, 2, th->border);
    }

    /* Sports & Milestones Card */
    canvas_fill_rrect(c, 24, 358, 552, 338, 14, th->card_bg);
    canvas_stroke_rrect(c, 24, 358, 552, 338, 14, 2, th->border);
    int nitems = pg->item_count > 4 ? 4 : pg->item_count;
    for (int i = 0; i < nitems; i++) {
        int ry = 404 + i * 72;
        if (i > 0) canvas_hline(c, 40, ry - 6, 520, 1, th->subtle);
        if (pg->items[i].urgent) {
            canvas_fill_rrect(c, 40, ry + 10, 118, 40, 9, th->pill_bg);
        } else {
            canvas_fill_rrect(c, 40, ry + 10, 118, 40, 9, th->bg);
            canvas_stroke_rrect(c, 40, ry + 10, 118, 40, 9, 1, th->border);
        }
    }

    display_blit_canvas(c, NULL);
    draw_dash_chrome_text(pg, page_idx, b->page_count, sync, th);

    /* KPI Metrics Text */
    for (int i = 0; i < 4 && i < pg->metric_count; i++) {
        int x = mx[i & 1], y = my[i >> 1];
        const DashMetric *m = &pg->metrics[i];
        display_text(m->label, KD_FONT_BOLD, 14, KD_ALIGN_LEFT,
                     x + 16, y + 12, 236, 22, th->muted, true, th->card_bg);
        display_text(m->value[0] ? m->value : "-", KD_FONT_CLOCK, 40, KD_ALIGN_LEFT,
                     x + 16, y + 40, 236, 48, th->fg, true, th->card_bg);
        display_text(m->detail, KD_FONT_REGULAR, 15, KD_ALIGN_LEFT,
                     x + 16, y + 96, 236, 24, th->muted, true, th->card_bg);
    }

    /* Sports & Milestones Text */
    display_text("SPORTS & MILESTONES", KD_FONT_BOLD, 15, KD_ALIGN_LEFT,
                 40, 370, 520, 24, th->muted, true, th->card_bg);
    for (int i = 0; i < nitems; i++) {
        int ry = 404 + i * 72;
        const DashItem *it = &pg->items[i];
        display_text(it->label, KD_FONT_BOLD, 16, KD_ALIGN_CENTER,
                     40, ry + 10, 118, 40,
                     it->urgent ? th->pill_fg : th->fg, true,
                     it->urgent ? th->pill_bg : th->bg);
        display_text(it->value, KD_FONT_BOLD, 18, KD_ALIGN_LEFT,
                     172, ry + 6, 388, 28, th->fg, true, th->card_bg);
        display_text(it->detail, KD_FONT_REGULAR, 15, KD_ALIGN_LEFT,
                     172, ry + 36, 388, 24, th->muted, true, th->card_bg);
    }

    draw_footer(pg, sync, th);
    display_refresh(NULL, mode);
}

/* --- Page Kind 0: GENERIC --- */
static void render_page_generic(Canvas *c, const DashboardBundle *b, int page_idx,
                                const SyncState *sync, const Gallery *gal,
                                const DashTheme *th, KdRefreshMode mode) {
    const DashPage *pg = &b->pages[page_idx];
    canvas_clear(c, th->bg);
    draw_dash_chrome_shapes(c, b, page_idx, gal, th);

    canvas_fill_rrect(c, 24, 78, 552, 620, 14, th->card_bg);
    canvas_stroke_rrect(c, 24, 78, 552, 620, 14, 2, th->border);
    canvas_hline(c, 44, 174, 512, 2, th->subtle);
    display_blit_canvas(c, NULL);

    draw_dash_chrome_text(pg, page_idx, b->page_count, sync, th);
    display_text(pg->title, KD_FONT_BOLD, 30, KD_ALIGN_LEFT,
                 44, 92, 512, 42, th->fg, true, th->card_bg);
    if (pg->subtitle[0]) {
        display_text(pg->subtitle, KD_FONT_REGULAR, 19, KD_ALIGN_LEFT,
                     44, 138, 512, 28, th->muted, true, th->card_bg);
    }
    for (int i = 0; i < pg->item_count && i < KD_MAX_PAGE_ITEMS; i++) {
        display_text(pg->items[i].value, KD_FONT_REGULAR, 21, KD_ALIGN_LEFT,
                     44, 194 + i * 54, 512, 42, th->fg, true, th->card_bg);
    }
    draw_footer(pg, sync, th);
    display_refresh(NULL, mode);
}

void dashboard_render_page(Canvas *c, const DashboardBundle *b, int page_idx,
                           const Config *cfg, const Telemetry *tel,
                           const SyncState *sync, const Gallery *gal,
                           const struct tm *now_tm, KdRefreshMode mode) {
    (void)tel;
    if (!b || !b->has_bundle || b->page_count <= 0) return;
    if (page_idx < 0 || page_idx >= b->page_count) page_idx = 0;
    DashTheme th = dash_theme(cfg);

    switch (b->pages[page_idx].kind) {
        case DASH_PAGE_OVERVIEW:
            render_page_overview(c, b, page_idx, sync, gal, now_tm, &th, mode);
            break;
        case DASH_PAGE_CALENDAR:
            render_page_calendar(c, b, page_idx, sync, gal, now_tm, &th, mode);
            break;
        case DASH_PAGE_LIST:
            render_page_list(c, b, page_idx, sync, gal, &th, mode);
            break;
        case DASH_PAGE_STATS:
            render_page_stats(c, b, page_idx, sync, gal, &th, mode);
            break;
        default:
            render_page_generic(c, b, page_idx, sync, gal, &th, mode);
            break;
    }
}

static int find_page_by_kind(const DashboardBundle *b, DashPageKind kind) {
    if (!b || !b->has_bundle) return -1;
    for (int i = 0; i < b->page_count; i++) {
        if (b->pages[i].kind == kind) return i;
    }
    return -1;
}

int dashboard_pick_sleep_page(const DashboardBundle *b, int sleep_style, int cycle_idx) {
    if (!b || !b->has_bundle || b->page_count <= 0 || sleep_style == SLEEP_STYLE_CLOCK) {
        return -1;
    }
    if (sleep_style == SLEEP_STYLE_TODAY) {
        int idx = find_page_by_kind(b, DASH_PAGE_OVERVIEW);
        return idx >= 0 ? idx : 0;
    }
    if (sleep_style == SLEEP_STYLE_CALENDAR) {
        int idx = find_page_by_kind(b, DASH_PAGE_CALENDAR);
        return idx >= 0 ? idx : 0;
    }
    if (sleep_style == SLEEP_STYLE_STATS) {
        int idx = find_page_by_kind(b, DASH_PAGE_STATS);
        return idx >= 0 ? idx : 0;
    }
    /* SLEEP_STYLE_CYCLE: rotate cleanly across Today & Progress -> Calendar -> Stats */
    static const DashPageKind order[3] = {
        DASH_PAGE_OVERVIEW, DASH_PAGE_CALENDAR, DASH_PAGE_STATS
    };
    int avail[3];
    int navail = 0;
    for (int i = 0; i < 3; i++) {
        int idx = find_page_by_kind(b, order[i]);
        if (idx >= 0) avail[navail++] = idx;
    }
    if (navail > 0) {
        int slot = cycle_idx % navail;
        if (slot < 0) slot += navail;
        return avail[slot];
    }
    return 0;
}

void dashboard_render_sleep_screen(Canvas *c, const DashboardBundle *b,
                                   int sleep_style, int cycle_idx, LockMode lock_mode,
                                   const Config *cfg, const Telemetry *tel,
                                   const SyncState *sync, const struct tm *now_tm,
                                   KdRefreshMode mode) {
    int page_idx = dashboard_pick_sleep_page(b, sleep_style, cycle_idx);
    if (page_idx >= 0) {
        g_lock_render_mode = (lock_mode == LOCK_NONE) ? LOCK_DEEP_SLEEP : lock_mode;
        g_lock_rotate_hrs = (cfg && sleep_style == SLEEP_STYLE_CYCLE) ? cfg->sleep_rotate_hrs : 0;
        dashboard_render_page(c, b, page_idx, cfg, tel, sync, NULL, now_tm, mode);
        g_lock_render_mode = LOCK_NONE;
        return;
    }

    /* Minimal Clock Lock Screen fallback (when SLEEP_STYLE_CLOCK or offline without cached bundle) */
    DashTheme th = dash_theme(cfg);
    canvas_clear(c, th.bg);
    canvas_hline(c, 24, 58, 552, 2, th.subtle);
    canvas_fill_rrect(c, 428, 13, 108, 34, 9, th.card_bg);
    canvas_stroke_rrect(c, 428, 13, 108, 34, 9, 1, th.border);
    canvas_draw_lock(c, 558, 30, th.fg, th.bg);
    canvas_hline(c, 240, 444, 120, 3, th.fg);
    canvas_hline(c, 24, 734, 552, 1, th.subtle);
    display_blit_canvas(c, NULL);

    display_text("KOBODASH", KD_FONT_BOLD, 20, KD_ALIGN_LEFT,
                 24, 10, 380, 40, th.fg, true, th.bg);
    display_text(lock_mode == LOCK_DEEP_SLEEP ? "SLEEPING" : "LOCKED",
                 KD_FONT_BOLD, 14, KD_ALIGN_CENTER,
                 428, 13, 108, 34, th.fg, true, th.card_bg);

    char tbuf[16] = "--:--", dow[32] = "", dstr[64] = "";
    if (now_tm) {
        int h = now_tm->tm_hour;
        if (cfg && !cfg->clock_24h) {
            h %= 12;
            if (h == 0) h = 12;
            snprintf(tbuf, sizeof(tbuf), "%d:%02d", h, now_tm->tm_min);
        } else {
            snprintf(tbuf, sizeof(tbuf), "%02d:%02d", h, now_tm->tm_min);
        }
        strftime(dow, sizeof(dow), "%A", now_tm);
        for (char *p = dow; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
        strftime(dstr, sizeof(dstr), "%d %B %Y", now_tm);
    }
    display_text(tbuf, KD_FONT_CLOCK, 188, KD_ALIGN_CENTER,
                 24, 180, 552, 232, th.fg, true, th.bg);
    display_text(dow, KD_FONT_BOLD, 30, KD_ALIGN_CENTER,
                 24, 468, 552, 42, th.fg, true, th.bg);
    display_text(dstr, KD_FONT_REGULAR, 24, KD_ALIGN_CENTER,
                 24, 516, 552, 36, th.muted, true, th.bg);

    char binfo[64];
    snprintf(binfo, sizeof(binfo), "Battery %d%%", tel && tel->bat_pct >= 0 ? tel->bat_pct : 0);
    display_text(binfo, KD_FONT_REGULAR, 17, KD_ALIGN_CENTER,
                 24, 620, 552, 28, th.muted, true, th.bg);

    display_text(lock_mode == LOCK_DEEP_SLEEP
                     ? "SLEEPING  •  SLIDE POWER TO WAKE"
                     : "TOUCH LOCKED  •  SLIDE POWER OR HOME TO UNLOCK",
                 KD_FONT_BOLD, 15, KD_ALIGN_CENTER,
                 24, 742, 552, 24, th.fg, true, th.bg);
    display_text("Touchscreen locked", KD_FONT_REGULAR, 14, KD_ALIGN_CENTER,
                 24, 768, 552, 24, th.muted, true, th.bg);
    display_refresh(NULL, mode);
}

/* ============================================================================
 * High-Contrast Attention Alert Overlay (Remote Push & Offline Event Triggers)
 * ============================================================================ */

void dashboard_render_alert_overlay(Canvas *c, const ActiveAlert *al,
                                    const Config *cfg, KdRefreshMode mode) {
    if (!al || !al->active) return;
    DashTheme th = dash_theme(cfg);

    canvas_clear(c, th.bg);

    /* Centered outer modal frame */
    canvas_fill_rrect(c, 32, 118, 536, 540, 16, th.card_bg);
    canvas_stroke_rrect(c, 32, 118, 536, 540, 16, 3, th.fg);

    /* Solid high-contrast top banner inside card */
    canvas_fill_rrect(c, 32, 118, 536, 72, 16, th.pill_bg);
    canvas_fill_rect(c, 32, 164, 536, 26, th.pill_bg);

    /* Priority badge pill on right of banner */
    canvas_fill_rrect(c, 424, 136, 124, 36, 9, th.bg);

    /* Divider above buttons */
    canvas_hline(c, 56, 544, 488, 1, th.subtle);

    /* 3 Action buttons: DISMISS | SNOOZE 30m | VIEW ALL */
    canvas_fill_rrect(c, 56, 566, 152, 66, 12, th.pill_bg);
    canvas_fill_rrect(c, 222, 566, 164, 66, 12, th.bg);
    canvas_stroke_rrect(c, 222, 566, 164, 66, 12, 2, th.border);
    canvas_fill_rrect(c, 400, 566, 144, 66, 12, th.bg);
    canvas_stroke_rrect(c, 400, 566, 144, 66, 12, 2, th.border);

    /* Wrapped body directly onto canvas if TTF fonts are loaded */
    if (font_is_ready() && al->body[0]) {
        font_draw_wrapped(c, FONT_STYLE_UI_REG, 22, 58, 304, 484, 176, 6, al->body, th.fg);
    }

    display_blit_canvas(c, NULL);

    display_text(al->is_remote ? "WAITING ON YOU" : "CALENDAR REMINDER",
                 KD_FONT_BOLD, 24, KD_ALIGN_LEFT,
                 56, 134, 360, 40, th.pill_fg, true, th.pill_bg);
    display_text(al->badge[0] ? al->badge : "ALERT",
                 KD_FONT_BOLD, 16, KD_ALIGN_CENTER,
                 424, 136, 124, 36, th.fg, true, th.bg);

    display_text(al->title, KD_FONT_BOLD, 26, KD_ALIGN_LEFT,
                 58, 214, 484, 72, th.fg, true, th.card_bg);

    if (!font_is_ready() && al->body[0]) {
        display_text(al->body, KD_FONT_REGULAR, 20, KD_ALIGN_LEFT,
                     58, 304, 484, 160, th.fg, true, th.card_bg);
    }

    display_text(al->meta, KD_FONT_REGULAR, 16, KD_ALIGN_LEFT,
                 58, 496, 484, 30, th.muted, true, th.card_bg);

    display_text("DISMISS", KD_FONT_BOLD, 19, KD_ALIGN_CENTER,
                 56, 566, 152, 66, th.pill_fg, true, th.pill_bg);
    display_text("SNOOZE 30m", KD_FONT_BOLD, 18, KD_ALIGN_CENTER,
                 222, 566, 164, 66, th.fg, true, th.bg);
    display_text("VIEW ALL", KD_FONT_BOLD, 18, KD_ALIGN_CENTER,
                 400, 566, 144, 66, th.fg, true, th.bg);

    display_text("Press HOME to dismiss  •  Hold HOME 2s for Stock UI",
                 KD_FONT_REGULAR, 14, KD_ALIGN_CENTER,
                 24, 700, 552, 28, th.muted, true, th.bg);

    display_refresh(NULL, mode);
}

int dashboard_handle_alert_input(const ActiveAlert *al, const InputEvent *ev) {
    if (!al || !al->active || !ev) return 0;
    if (ev->type == INPUT_HOME_LONG) return 4;
    if (ev->type == INPUT_HOME_SHORT || ev->type == INPUT_SWIPE_LEFT || ev->type == INPUT_SWIPE_RIGHT) {
        return 1; /* dismiss */
    }
    if (ev->type != INPUT_TAP) return 0;

    int x = ev->x, y = ev->y;
    if (y >= 556 && y <= 646) {
        if (x >= 48 && x <= 214) return 1;  /* DISMISS */
        if (x >= 216 && x <= 392) return 2; /* SNOOZE 30m */
        if (x >= 394 && x <= 556) return 3; /* VIEW ALL */
    }
    return 0;
}
