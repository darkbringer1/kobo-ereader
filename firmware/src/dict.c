#include "kobodash.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct Dict {
    int fd;
    const uint8_t *base;
    size_t size;
    uint32_t count;
    uint32_t index_off;
};

struct DictSet {
    Dict *dicts[KD_MAX_DICTS];
    char ids[KD_MAX_DICTS][16];
    char labels[KD_MAX_DICTS][24];
    int count;
};

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

Dict *dict_open(const char *path) {
    if (!path || !path[0]) return NULL;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 16) {
        close(fd);
        return NULL;
    }

    size_t sz = (size_t)st.st_size;
    void *map = mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return NULL;
    }

    const uint8_t *base = (const uint8_t *)map;
    if (memcmp(base, "KDDICT01", 8) != 0) {
        munmap(map, sz);
        close(fd);
        return NULL;
    }

    uint32_t count = rd_u32(base + 8);
    uint32_t index_off = rd_u32(base + 12);
    if (count == 0 || (size_t)index_off + (size_t)count * 4u > sz) {
        munmap(map, sz);
        close(fd);
        return NULL;
    }

    Dict *d = calloc(1, sizeof(*d));
    if (!d) {
        munmap(map, sz);
        close(fd);
        return NULL;
    }
    d->fd = fd;
    d->base = base;
    d->size = sz;
    d->count = count;
    d->index_off = index_off;
    return d;
}

void dict_close(Dict *d) {
    if (!d) return;
    if (d->base && d->size > 0) munmap((void *)d->base, d->size);
    if (d->fd >= 0) close(d->fd);
    free(d);
}

uint32_t dict_entry_count(const Dict *d) {
    return d ? d->count : 0;
}

static bool dict_find_exact(const Dict *d, const char *word, char *def_out, size_t def_sz) {
    if (!d || !word || !word[0]) return false;
    size_t qlen = strlen(word);

    uint32_t lo = 0;
    uint32_t hi = d->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t off = rd_u32(d->base + d->index_off + mid * 4u);
        if ((size_t)off + 3u > d->size) return false;
        uint8_t wlen = d->base[off];
        uint16_t dlen = rd_u16(d->base + off + 1);
        if ((size_t)off + 3u + wlen + dlen > d->size) return false;

        const char *wptr = (const char *)(d->base + off + 3u);
        size_t mlen = qlen < wlen ? qlen : wlen;
        int cmp = memcmp(word, wptr, mlen);
        if (cmp == 0) {
            if (qlen == wlen) {
                if (def_out && def_sz > 0) {
                    size_t cpy = dlen < def_sz - 1 ? dlen : def_sz - 1;
                    memcpy(def_out, d->base + off + 3u + wlen, cpy);
                    def_out[cpy] = '\0';
                }
                return true;
            }
            cmp = (qlen < wlen) ? -1 : 1;
        }
        if (cmp < 0) hi = mid;
        else lo = mid + 1;
    }
    return false;
}

/* UTF-8 letter classifier & lowercaser covering ASCII + Latin-1 / German (ÄÖÜäöüß, etc.) */
static bool is_utf8_letter(const unsigned char *p, int *adv_bytes) {
    if (p[0] < 0x80) {
        *adv_bytes = 1;
        return isalpha(p[0]) != 0;
    }
    /* UTF-8 U+00C0..U+00FF -> 0xC3 0x80..0xBF */
    if (p[0] == 0xC3 && p[1] >= 0x80 && p[1] <= 0xBF && p[1] != 0x97 && p[1] != 0xB7) {
        *adv_bytes = 2;
        return true;
    }
    /* Latin Extended-A (U+0100..U+017F -> 0xC4/0xC5) */
    if ((p[0] == 0xC4 || p[0] == 0xC5) && (p[1] & 0xC0) == 0x80) {
        *adv_bytes = 2;
        return true;
    }
    *adv_bytes = 1;
    return false;
}

static void clean_word_utf8(const char *raw, char *out, size_t out_sz) {
    if (!raw || !out || out_sz == 0) return;
    size_t i = 0;
    const unsigned char *p = (const unsigned char *)raw;

    /* Skip leading punctuation / quotes */
    while (*p) {
        int adv = 1;
        if (is_utf8_letter(p, &adv)) break;
        p += adv;
    }

    while (*p && i + 2 < out_sz) {
        int adv = 1;
        if (p[0] < 0x80) {
            if (isalpha(p[0])) {
                out[i++] = (char)tolower(p[0]);
                p++;
            } else if (p[0] == '-' || p[0] == '\'') {
                out[i++] = (char)p[0];
                p++;
            } else {
                break;
            }
        } else if (p[0] == 0xC3 && p[1] >= 0x80 && p[1] <= 0xBF && p[1] != 0x97 && p[1] != 0xB7) {
            unsigned char b2 = p[1];
            /* Lowercase U+00C0..U+00D6 (0x80..0x96) and U+00D8..U+00DE (0x98..0x9E) -> +0x20 */
            /* Note: Ä=C3 84 -> ä=C3 A4, Ö=C3 96 -> ö=C3 B6, Ü=C3 9C -> ü=C3 BC, ß=C3 9F stays */
            if ((b2 >= 0x80 && b2 <= 0x96) || (b2 >= 0x98 && b2 <= 0x9E)) {
                b2 = (unsigned char)(b2 + 0x20);
            }
            out[i++] = (char)0xC3;
            out[i++] = (char)b2;
            p += 2;
        } else if ((p[0] == 0xC4 || p[0] == 0xC5) && (p[1] & 0xC0) == 0x80) {
            out[i++] = (char)p[0];
            out[i++] = (char)p[1];
            p += 2;
        } else if (p[0] == 0xE2 && p[1] == 0x80 && (p[2] == 0x99 || p[2] == 0x98)) {
            out[i++] = '\'';
            p += 3;
        } else {
            break;
        }
        (void)adv;
    }
    while (i > 0 && (out[i - 1] == '-' || out[i - 1] == '\'')) {
        i--;
    }
    out[i] = '\0';
}

static bool try_candidate(const Dict *d, const char *cand,
                          char *matched, size_t matched_sz,
                          char *def_out, size_t def_sz) {
    if (!cand || strlen(cand) < 2) return false;
    if (dict_find_exact(d, cand, def_out, def_sz)) {
        if (matched && matched_sz > 0) snprintf(matched, matched_sz, "%s", cand);
        return true;
    }
    return false;
}

static void replace_ss_eszett(const char *src, char *dst, size_t dst_sz, bool to_ss) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 2 < dst_sz; ) {
        if (to_ss && (unsigned char)src[i] == 0xC3 && (unsigned char)src[i + 1] == 0x9F) {
            dst[j++] = 's'; dst[j++] = 's'; i += 2;
        } else if (!to_ss && src[i] == 's' && src[i + 1] == 's') {
            dst[j++] = (char)0xC3; dst[j++] = (char)0x9F; i += 2;
        } else {
            dst[j++] = src[i++];
        }
    }
    dst[j] = '\0';
}

static void strip_german_umlauts(const char *src, char *dst, size_t dst_sz) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < dst_sz; ) {
        if ((unsigned char)src[i] == 0xC3) {
            unsigned char b2 = (unsigned char)src[i + 1];
            if (b2 == 0xA4) { dst[j++] = 'a'; i += 2; continue; } /* ä */
            if (b2 == 0xB6) { dst[j++] = 'o'; i += 2; continue; } /* ö */
            if (b2 == 0xBC) { dst[j++] = 'u'; i += 2; continue; } /* ü */
        }
        dst[j++] = src[i++];
    }
    dst[j] = '\0';
}

static bool lookup_german_lemmas(const Dict *d, const char *w,
                                 char *matched, size_t matched_sz,
                                 char *def_out, size_t def_sz) {
    size_t len = strlen(w);
    char cand[96];

    /* 1. ß <-> ss variants */
    replace_ss_eszett(w, cand, sizeof(cand), true);
    if (strcmp(cand, w) != 0 && try_candidate(d, cand, matched, matched_sz, def_out, def_sz)) return true;
    replace_ss_eszett(w, cand, sizeof(cand), false);
    if (strcmp(cand, w) != 0 && try_candidate(d, cand, matched, matched_sz, def_out, def_sz)) return true;

    /* 2. Strip common German declension / conjugation endings */
    static const char *const suffixes[] = {
        "esten", "sten", "ende", "enden", "ender", "endes",
        "test", "ten", "tet", "est", "end",
        "en", "em", "er", "es", "te", "st", "e", "n", "s", "t", NULL
    };
    for (int s = 0; suffixes[s]; s++) {
        size_t slen = strlen(suffixes[s]);
        if (len > slen + 2 && strcmp(w + len - slen, suffixes[s]) == 0) {
            /* Stem alone (nouns/adjectives: Hauses -> Haus, alten -> alt) */
            snprintf(cand, sizeof(cand), "%.*s", (int)(len - slen), w);
            if (try_candidate(d, cand, matched, matched_sz, def_out, def_sz)) return true;

            /* Stem + "en" (verb infinitive: machte -> machen, kämpften -> kämpfen) */
            snprintf(cand, sizeof(cand), "%.*sen", (int)(len - slen), w);
            if (try_candidate(d, cand, matched, matched_sz, def_out, def_sz)) return true;

            /* Stem + "e" (nouns ending in -e: Lampen -> Lampe) */
            snprintf(cand, sizeof(cand), "%.*se", (int)(len - slen), w);
            if (try_candidate(d, cand, matched, matched_sz, def_out, def_sz)) return true;

            /* Umlaut-stripped stem (Häuser -> Haus, Bäume -> Baum, Männer -> Mann, läuft -> laufen) */
            char deum[96];
            snprintf(cand, sizeof(cand), "%.*s", (int)(len - slen), w);
            strip_german_umlauts(cand, deum, sizeof(deum));
            if (strcmp(deum, cand) != 0) {
                if (try_candidate(d, deum, matched, matched_sz, def_out, def_sz)) return true;
                char inf[104];
                snprintf(inf, sizeof(inf), "%sen", deum);
                if (try_candidate(d, inf, matched, matched_sz, def_out, def_sz)) return true;
            }
        }
    }

    /* 3. Past participle ge-...-t / ge-...-en (gemacht -> machen, gekämpft -> kämpfen) */
    if (len > 5 && strncmp(w, "ge", 2) == 0) {
        const char *rest = w + 2;
        size_t rlen = len - 2;
        if (rlen > 2 && rest[rlen - 1] == 't') {
            snprintf(cand, sizeof(cand), "%.*sen", (int)(rlen - 1), rest);
            if (try_candidate(d, cand, matched, matched_sz, def_out, def_sz)) return true;
        }
        if (try_candidate(d, rest, matched, matched_sz, def_out, def_sz)) return true;
    }

    return false;
}

static bool lookup_english_lemmas(const Dict *d, const char *w_in,
                                  char *matched, size_t matched_sz,
                                  char *def_out, size_t def_sz) {
    char w[96];
    snprintf(w, sizeof(w), "%s", w_in);
    size_t len = strlen(w);
    char cand[96];

    if (len > 3 && strcmp(w + len - 2, "'s") == 0) {
        snprintf(cand, sizeof(cand), "%.*s", (int)(len - 2), w);
        if (try_candidate(d, cand, matched, matched_sz, def_out, def_sz)) return true;
        snprintf(w, sizeof(w), "%s", cand);
        len = strlen(w);
    }

    if (len > 4 && strcmp(w + len - 3, "ies") == 0) {
        snprintf(cand, sizeof(cand), "%.*sy", (int)(len - 3), w);
        if (try_candidate(d, cand, matched, matched_sz, def_out, def_sz)) return true;
    }
    if (len > 4 && strcmp(w + len - 3, "ves") == 0) {
        snprintf(cand, sizeof(cand), "%.*sf", (int)(len - 3), w);
        if (try_candidate(d, cand, matched, matched_sz, def_out, def_sz)) return true;
        snprintf(cand, sizeof(cand), "%.*sfe", (int)(len - 3), w);
        if (try_candidate(d, cand, matched, matched_sz, def_out, def_sz)) return true;
    }
    if (len > 3 && strcmp(w + len - 2, "es") == 0) {
        snprintf(cand, sizeof(cand), "%.*s", (int)(len - 2), w);
        if (try_candidate(d, cand, matched, matched_sz, def_out, def_sz)) return true;
    }
    if (len > 2 && w[len - 1] == 's' && w[len - 2] != 's') {
        snprintf(cand, sizeof(cand), "%.*s", (int)(len - 1), w);
        if (try_candidate(d, cand, matched, matched_sz, def_out, def_sz)) return true;
    }

    if (len > 4 && strcmp(w + len - 3, "ied") == 0) {
        snprintf(cand, sizeof(cand), "%.*sy", (int)(len - 3), w);
        if (try_candidate(d, cand, matched, matched_sz, def_out, def_sz)) return true;
    }
    if (len > 3 && strcmp(w + len - 2, "ed") == 0) {
        snprintf(cand, sizeof(cand), "%.*s", (int)(len - 2), w);
        if (try_candidate(d, cand, matched, matched_sz, def_out, def_sz)) return true;
        snprintf(cand, sizeof(cand), "%.*se", (int)(len - 2), w);
        if (try_candidate(d, cand, matched, matched_sz, def_out, def_sz)) return true;
        if (len > 4 && w[len - 3] == w[len - 4]) {
            snprintf(cand, sizeof(cand), "%.*s", (int)(len - 3), w);
            if (try_candidate(d, cand, matched, matched_sz, def_out, def_sz)) return true;
        }
    }

    if (len > 4 && strcmp(w + len - 3, "ing") == 0) {
        snprintf(cand, sizeof(cand), "%.*s", (int)(len - 3), w);
        if (try_candidate(d, cand, matched, matched_sz, def_out, def_sz)) return true;
        snprintf(cand, sizeof(cand), "%.*se", (int)(len - 3), w);
        if (try_candidate(d, cand, matched, matched_sz, def_out, def_sz)) return true;
        if (len > 5 && w[len - 4] == w[len - 5]) {
            snprintf(cand, sizeof(cand), "%.*s", (int)(len - 4), w);
            if (try_candidate(d, cand, matched, matched_sz, def_out, def_sz)) return true;
        }
    }

    if (len > 4 && strcmp(w + len - 3, "ily") == 0) {
        snprintf(cand, sizeof(cand), "%.*sy", (int)(len - 3), w);
        if (try_candidate(d, cand, matched, matched_sz, def_out, def_sz)) return true;
    }
    if (len > 3 && strcmp(w + len - 2, "ly") == 0) {
        snprintf(cand, sizeof(cand), "%.*s", (int)(len - 2), w);
        if (try_candidate(d, cand, matched, matched_sz, def_out, def_sz)) return true;
    }
    if (len > 4 && strcmp(w + len - 3, "est") == 0) {
        snprintf(cand, sizeof(cand), "%.*s", (int)(len - 3), w);
        if (try_candidate(d, cand, matched, matched_sz, def_out, def_sz)) return true;
        snprintf(cand, sizeof(cand), "%.*se", (int)(len - 3), w);
        if (try_candidate(d, cand, matched, matched_sz, def_out, def_sz)) return true;
    }
    if (len > 3 && strcmp(w + len - 2, "er") == 0) {
        snprintf(cand, sizeof(cand), "%.*s", (int)(len - 2), w);
        if (try_candidate(d, cand, matched, matched_sz, def_out, def_sz)) return true;
        snprintf(cand, sizeof(cand), "%.*se", (int)(len - 2), w);
        if (try_candidate(d, cand, matched, matched_sz, def_out, def_sz)) return true;
    }

    char *dash = strchr(w, '-');
    if (dash && dash > w && dash[1]) {
        if (try_candidate(d, dash + 1, matched, matched_sz, def_out, def_sz)) return true;
        snprintf(cand, sizeof(cand), "%.*s", (int)(dash - w), w);
        if (try_candidate(d, cand, matched, matched_sz, def_out, def_sz)) return true;
    }
    return false;
}

bool dict_lookup_lang(const Dict *d, const char *lang_id, const char *raw_word,
                      char *matched_word, size_t matched_sz,
                      char *def_out, size_t def_sz) {
    if (!d || !raw_word) return false;
    char w[96];
    clean_word_utf8(raw_word, w, sizeof(w));
    if (w[0] == '\0') return false;

    if (try_candidate(d, w, matched_word, matched_sz, def_out, def_sz)) return true;

    bool is_german = (lang_id && strncmp(lang_id, "de", 2) == 0);
    if (is_german) {
        return lookup_german_lemmas(d, w, matched_word, matched_sz, def_out, def_sz);
    }
    return lookup_english_lemmas(d, w, matched_word, matched_sz, def_out, def_sz);
}

bool dict_lookup(const Dict *d, const char *raw_word,
                 char *matched_word, size_t matched_sz,
                 char *def_out, size_t def_sz) {
    return dict_lookup_lang(d, "en", raw_word, matched_word, matched_sz, def_out, def_sz);
}

/* =========================================================================
 * Multi-dictionary container (EN, EN->DE, DE->EN)
 * ========================================================================= */

static void try_add_dict(DictSet *ds, const char *dir, const char *file,
                         const char *id, const char *label) {
    if (ds->count >= KD_MAX_DICTS) return;
    char full[360];
    snprintf(full, sizeof(full), "%s/%s", dir, file);
    Dict *d = dict_open(full);
    if (!d) return;
    int i = ds->count++;
    ds->dicts[i] = d;
    snprintf(ds->ids[i], sizeof(ds->ids[0]), "%s", id);
    snprintf(ds->labels[i], sizeof(ds->labels[0]), "%s", label);
}

DictSet *dictset_open_dir(const char *dict_dir) {
    DictSet *ds = calloc(1, sizeof(*ds));
    if (!ds || !dict_dir || !dict_dir[0]) return ds;

    try_add_dict(ds, dict_dir, "en.kdict",    "en",    "EN");
    try_add_dict(ds, dict_dir, "en-de.kdict", "en-de", "EN>DE");
    try_add_dict(ds, dict_dir, "de-en.kdict", "de-en", "DE>EN");
    try_add_dict(ds, dict_dir, "de.kdict",    "de",    "DE");
    return ds;
}

void dictset_close(DictSet *ds) {
    if (!ds) return;
    for (int i = 0; i < ds->count; i++) {
        dict_close(ds->dicts[i]);
    }
    free(ds);
}

int dictset_count(const DictSet *ds) { return ds ? ds->count : 0; }

int dictset_find_by_id(const DictSet *ds, const char *id) {
    if (!ds || !id) return 0;
    for (int i = 0; i < ds->count; i++) {
        if (strcasecmp(ds->ids[i], id) == 0) return i;
    }
    return 0;
}

const char *dictset_id(const DictSet *ds, int idx) {
    if (!ds || idx < 0 || idx >= ds->count) return "en";
    return ds->ids[idx];
}

const char *dictset_label(const DictSet *ds, int idx) {
    if (!ds || idx < 0 || idx >= ds->count) return "DICT";
    return ds->labels[idx];
}

bool dictset_lookup(const DictSet *ds, int preferred_idx, bool allow_fallback,
                    const char *raw_word, int *used_idx,
                    char *matched_word, size_t matched_sz,
                    char *def_out, size_t def_sz) {
    if (!ds || ds->count <= 0) return false;
    if (preferred_idx < 0 || preferred_idx >= ds->count) preferred_idx = 0;

    if (dict_lookup_lang(ds->dicts[preferred_idx], ds->ids[preferred_idx], raw_word,
                         matched_word, matched_sz, def_out, def_sz)) {
        if (used_idx) *used_idx = preferred_idx;
        return true;
    }

    if (!allow_fallback) {
        if (used_idx) *used_idx = preferred_idx;
        return false;
    }

    for (int i = 0; i < ds->count; i++) {
        if (i == preferred_idx) continue;
        if (dict_lookup_lang(ds->dicts[i], ds->ids[i], raw_word,
                             matched_word, matched_sz, def_out, def_sz)) {
            if (used_idx) *used_idx = i;
            return true;
        }
    }

    if (used_idx) *used_idx = preferred_idx;
    return false;
}
