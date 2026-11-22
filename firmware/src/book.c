#include "kobodash.h"
#include "kd_zlib.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* =========================================================================
 * UTF-8 / CP1252 / HTML-to-styled-text helpers
 * Inline style control codes emitted for reader.c:
 *   \x01 = bold on, \x02 = bold off
 *   \x03 = italic on, \x04 = italic off
 *   \x05 = heading paragraph marker (at start of line)
 * ========================================================================= */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

static void sb_init(StrBuf *sb, size_t init_cap) {
    sb->cap = init_cap > 64 ? init_cap : 64;
    sb->data = malloc(sb->cap);
    sb->len = 0;
    if (sb->data) sb->data[0] = '\0';
}

static void sb_grow(StrBuf *sb, size_t need) {
    if (!sb->data) return;
    if (sb->len + need + 1 <= sb->cap) return;
    size_t ncap = sb->cap * 2;
    while (ncap < sb->len + need + 1) ncap *= 2;
    if (ncap > 4 * 1024 * 1024) return;
    char *nd = realloc(sb->data, ncap);
    if (!nd) return;
    sb->data = nd;
    sb->cap = ncap;
}

static void sb_putc(StrBuf *sb, char ch) {
    sb_grow(sb, 1);
    if (sb->data && sb->len + 1 < sb->cap) {
        sb->data[sb->len++] = ch;
        sb->data[sb->len] = '\0';
    }
}

static void sb_puts(StrBuf *sb, const char *s, size_t n) {
    if (!s || n == 0) return;
    sb_grow(sb, n);
    if (sb->data && sb->len + n < sb->cap) {
        memcpy(sb->data + sb->len, s, n);
        sb->len += n;
        sb->data[sb->len] = '\0';
    }
}

static void sb_put_utf8(StrBuf *sb, uint32_t cp) {
    if (cp == 0) return;
    if (cp < 0x80) {
        sb_putc(sb, (char)cp);
    } else if (cp < 0x800) {
        sb_putc(sb, (char)(0xC0 | (cp >> 6)));
        sb_putc(sb, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        sb_putc(sb, (char)(0xE0 | (cp >> 12)));
        sb_putc(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_putc(sb, (char)(0x80 | (cp & 0x3F)));
    } else {
        sb_putc(sb, (char)(0xF0 | (cp >> 18)));
        sb_putc(sb, (char)(0x80 | ((cp >> 12) & 0x3F)));
        sb_putc(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_putc(sb, (char)(0x80 | (cp & 0x3F)));
    }
}

static uint32_t cp1252_to_unicode(uint8_t b) {
    static const uint16_t tbl[32] = {
        0x20AC, 0x0020, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x0020, 0x017D, 0x0020,
        0x0020, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x0020, 0x017E, 0x0178
    };
    if (b >= 0x80 && b <= 0x9F) return tbl[b - 0x80];
    if (b == 0xA0) return 0x0020; /* NBSP -> normal space */
    if (b == 0xAD) return 0;      /* soft hyphen -> strip */
    return b;
}

static size_t utf8_seq_len(const uint8_t *buf, size_t len, size_t i) {
    uint8_t c = buf[i];
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0 && c >= 0xC2 && i + 1 < len && (buf[i + 1] & 0xC0) == 0x80) return 2;
    if ((c & 0xF0) == 0xE0 && i + 2 < len && (buf[i + 1] & 0xC0) == 0x80 && (buf[i + 2] & 0xC0) == 0x80) return 3;
    if ((c & 0xF8) == 0xF0 && c <= 0xF4 && i + 3 < len && (buf[i + 1] & 0xC0) == 0x80 && (buf[i + 2] & 0xC0) == 0x80 && (buf[i + 3] & 0xC0) == 0x80) return 4;
    return 0;
}

static void analyze_utf8(const uint8_t *buf, size_t len, size_t *valid_mb, size_t *invalid) {
    size_t mb = 0, inv = 0, i = 0;
    while (i < len) {
        size_t sl = utf8_seq_len(buf, len, i);
        if (sl == 1) {
            i++;
        } else if (sl > 1) {
            mb++;
            i += sl;
        } else {
            inv++;
            i++;
        }
    }
    *valid_mb = mb;
    *invalid = inv;
}

/* Repair classic UTF-8-decoded-as-CP1252-then-reencoded-as-UTF-8 mojibake (e.g. â€™ -> ’) */
static void repair_utf8_mojibake(StrBuf *sb) {
    if (!sb || !sb->data || sb->len < 4) return;
    /* Quick scan: only touch buffer if C3 A2 E2 82 AC ("â€") or C3 82 C2 A0 ("Â ") appears */
    if (!memchr(sb->data, 0xC3, sb->len)) return;

    static const struct { const char *bad; size_t blen; uint32_t cp; } reps[] = {
        { "\xC3\xA2\xE2\x82\xAC\xE2\x84\xA2", 8, 0x2019 }, /* â€™ -> ’ */
        { "\xC3\xA2\xE2\x82\xAC\xCB\x9C",     7, 0x2018 }, /* â€˜ -> ‘ */
        { "\xC3\xA2\xE2\x82\xAC\xC5\x93",     7, 0x201C }, /* â€œ -> “ */
        { "\xC3\xA2\xE2\x82\xAC\xC2\x9D",     7, 0x201D }, /* â€\x9d -> ” */
        { "\xC3\xA2\xE2\x82\xAC\xE2\x80\x9D", 8, 0x2014 }, /* â€” -> — */
        { "\xC3\xA2\xE2\x82\xAC\xE2\x80\x9C", 8, 0x2013 }, /* â€“ -> – */
        { "\xC3\xA2\xE2\x82\xAC\xC2\xA6",     7, 0x2026 }, /* â€¦ -> … */
        { "\xC3\xA2\xE2\x82\xAC\xC2\xA2",     7, 0x2022 }, /* â€¢ -> • */
        { "\xC3\x82\xC2\xA0",                 4, 0x0020 }, /* Â<NBSP> -> space */
    };

    bool found = false;
    for (size_t i = 0; i + 4 <= sb->len; i++) {
        if ((uint8_t)sb->data[i] == 0xC3 &&
            ((uint8_t)sb->data[i + 1] == 0xA2 || (uint8_t)sb->data[i + 1] == 0x82)) {
            found = true;
            break;
        }
    }
    if (!found) return;

    StrBuf out;
    sb_init(&out, sb->len + 16);
    if (!out.data) return;

    size_t i = 0;
    while (i < sb->len) {
        bool matched = false;
        if ((uint8_t)sb->data[i] == 0xC3) {
            for (size_t r = 0; r < sizeof(reps) / sizeof(reps[0]); r++) {
                if (i + reps[r].blen <= sb->len &&
                    memcmp(sb->data + i, reps[r].bad, reps[r].blen) == 0) {
                    sb_put_utf8(&out, reps[r].cp);
                    i += reps[r].blen;
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) {
            sb_putc(&out, sb->data[i++]);
        }
    }
    free(sb->data);
    *sb = out;
}

static char *ensure_utf8(const uint8_t *raw, size_t len, bool force_cp1252, size_t *out_len) {
    /* Handle UTF-16LE / UTF-16BE BOMs */
    if (len >= 2 && ((raw[0] == 0xFF && raw[1] == 0xFE) || (raw[0] == 0xFE && raw[1] == 0xFF))) {
        bool le = (raw[0] == 0xFF);
        StrBuf sb;
        sb_init(&sb, len + 64);
        for (size_t i = 2; i + 1 < len; i += 2) {
            uint32_t u = le ? ((uint32_t)raw[i] | ((uint32_t)raw[i + 1] << 8))
                            : (((uint32_t)raw[i] << 8) | (uint32_t)raw[i + 1]);
            if (u >= 0xD800 && u <= 0xDBFF && i + 3 < len) {
                uint32_t u2 = le ? ((uint32_t)raw[i + 2] | ((uint32_t)raw[i + 3] << 8))
                                 : (((uint32_t)raw[i + 2] << 8) | (uint32_t)raw[i + 3]);
                if (u2 >= 0xDC00 && u2 <= 0xDFFF) {
                    u = 0x10000u + (((u - 0xD800u) << 10) | (u2 - 0xDC00u));
                    i += 2;
                }
            }
            if (u == 0x00A0) u = ' ';
            if (u != 0 && u != 0xFEFF && u != 0x00AD) sb_put_utf8(&sb, u);
        }
        *out_len = sb.len;
        return sb.data;
    }

    /* Strip UTF-8 BOM if present */
    if (len >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
        raw += 3;
        len -= 3;
        force_cp1252 = false;
    }

    size_t valid_mb = 0, invalid = 0;
    analyze_utf8(raw, len, &valid_mb, &invalid);

    StrBuf sb;
    sb_init(&sb, len + len / 8 + 64);
    if (!sb.data) return NULL;

    /* Pure valid UTF-8 (even if a broken MOBI header claimed CP1252!) */
    if (invalid == 0 && (!force_cp1252 || valid_mb > 0)) {
        sb_puts(&sb, (const char *)raw, len);
    } else if (valid_mb > 0 && valid_mb >= invalid * 3 && !force_cp1252) {
        /* Predominantly UTF-8 with a few stray non-UTF-8 bytes: preserve valid UTF-8 and rescue stray bytes */
        size_t i = 0;
        while (i < len) {
            size_t sl = utf8_seq_len(raw, len, i);
            if (sl > 0) {
                sb_puts(&sb, (const char *)(raw + i), sl);
                i += sl;
            } else {
                uint32_t cp = cp1252_to_unicode(raw[i++]);
                if (cp > 0) sb_put_utf8(&sb, cp);
            }
        }
    } else {
        /* CP1252 / Latin-1 -> UTF-8 */
        for (size_t i = 0; i < len; i++) {
            if (raw[i] < 0x80) sb_putc(&sb, (char)raw[i]);
            else {
                uint32_t cp = cp1252_to_unicode(raw[i]);
                if (cp > 0) sb_put_utf8(&sb, cp);
            }
        }
    }

    repair_utf8_mojibake(&sb);
    *out_len = sb.len;
    return sb.data;
}

#define ENT_DROP 0xFFFFFFFFu

static uint32_t normalize_decoded_cp(uint32_t cp) {
    if (cp == 0) return ENT_DROP;
    if (cp >= 0x80 && cp <= 0x9F) return cp1252_to_unicode((uint8_t)cp);
    if (cp == '\t' || cp == '\n' || cp == '\r' || cp == 0x00A0 ||
        (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F || cp == 0x205F || cp == 0x3000) {
        return ' ';
    }
    if (cp == 0x00AD || cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0xFEFF) {
        return ENT_DROP;
    }
    if (cp < 32) return ENT_DROP;
    return cp;
}

static uint32_t decode_html_entity(const char *ent, size_t len) {
    if (len == 0 || len > 12) return 0;
    char buf[16];
    memcpy(buf, ent, len);
    buf[len] = '\0';

    if (buf[0] == '#') {
        char *endp = NULL;
        unsigned long v = 0;
        if (len > 1 && (buf[1] == 'x' || buf[1] == 'X')) {
            if (len < 3) return 0;
            v = strtoul(buf + 2, &endp, 16);
        } else {
            if (len < 2) return 0;
            v = strtoul(buf + 1, &endp, 10);
        }
        if (!endp || *endp != '\0' || v > 0x10FFFFul) return 0;
        return normalize_decoded_cp((uint32_t)v);
    }

    /* Case-sensitive Latin/German/French entities first (e.g. Auml vs auml, Dagger vs dagger) */
    static const struct { const char *name; uint32_t cp; } cs_tbl[] = {
        { "Auml",   0x00C4 }, { "Ouml",   0x00D6 }, { "Uuml",   0x00DC },
        { "auml",   0x00E4 }, { "ouml",   0x00F6 }, { "uuml",   0x00FC },
        { "szlig",  0x00DF },
        { "Aacute", 0x00C1 }, { "Agrave", 0x00C0 }, { "Acirc",  0x00C2 }, { "Atilde", 0x00C3 }, { "Aring", 0x00C5 }, { "AElig", 0x00C6 },
        { "aacute", 0x00E1 }, { "agrave", 0x00E0 }, { "acirc",  0x00E2 }, { "atilde", 0x00E3 }, { "aring", 0x00E5 }, { "aelig", 0x00E6 },
        { "Ccedil", 0x00C7 }, { "ccedil", 0x00E7 },
        { "Eacute", 0x00C9 }, { "Egrave", 0x00C8 }, { "Ecirc",  0x00CA }, { "Euml",   0x00CB },
        { "eacute", 0x00E9 }, { "egrave", 0x00E8 }, { "ecirc",  0x00EA }, { "euml",   0x00EB },
        { "Iacute", 0x00CD }, { "Igrave", 0x00CC }, { "Icirc",  0x00CE }, { "Iuml",   0x00CF },
        { "iacute", 0x00ED }, { "igrave", 0x00EC }, { "icirc",  0x00EE }, { "iuml",   0x00EF },
        { "Ntilde", 0x00D1 }, { "ntilde", 0x00F1 },
        { "Oacute", 0x00D3 }, { "Ograve", 0x00D2 }, { "Ocirc",  0x00D4 }, { "Otilde", 0x00D5 }, { "Oslash", 0x00D8 }, { "OElig", 0x0152 },
        { "oacute", 0x00F3 }, { "ograve", 0x00F2 }, { "ocirc",  0x00F4 }, { "otilde", 0x00F5 }, { "oslash", 0x00F8 }, { "oelig", 0x0153 },
        { "Uacute", 0x00DA }, { "Ugrave", 0x00D9 }, { "Ucirc",  0x00DB },
        { "uacute", 0x00FA }, { "ugrave", 0x00F9 }, { "ucirc",  0x00FB },
        { "Yacute", 0x00DD }, { "yacute", 0x00FD }, { "yuml",   0x00FF }, { "Yuml",   0x0178 },
        { "Scaron", 0x0160 }, { "scaron", 0x0161 },
        { "Dagger", 0x2021 }, { "dagger", 0x2020 },
    };
    for (size_t k = 0; k < sizeof(cs_tbl) / sizeof(cs_tbl[0]); k++) {
        if (strcmp(buf, cs_tbl[k].name) == 0) return cs_tbl[k].cp;
    }

    /* Case-insensitive common HTML/XML entities */
    static const struct { const char *name; uint32_t cp; } ci_tbl[] = {
        { "amp",    '&' },      { "lt",     '<' },      { "gt",     '>' },
        { "quot",   '"' },      { "apos",   '\'' },     { "nbsp",   ' ' },
        { "ensp",   ' ' },      { "emsp",   ' ' },      { "thinsp", ' ' },
        { "shy",    ENT_DROP }, { "zwnj",   ENT_DROP }, { "zwj",    ENT_DROP },
        { "mdash",  0x2014 },   { "ndash",  0x2013 },   { "hellip", 0x2026 },
        { "lsquo",  0x2018 },   { "rsquo",  0x2019 },   { "sbquo",  0x201A },
        { "ldquo",  0x201C },   { "rdquo",  0x201D },   { "bdquo",  0x201E },
        { "laquo",  0x00AB },   { "raquo",  0x00BB },   { "lsaquo", 0x2039 }, { "rsaquo", 0x203A },
        { "copy",   0x00A9 },   { "reg",    0x00AE },   { "trade",  0x2122 },
        { "bull",   0x2022 },   { "middot", 0x00B7 },   { "sect",   0x00A7 }, { "para",   0x00B6 },
        { "deg",    0x00B0 },   { "plusmn", 0x00B1 },   { "times",  0x00D7 }, { "divide", 0x00F7 },
        { "euro",   0x20AC },   { "pound",  0x00A3 },   { "yen",    0x00A5 }, { "cent",   0x00A2 },
        { "frac12", 0x00BD },   { "frac14", 0x00BC },   { "frac34", 0x00BE },
        { "iexcl",  0x00A1 },   { "iquest", 0x00BF },   { "minus",  0x2212 },
    };
    for (size_t k = 0; k < sizeof(ci_tbl) / sizeof(ci_tbl[0]); k++) {
        if (strcasecmp(buf, ci_tbl[k].name) == 0) return ci_tbl[k].cp;
    }
    return 0;
}

static void decode_entities_inplace(char *s) {
    if (!s) return;
    StrBuf sb;
    sb_init(&sb, strlen(s) + 16);
    if (!sb.data) return;
    size_t len = strlen(s);
    size_t i = 0;
    while (i < len) {
        if (s[i] == '&') {
            size_t j = i + 1;
            while (j < len && j < i + 13 && s[j] != ';' && s[j] != '&' && s[j] != '<' && !isspace((unsigned char)s[j])) j++;
            if (j < len && s[j] == ';') {
                uint32_t cp = decode_html_entity(s + i + 1, j - (i + 1));
                if (cp == ENT_DROP) { i = j + 1; continue; }
                if (cp > 0) { sb_put_utf8(&sb, cp); i = j + 1; continue; }
            }
        }
        sb_putc(&sb, s[i++]);
    }
    size_t cpy = sb.len < len ? sb.len : len;
    memcpy(s, sb.data, cpy);
    s[cpy] = '\0';
    free(sb.data);
}

static void sb_ensure_para_break(StrBuf *sb) {
    while (sb->len > 0 && sb->data[sb->len - 1] == ' ') sb->len--;
    if (sb->len == 0) return;
    if (sb->len >= 2 && sb->data[sb->len - 1] == '\n' && sb->data[sb->len - 2] == '\n') return;
    if (sb->data[sb->len - 1] == '\n') sb_putc(sb, '\n');
    else sb_puts(sb, "\n\n", 2);
}

static char *html_to_styled_text(const char *html, size_t len, size_t *out_len) {
    StrBuf sb;
    sb_init(&sb, len / 2 + 256);

    bool in_skip = false;
    char skip_tag[16] = {0};
    bool last_space = true;
    size_t i = 0;

    while (i < len) {
        if (html[i] == '<') {
            /* Comment <!-- ... --> */
            if (i + 4 <= len && memcmp(html + i, "<!--", 4) == 0) {
                const char *end = strstr(html + i + 4, "-->");
                if (end) { i = (size_t)(end - html) + 3; continue; }
                break;
            }
            size_t j = i + 1;
            while (j < len && html[j] != '>') j++;
            if (j >= len) break;

            const char *tp = html + i + 1;
            size_t tlen = j - (i + 1);
            while (tlen > 0 && isspace((unsigned char)*tp)) { tp++; tlen--; }
            bool closing = false;
            if (tlen > 0 && *tp == '/') { closing = true; tp++; tlen--; }

            char tag[24];
            size_t tn = 0;
            while (tn < tlen && tn + 1 < sizeof(tag) &&
                   (isalnum((unsigned char)tp[tn]) || tp[tn] == ':')) {
                tag[tn] = (char)tolower((unsigned char)tp[tn]);
                tn++;
            }
            tag[tn] = '\0';

            if (in_skip) {
                if (closing && strcmp(tag, skip_tag) == 0) {
                    in_skip = false;
                }
            } else if (!closing && (strcmp(tag, "script") == 0 ||
                                    strcmp(tag, "style") == 0 ||
                                    strcmp(tag, "head") == 0 ||
                                    strcmp(tag, "title") == 0)) {
                in_skip = true;
                snprintf(skip_tag, sizeof(skip_tag), "%s", tag);
            } else if (strcmp(tag, "p") == 0 || strcmp(tag, "div") == 0 ||
                       strcmp(tag, "blockquote") == 0 || strcmp(tag, "section") == 0 ||
                       strcmp(tag, "li") == 0 || strcmp(tag, "tr") == 0 ||
                       strcmp(tag, "hr") == 0) {
                sb_ensure_para_break(&sb);
                if (!closing && strcmp(tag, "li") == 0) {
                    sb_put_utf8(&sb, 0x2022);
                    sb_putc(&sb, ' ');
                }
                last_space = true;
            } else if (strcmp(tag, "br") == 0) {
                if (sb.len > 0 && sb.data[sb.len - 1] != '\n') sb_putc(&sb, '\n');
                last_space = true;
            } else if (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' && tag[2] == '\0') {
                sb_ensure_para_break(&sb);
                if (!closing) sb_putc(&sb, '\x05');
                last_space = true;
            } else if (strcmp(tag, "b") == 0 || strcmp(tag, "strong") == 0) {
                sb_putc(&sb, closing ? '\x02' : '\x01');
            } else if (strcmp(tag, "i") == 0 || strcmp(tag, "em") == 0) {
                sb_putc(&sb, closing ? '\x04' : '\x03');
            }
            i = j + 1;
            continue;
        }

        if (in_skip) { i++; continue; }

        if (html[i] == '&') {
            size_t j = i + 1;
            while (j < len && j < i + 13 && html[j] != ';' && html[j] != '&' &&
                   html[j] != '<' && !isspace((unsigned char)html[j])) {
                j++;
            }
            if (j < len && html[j] == ';') {
                uint32_t cp = decode_html_entity(html + i + 1, j - (i + 1));
                if (cp == ENT_DROP) {
                    i = j + 1;
                    continue;
                }
                if (cp == ' ') {
                    if (!last_space) { sb_putc(&sb, ' '); last_space = true; }
                    i = j + 1;
                    continue;
                }
                if (cp > 0) {
                    sb_put_utf8(&sb, cp);
                    last_space = false;
                    i = j + 1;
                    continue;
                }
            }
            /* Unknown/literal '&': preserve it instead of deleting */
            sb_putc(&sb, '&');
            last_space = false;
            i++;
            continue;
        }

        /* Normalize raw UTF-8 NBSP (C2 A0), soft hyphen (C2 AD), zero-width chars, BOM */
        unsigned char ch = (unsigned char)html[i];
        if (ch == 0xC2 && i + 1 < len) {
            unsigned char b2 = (unsigned char)html[i + 1];
            if (b2 == 0xA0) {
                if (!last_space) { sb_putc(&sb, ' '); last_space = true; }
                i += 2;
                continue;
            }
            if (b2 == 0xAD) { i += 2; continue; }
        }
        if (ch == 0xEF && i + 2 < len && (unsigned char)html[i + 1] == 0xBB && (unsigned char)html[i + 2] == 0xBF) {
            i += 3;
            continue;
        }
        if (ch == 0xE2 && i + 2 < len && (unsigned char)html[i + 1] == 0x80) {
            unsigned char b3 = (unsigned char)html[i + 2];
            if (b3 >= 0x80 && b3 <= 0x8A) {
                if (!last_space) { sb_putc(&sb, ' '); last_space = true; }
                i += 3;
                continue;
            }
            if (b3 >= 0x8B && b3 <= 0x8D) { i += 3; continue; }
            if (b3 == 0xAF) {
                if (!last_space) { sb_putc(&sb, ' '); last_space = true; }
                i += 3;
                continue;
            }
        }

        i++;
        if (ch == '\r' || ch == '\n' || ch == '\t' || ch == ' ') {
            if (!last_space) {
                sb_putc(&sb, ' ');
                last_space = true;
            }
        } else if (ch >= 32) {
            sb_putc(&sb, (char)ch);
            last_space = false;
        }
    }

    /* Trim leading/trailing newlines */
    while (sb.len > 0 && (sb.data[sb.len - 1] == '\n' || sb.data[sb.len - 1] == ' ')) {
        sb.data[--sb.len] = '\0';
    }
    *out_len = sb.len;
    return sb.data;
}

static char *normalize_plain_txt(const char *txt, size_t len, size_t *out_len) {
    StrBuf sb;
    sb_init(&sb, len + 64);

    size_t i = 0;
    while (i < len) {
        if (txt[i] == '\r') { i++; continue; }
        if (txt[i] == '\n') {
            /* Count consecutive newlines; >=2 means paragraph break */
            int nl = 0;
            while (i < len && (txt[i] == '\n' || txt[i] == '\r')) {
                if (txt[i] == '\n') nl++;
                i++;
            }
            if (nl >= 2) {
                sb_ensure_para_break(&sb);
            } else {
                /* Single newline: unwrap hard-wrapped lines unless next line starts indented */
                if (i < len && (txt[i] == ' ' || txt[i] == '\t')) {
                    sb_ensure_para_break(&sb);
                    while (i < len && (txt[i] == ' ' || txt[i] == '\t')) i++;
                } else if (sb.len > 0 && sb.data[sb.len - 1] != ' ' && sb.data[sb.len - 1] != '\n') {
                    sb_putc(&sb, ' ');
                }
            }
            continue;
        }
        if (txt[i] == '\t') {
            sb_putc(&sb, ' ');
            i++;
            continue;
        }
        /* Normalize UTF-8 NBSP (C2 A0) -> space, strip soft hyphen (C2 AD) & BOM */
        unsigned char uc = (unsigned char)txt[i];
        if (uc == 0xC2 && i + 1 < len) {
            unsigned char b2 = (unsigned char)txt[i + 1];
            if (b2 == 0xA0) { sb_putc(&sb, ' '); i += 2; continue; }
            if (b2 == 0xAD) { i += 2; continue; }
        }
        if (uc == 0xEF && i + 2 < len && (unsigned char)txt[i + 1] == 0xBB && (unsigned char)txt[i + 2] == 0xBF) {
            i += 3;
            continue;
        }
        if (uc >= 32) {
            sb_putc(&sb, txt[i]);
        }
        i++;
    }
    *out_len = sb.len;
    return sb.data;
}

/* =========================================================================
 * Minimal ZIP reader for EPUB (uses libz.so.1 raw DEFLATE)
 * ========================================================================= */

typedef struct {
    char name[256];
    uint16_t method;
    uint32_t comp_size;
    uint32_t uncomp_size;
    uint32_t local_hdr_off;
} ZipEntry;

typedef struct {
    FILE *fp;
    ZipEntry *entries;
    int count;
} ZipArc;

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static bool zip_open_arc(ZipArc *za, const char *path) {
    memset(za, 0, sizeof(*za));
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return false; }
    long fsz = ftell(fp);
    if (fsz < 22) { fclose(fp); return false; }

    size_t tail_sz = (size_t)(fsz < 65558 ? fsz : 65558);
    uint8_t *tail = malloc(tail_sz);
    if (!tail) { fclose(fp); return false; }
    fseek(fp, fsz - (long)tail_sz, SEEK_SET);
    if (fread(tail, 1, tail_sz, fp) != tail_sz) { free(tail); fclose(fp); return false; }

    long eocd = -1;
    for (long i = (long)tail_sz - 22; i >= 0; i--) {
        if (tail[i] == 0x50 && tail[i + 1] == 0x4B && tail[i + 2] == 0x05 && tail[i + 3] == 0x06) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) { free(tail); fclose(fp); return false; }

    uint16_t total_ent = le16(tail + eocd + 10);
    uint32_t cd_size = le32(tail + eocd + 12);
    uint32_t cd_off = le32(tail + eocd + 16);
    free(tail);

    if (total_ent == 0 || cd_size > 16 * 1024 * 1024 || (long)(cd_off + cd_size) > fsz) {
        fclose(fp);
        return false;
    }

    uint8_t *cd = malloc(cd_size);
    if (!cd) { fclose(fp); return false; }
    fseek(fp, (long)cd_off, SEEK_SET);
    if (fread(cd, 1, cd_size, fp) != cd_size) { free(cd); fclose(fp); return false; }

    int cap = total_ent > 4096 ? 4096 : total_ent;
    za->entries = calloc((size_t)cap, sizeof(ZipEntry));
    if (!za->entries) { free(cd); fclose(fp); return false; }

    size_t pos = 0;
    int idx = 0;
    while (pos + 46 <= cd_size && idx < cap) {
        if (le32(cd + pos) != 0x02014B50u) break;
        uint16_t method = le16(cd + pos + 10);
        uint32_t csz = le32(cd + pos + 20);
        uint32_t usz = le32(cd + pos + 24);
        uint16_t nlen = le16(cd + pos + 28);
        uint16_t elen = le16(cd + pos + 30);
        uint16_t clen = le16(cd + pos + 32);
        uint32_t lhoff = le32(cd + pos + 42);
        if (pos + 46 + nlen > cd_size) break;

        ZipEntry *ze = &za->entries[idx++];
        size_t cpy = nlen < sizeof(ze->name) - 1 ? nlen : sizeof(ze->name) - 1;
        memcpy(ze->name, cd + pos + 46, cpy);
        ze->name[cpy] = '\0';
        ze->method = method;
        ze->comp_size = csz;
        ze->uncomp_size = usz;
        ze->local_hdr_off = lhoff;

        pos += 46u + nlen + elen + clen;
    }
    free(cd);
    za->fp = fp;
    za->count = idx;
    return idx > 0;
}

static void zip_close_arc(ZipArc *za) {
    if (!za) return;
    free(za->entries);
    if (za->fp) fclose(za->fp);
    memset(za, 0, sizeof(*za));
}

static const ZipEntry *zip_find(const ZipArc *za, const char *name) {
    for (int i = 0; i < za->count; i++) {
        if (strcmp(za->entries[i].name, name) == 0) return &za->entries[i];
    }
    /* Case-insensitive fallback */
    for (int i = 0; i < za->count; i++) {
        if (strcasecmp(za->entries[i].name, name) == 0) return &za->entries[i];
    }
    return NULL;
}

static uint8_t *zip_extract(const ZipArc *za, const ZipEntry *ze, size_t *out_len) {
    if (!za || !za->fp || !ze) return NULL;
    if (ze->uncomp_size > 4 * 1024 * 1024 || ze->comp_size > 4 * 1024 * 1024) return NULL;

    uint8_t lhdr[30];
    if (fseek(za->fp, (long)ze->local_hdr_off, SEEK_SET) != 0) return NULL;
    if (fread(lhdr, 1, 30, za->fp) != 30) return NULL;
    if (le32(lhdr) != 0x04034B50u) return NULL;
    uint16_t nlen = le16(lhdr + 26);
    uint16_t elen = le16(lhdr + 28);
    if (fseek(za->fp, (long)(ze->local_hdr_off + 30u + nlen + elen), SEEK_SET) != 0) return NULL;

    uint8_t *comp = malloc(ze->comp_size + 1);
    if (!comp) return NULL;
    if (ze->comp_size > 0 && fread(comp, 1, ze->comp_size, za->fp) != ze->comp_size) {
        free(comp);
        return NULL;
    }

    if (ze->method == 0) {
        comp[ze->comp_size] = '\0';
        *out_len = ze->comp_size;
        return comp;
    }

    if (ze->method != 8) {
        free(comp);
        return NULL;
    }

    size_t out_cap = ze->uncomp_size > 0 ? ze->uncomp_size + 16 : 65536;
    uint8_t *out = malloc(out_cap + 1);
    if (!out) { free(comp); return NULL; }

    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    zs.next_in = comp;
    zs.avail_in = (uInt)ze->comp_size;
    zs.next_out = out;
    zs.avail_out = (uInt)out_cap;

    if (inflateInit2_(&zs, -MAX_WBITS, zlibVersion(), (int)sizeof(zs)) != Z_OK) {
        free(out);
        free(comp);
        return NULL;
    }
    int r = inflate(&zs, Z_FINISH);
    size_t produced = (size_t)zs.total_out;
    inflateEnd(&zs);
    free(comp);

    if (r != Z_STREAM_END && r != Z_OK) {
        free(out);
        return NULL;
    }
    out[produced] = '\0';
    *out_len = produced;
    return out;
}

/* =========================================================================
 * PalmDOC LZ77 decompressor for MOBI
 * ========================================================================= */

static size_t palmdoc_decompress(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap) {
    size_t si = 0, di = 0;
    while (si < src_len && di < dst_cap) {
        uint8_t c = src[si++];
        if (c == 0) {
            dst[di++] = 0;
        } else if (c <= 8) {
            for (int k = 0; k < c && si < src_len && di < dst_cap; k++) {
                dst[di++] = src[si++];
            }
        } else if (c <= 0x7F) {
            dst[di++] = c;
        } else if (c <= 0xBF) {
            if (si >= src_len) break;
            uint16_t pair = (uint16_t)(((c & 0x3Fu) << 8) | src[si++]);
            size_t dist = (pair >> 3) & 0x07FFu;
            size_t len = (pair & 0x07u) + 3u;
            if (dist == 0 || dist > di) continue;
            for (size_t k = 0; k < len && di < dst_cap; k++) {
                dst[di] = dst[di - dist];
                di++;
            }
        } else {
            if (di + 2 <= dst_cap) {
                dst[di++] = ' ';
                dst[di++] = (uint8_t)(c ^ 0x80u);
            }
        }
    }
    return di;
}

static size_t mobi_trim_extra(const uint8_t *rec, size_t len, uint32_t extra_flags) {
    /* Strip trailing data entries encoded by MOBI extra_data_flags */
    uint32_t flags = extra_flags >> 1;
    while (flags && len > 0) {
        if (flags & 1u) {
            /* Backward-encoded variable-width integer at end of record */
            size_t sz = 0;
            size_t shift = 0;
            size_t pos = len;
            for (int k = 0; k < 4 && pos > 0; k++) {
                pos--;
                uint8_t b = rec[pos];
                sz |= (size_t)(b & 0x7Fu) << shift;
                shift += 7;
                if (b & 0x80u) break;
            }
            if (sz > 0 && sz <= len) len -= sz;
            else break;
        }
        flags >>= 1;
    }
    if ((extra_flags & 1u) && len > 0) {
        size_t trail = (size_t)(rec[len - 1] & 0x03u) + 1u;
        if (trail <= len) len -= trail;
    }
    return len;
}

/* =========================================================================
 * Unified Book implementation
 * ========================================================================= */

#define KD_MAX_CHAPTERS 256
#define TXT_CHUNK_SIZE  32768u
#define MOBI_RECS_PER_CH 12

struct Book {
    BookFormat fmt;
    char path[384];
    char title[128];
    int chapter_count;
    size_t total_size;

    /* TXT virtual chunks */
    FILE *txt_fp;
    size_t txt_off[KD_MAX_CHAPTERS + 1];

    /* EPUB spine */
    ZipArc epub_zip;
    char epub_chapters[KD_MAX_CHAPTERS][256];
    size_t epub_sizes[KD_MAX_CHAPTERS];
    size_t epub_cum_off[KD_MAX_CHAPTERS + 1];

    /* MOBI PalmDB */
    FILE *mobi_fp;
    uint32_t *mobi_rec_off;
    int mobi_num_recs;
    int mobi_text_recs;
    uint16_t mobi_comp;
    uint32_t mobi_enc;
    uint32_t mobi_extra_flags;
    size_t mobi_cum_off[KD_MAX_CHAPTERS + 1];
};

BookFormat book_detect_format(const char *path) {
    if (!path) return BOOK_FMT_NONE;
    const char *dot = strrchr(path, '.');
    if (!dot) return BOOK_FMT_NONE;
    if (strcasecmp(dot, ".epub") == 0) return BOOK_FMT_EPUB;
    if (strcasecmp(dot, ".mobi") == 0) return BOOK_FMT_MOBI;
    if (strcasecmp(dot, ".txt") == 0 || strcasecmp(dot, ".md") == 0) return BOOK_FMT_TXT;
    return BOOK_FMT_NONE;
}

const char *book_format_tag(BookFormat fmt) {
    switch (fmt) {
        case BOOK_FMT_EPUB: return "EPUB";
        case BOOK_FMT_MOBI: return "MOBI";
        case BOOK_FMT_TXT:  return "TXT";
        default:            return "FILE";
    }
}

static void title_from_filename(const char *path, char *out, size_t out_sz) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *dot = strrchr(base, '.');
    size_t len = dot ? (size_t)(dot - base) : strlen(base);
    if (len >= out_sz) len = out_sz - 1;
    for (size_t i = 0; i < len; i++) {
        out[i] = (base[i] == '_') ? ' ' : base[i];
    }
    out[len] = '\0';
}

/* Simple XML attribute extractor */
static bool xml_get_attr(const char *tag, size_t tag_len, const char *attr, char *out, size_t out_sz) {
    size_t alen = strlen(attr);
    for (size_t i = 0; i + alen + 2 < tag_len; i++) {
        if ((i == 0 || isspace((unsigned char)tag[i - 1])) &&
            strncasecmp(tag + i, attr, alen) == 0 && tag[i + alen] == '=') {
            size_t p = i + alen + 1;
            char q = tag[p];
            if (q != '"' && q != '\'') continue;
            p++;
            size_t start = p;
            while (p < tag_len && tag[p] != q) p++;
            size_t vlen = p - start;
            if (vlen >= out_sz) vlen = out_sz - 1;
            memcpy(out, tag + start, vlen);
            out[vlen] = '\0';
            return true;
        }
    }
    return false;
}

static void url_decode_inplace(char *s) {
    char *r = s, *w = s;
    while (*r) {
        if (*r == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
            char hex[3] = { r[1], r[2], '\0' };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 3;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static bool open_txt_book(Book *b) {
    FILE *fp = fopen(b->path, "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz <= 0) { fclose(fp); return false; }
    rewind(fp);

    b->txt_fp = fp;
    b->total_size = (size_t)sz;
    b->txt_off[0] = 0;
    int ch = 0;
    size_t pos = 0;
    while (pos < (size_t)sz && ch < KD_MAX_CHAPTERS) {
        size_t next = pos + TXT_CHUNK_SIZE;
        if (next >= (size_t)sz) {
            next = (size_t)sz;
        } else {
            /* Advance up to 2 KB to land on a paragraph / newline break */
            fseek(fp, (long)next, SEEK_SET);
            char probe[2048];
            size_t pn = fread(probe, 1, sizeof(probe), fp);
            size_t snap = 0;
            for (size_t i = 0; i + 1 < pn; i++) {
                if (probe[i] == '\n' && probe[i + 1] == '\n') { snap = i + 2; break; }
            }
            if (snap == 0) {
                for (size_t i = 0; i < pn; i++) {
                    if (probe[i] == '\n') { snap = i + 1; break; }
                }
            }
            next += snap;
            if (next > (size_t)sz) next = (size_t)sz;
        }
        ch++;
        b->txt_off[ch] = next;
        pos = next;
    }
    b->chapter_count = ch;
    return ch > 0;
}

static bool open_epub_book(Book *b) {
    if (!zip_open_arc(&b->epub_zip, b->path)) return false;

    /* 1. Find OPF path via META-INF/container.xml */
    char opf_path[256] = {0};
    const ZipEntry *ce = zip_find(&b->epub_zip, "META-INF/container.xml");
    if (ce) {
        size_t clen = 0;
        uint8_t *cdata = zip_extract(&b->epub_zip, ce, &clen);
        if (cdata) {
            const char *rf = strstr((const char *)cdata, "full-path=");
            if (rf) {
                const char *q = strchr(rf, '"');
                if (!q) q = strchr(rf, '\'');
                if (q) {
                    char qc = *q++;
                    const char *end = strchr(q, qc);
                    if (end) {
                        size_t n = (size_t)(end - q);
                        if (n >= sizeof(opf_path)) n = sizeof(opf_path) - 1;
                        memcpy(opf_path, q, n);
                        opf_path[n] = '\0';
                    }
                }
            }
            free(cdata);
        }
    }
    /* Fallback: search for any .opf file */
    if (!opf_path[0]) {
        for (int i = 0; i < b->epub_zip.count; i++) {
            const char *nm = b->epub_zip.entries[i].name;
            size_t l = strlen(nm);
            if (l > 4 && strcasecmp(nm + l - 4, ".opf") == 0) {
                snprintf(opf_path, sizeof(opf_path), "%s", nm);
                break;
            }
        }
    }

    char opf_dir[256] = {0};
    char *last_sl = strrchr(opf_path, '/');
    if (last_sl) {
        size_t dlen = (size_t)(last_sl - opf_path + 1);
        if (dlen < sizeof(opf_dir)) {
            memcpy(opf_dir, opf_path, dlen);
            opf_dir[dlen] = '\0';
        }
    }

    const ZipEntry *oe = opf_path[0] ? zip_find(&b->epub_zip, opf_path) : NULL;
    if (oe) {
        size_t olen = 0;
        uint8_t *odata = zip_extract(&b->epub_zip, oe, &olen);
        if (odata) {
            const char *xml = (const char *)odata;
            /* Extract <dc:title> or <title> strictly (never match calibre:title_sort!) */
            const char *ts = NULL;
            const char *scan = xml;
            while ((scan = strchr(scan, '<')) != NULL) {
                if (strncasecmp(scan, "<dc:title", 9) == 0 &&
                    (scan[9] == '>' || isspace((unsigned char)scan[9]))) {
                    ts = scan;
                    break;
                }
                if (!ts && strncasecmp(scan, "<title", 6) == 0 &&
                    (scan[6] == '>' || isspace((unsigned char)scan[6]))) {
                    ts = scan;
                }
                scan++;
            }
            if (ts) {
                const char *gt = strchr(ts, '>');
                if (gt) {
                    gt++;
                    const char *lt = strchr(gt, '<');
                    if (lt && lt > gt) {
                        char raw_title[160];
                        size_t tl = (size_t)(lt - gt);
                        if (tl >= sizeof(raw_title)) tl = sizeof(raw_title) - 1;
                        memcpy(raw_title, gt, tl);
                        raw_title[tl] = '\0';
                        decode_entities_inplace(raw_title);
                        /* Collapse whitespace/newlines */
                        size_t wi = 0;
                        bool prev_sp = true;
                        for (size_t ri = 0; raw_title[ri] && wi + 1 < sizeof(b->title); ri++) {
                            unsigned char ch = (unsigned char)raw_title[ri];
                            if (isspace(ch)) {
                                if (!prev_sp) { b->title[wi++] = ' '; prev_sp = true; }
                            } else {
                                b->title[wi++] = (char)ch;
                                prev_sp = false;
                            }
                        }
                        while (wi > 0 && b->title[wi - 1] == ' ') wi--;
                        if (wi > 0) b->title[wi] = '\0';
                    }
                }
            }

            /* Parse manifest items: id -> href */
            typedef struct { char id[96]; char href[200]; } ManItem;
            ManItem *items = calloc(1024, sizeof(ManItem));
            int item_cnt = 0;
            if (items) {
                const char *p = xml;
                while ((p = strstr(p, "<item")) != NULL && item_cnt < 1024) {
                    if (!isspace((unsigned char)p[5])) { p += 5; continue; }
                    const char *gt = strchr(p, '>');
                    if (!gt) break;
                    size_t tlen = (size_t)(gt - p);
                    char id[96] = {0}, href[200] = {0};
                    if (xml_get_attr(p, tlen, "id", id, sizeof(id)) &&
                        xml_get_attr(p, tlen, "href", href, sizeof(href))) {
                        url_decode_inplace(href);
                        snprintf(items[item_cnt].id, sizeof(items[0].id), "%s", id);
                        snprintf(items[item_cnt].href, sizeof(items[0].href), "%s", href);
                        item_cnt++;
                    }
                    p = gt + 1;
                }

                /* Parse spine itemref idref -> resolve chapter path */
                p = strstr(xml, "<spine");
                if (p) {
                    while ((p = strstr(p, "<itemref")) != NULL && b->chapter_count < KD_MAX_CHAPTERS) {
                        const char *gt = strchr(p, '>');
                        if (!gt) break;
                        char idref[96] = {0};
                        if (xml_get_attr(p, (size_t)(gt - p), "idref", idref, sizeof(idref))) {
                            for (int k = 0; k < item_cnt; k++) {
                                if (strcmp(items[k].id, idref) == 0) {
                                    char full[256];
                                    snprintf(full, sizeof(full), "%s%s", opf_dir, items[k].href);
                                    const ZipEntry *ze = zip_find(&b->epub_zip, full);
                                    if (ze) {
                                        int ci = b->chapter_count++;
                                        snprintf(b->epub_chapters[ci], sizeof(b->epub_chapters[0]), "%s", ze->name);
                                        b->epub_sizes[ci] = ze->uncomp_size ? ze->uncomp_size : 1024;
                                    }
                                    break;
                                }
                            }
                        }
                        p = gt + 1;
                    }
                }
                free(items);
            }
            free(odata);
        }
    }

    /* Fallback if OPF had no spine: add all .xhtml/.html/.htm entries in archive order */
    if (b->chapter_count == 0) {
        for (int i = 0; i < b->epub_zip.count && b->chapter_count < KD_MAX_CHAPTERS; i++) {
            const char *nm = b->epub_zip.entries[i].name;
            size_t l = strlen(nm);
            if ((l > 6 && strcasecmp(nm + l - 6, ".xhtml") == 0) ||
                (l > 5 && strcasecmp(nm + l - 5, ".html") == 0) ||
                (l > 4 && strcasecmp(nm + l - 4, ".htm") == 0)) {
                int ci = b->chapter_count++;
                snprintf(b->epub_chapters[ci], sizeof(b->epub_chapters[0]), "%s", nm);
                b->epub_sizes[ci] = b->epub_zip.entries[i].uncomp_size ? b->epub_zip.entries[i].uncomp_size : 1024;
            }
        }
    }

    size_t cum = 0;
    for (int i = 0; i < b->chapter_count; i++) {
        b->epub_cum_off[i] = cum;
        cum += b->epub_sizes[i];
    }
    b->epub_cum_off[b->chapter_count] = cum;
    b->total_size = cum;
    return b->chapter_count > 0;
}

static bool open_mobi_book(Book *b) {
    FILE *fp = fopen(b->path, "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    long fsz = ftell(fp);
    if (fsz < 128) { fclose(fp); return false; }
    rewind(fp);

    uint8_t pdb[78];
    if (fread(pdb, 1, 78, fp) != 78) { fclose(fp); return false; }
    int num_recs = be16(pdb + 76);
    if (num_recs < 2 || num_recs > 65535) { fclose(fp); return false; }

    uint32_t *offs = malloc((size_t)(num_recs + 1) * sizeof(uint32_t));
    if (!offs) { fclose(fp); return false; }
    for (int i = 0; i < num_recs; i++) {
        uint8_t entry[8];
        if (fread(entry, 1, 8, fp) != 8) { free(offs); fclose(fp); return false; }
        offs[i] = be32(entry);
    }
    offs[num_recs] = (uint32_t)fsz;

    /* Read record 0 (PalmDOC + MOBI header) */
    uint32_t r0_off = offs[0];
    uint32_t r0_len = (offs[1] > r0_off) ? (offs[1] - r0_off) : 0;
    if (r0_len < 16 || r0_len > 65536) { free(offs); fclose(fp); return false; }
    uint8_t *r0 = malloc(r0_len);
    if (!r0) { free(offs); fclose(fp); return false; }
    fseek(fp, (long)r0_off, SEEK_SET);
    if (fread(r0, 1, r0_len, fp) != r0_len) { free(r0); free(offs); fclose(fp); return false; }

    uint16_t comp = be16(r0);
    uint16_t text_recs = be16(r0 + 8);
    if (comp != 1 && comp != 2) {
        free(r0); free(offs); fclose(fp);
        return false;
    }
    if (text_recs < 1) text_recs = 1;
    if (text_recs > num_recs - 1) text_recs = (uint16_t)(num_recs - 1);

    uint32_t enc = 1252;
    uint32_t extra_flags = 0;
    if (r0_len >= 132 && memcmp(r0 + 16, "MOBI", 4) == 0) {
        enc = be32(r0 + 28);
        uint32_t full_name_off = be32(r0 + 84);
        uint32_t full_name_len = be32(r0 + 88);
        if (full_name_off < r0_len && full_name_len > 0 && full_name_off + full_name_len <= r0_len) {
            size_t ulen = 0;
            char *ut = ensure_utf8(r0 + full_name_off, full_name_len, enc == 1252, &ulen);
            if (ut) {
                decode_entities_inplace(ut);
                snprintf(b->title, sizeof(b->title), "%s", ut);
                free(ut);
            }
        }
        uint32_t mobi_hdr_len = be32(r0 + 20);
        if (mobi_hdr_len >= 228 && r0_len >= 244) {
            extra_flags = be32(r0 + 240);
        }
    }
    free(r0);

    b->mobi_fp = fp;
    b->mobi_rec_off = offs;
    b->mobi_num_recs = num_recs;
    b->mobi_text_recs = text_recs;
    b->mobi_comp = comp;
    b->mobi_enc = enc;
    b->mobi_extra_flags = extra_flags;

    int ch_cnt = (text_recs + MOBI_RECS_PER_CH - 1) / MOBI_RECS_PER_CH;
    if (ch_cnt > KD_MAX_CHAPTERS) ch_cnt = KD_MAX_CHAPTERS;
    b->chapter_count = ch_cnt;
    for (int i = 0; i <= ch_cnt; i++) {
        b->mobi_cum_off[i] = (size_t)i * MOBI_RECS_PER_CH * 4096u;
    }
    b->total_size = (size_t)text_recs * 4096u;
    return true;
}

Book *book_open(const char *path) {
    BookFormat fmt = book_detect_format(path);
    if (fmt == BOOK_FMT_NONE) return NULL;
    Book *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->fmt = fmt;
    snprintf(b->path, sizeof(b->path), "%s", path);
    title_from_filename(path, b->title, sizeof(b->title));

    bool ok = false;
    if (fmt == BOOK_FMT_TXT) ok = open_txt_book(b);
    else if (fmt == BOOK_FMT_EPUB) ok = open_epub_book(b);
    else if (fmt == BOOK_FMT_MOBI) ok = open_mobi_book(b);

    if (!ok) {
        book_close(b);
        return NULL;
    }
    return b;
}

void book_close(Book *b) {
    if (!b) return;
    if (b->txt_fp) fclose(b->txt_fp);
    zip_close_arc(&b->epub_zip);
    if (b->mobi_fp) fclose(b->mobi_fp);
    free(b->mobi_rec_off);
    free(b);
}

const char *book_title(const Book *b) { return b ? b->title : ""; }
BookFormat book_format(const Book *b) { return b ? b->fmt : BOOK_FMT_NONE; }
int book_chapter_count(const Book *b) { return b ? b->chapter_count : 0; }
size_t book_total_size(const Book *b) { return b ? b->total_size : 0; }

size_t book_chapter_start_offset(const Book *b, int ch_idx) {
    if (!b || ch_idx < 0 || ch_idx > b->chapter_count) return 0;
    if (b->fmt == BOOK_FMT_TXT) return b->txt_off[ch_idx];
    if (b->fmt == BOOK_FMT_EPUB) return b->epub_cum_off[ch_idx];
    if (b->fmt == BOOK_FMT_MOBI) return b->mobi_cum_off[ch_idx];
    return 0;
}

char *book_load_chapter(Book *b, int ch_idx, size_t *out_len) {
    if (!b || ch_idx < 0 || ch_idx >= b->chapter_count) return NULL;
    *out_len = 0;

    if (b->fmt == BOOK_FMT_TXT) {
        size_t s = b->txt_off[ch_idx];
        size_t e = b->txt_off[ch_idx + 1];
        if (e <= s) return NULL;
        size_t len = e - s;
        uint8_t *raw = malloc(len + 1);
        if (!raw) return NULL;
        fseek(b->txt_fp, (long)s, SEEK_SET);
        size_t rd = fread(raw, 1, len, b->txt_fp);
        raw[rd] = '\0';
        size_t ulen = 0;
        char *utf8 = ensure_utf8(raw, rd, false, &ulen);
        free(raw);
        if (!utf8) return NULL;
        char *norm = normalize_plain_txt(utf8, ulen, out_len);
        free(utf8);
        return norm;
    }

    if (b->fmt == BOOK_FMT_EPUB) {
        const ZipEntry *ze = zip_find(&b->epub_zip, b->epub_chapters[ch_idx]);
        if (!ze) return NULL;
        size_t rlen = 0;
        uint8_t *raw = zip_extract(&b->epub_zip, ze, &rlen);
        if (!raw) return NULL;
        size_t ulen = 0;
        char *utf8 = ensure_utf8(raw, rlen, false, &ulen);
        free(raw);
        if (!utf8) return NULL;
        char *styled = html_to_styled_text(utf8, ulen, out_len);
        free(utf8);
        return styled;
    }

    if (b->fmt == BOOK_FMT_MOBI) {
        int rec_start = 1 + ch_idx * MOBI_RECS_PER_CH;
        int rec_end = rec_start + MOBI_RECS_PER_CH;
        if (rec_end > b->mobi_text_recs + 1) rec_end = b->mobi_text_recs + 1;
        if (ch_idx == b->chapter_count - 1) rec_end = b->mobi_text_recs + 1;

        size_t cap = (size_t)(rec_end - rec_start) * 4096u + 4096u;
        uint8_t *raw = malloc(cap + 1);
        if (!raw) return NULL;
        size_t total = 0;

        uint8_t rec_buf[16384];
        for (int r = rec_start; r < rec_end; r++) {
            uint32_t roff = b->mobi_rec_off[r];
            uint32_t rend = b->mobi_rec_off[r + 1];
            if (rend <= roff) continue;
            size_t rlen = rend - roff;
            if (rlen > sizeof(rec_buf)) rlen = sizeof(rec_buf);
            fseek(b->mobi_fp, (long)roff, SEEK_SET);
            if (fread(rec_buf, 1, rlen, b->mobi_fp) != rlen) continue;
            rlen = mobi_trim_extra(rec_buf, rlen, b->mobi_extra_flags);

            if (total + 8192 > cap) {
                cap *= 2;
                uint8_t *nr = realloc(raw, cap + 1);
                if (!nr) break;
                raw = nr;
            }
            if (b->mobi_comp == 1) {
                memcpy(raw + total, rec_buf, rlen);
                total += rlen;
            } else {
                total += palmdoc_decompress(rec_buf, rlen, raw + total, cap - total);
            }
        }
        /* If chunk ends with a split multi-byte UTF-8 sequence, complete it from next record (or trim) */
        if (total > 0 && b->mobi_enc != 1252) {
            size_t tail_need = 0;
            size_t tail_start = total;
            for (size_t back = 1; back <= 3 && back <= total; back++) {
                uint8_t c = raw[total - back];
                if ((c & 0xC0) == 0x80) continue;
                size_t expected = 1;
                if ((c & 0xE0) == 0xC0) expected = 2;
                else if ((c & 0xF0) == 0xE0) expected = 3;
                else if ((c & 0xF8) == 0xF0) expected = 4;
                if (expected > back) {
                    tail_need = expected - back;
                    tail_start = total - back;
                }
                break;
            }
            if (tail_need > 0) {
                bool completed = false;
                if (rec_end <= b->mobi_text_recs && rec_end < b->mobi_num_recs) {
                    uint32_t noff = b->mobi_rec_off[rec_end];
                    uint32_t nend = b->mobi_rec_off[rec_end + 1];
                    if (nend > noff) {
                        size_t nlen = nend - noff;
                        if (nlen > sizeof(rec_buf)) nlen = sizeof(rec_buf);
                        fseek(b->mobi_fp, (long)noff, SEEK_SET);
                        if (fread(rec_buf, 1, nlen, b->mobi_fp) == nlen) {
                            nlen = mobi_trim_extra(rec_buf, nlen, b->mobi_extra_flags);
                            uint8_t next_dec[8192];
                            size_t dlen = (b->mobi_comp == 1)
                                              ? (nlen < sizeof(next_dec) ? nlen : sizeof(next_dec))
                                              : palmdoc_decompress(rec_buf, nlen, next_dec, sizeof(next_dec));
                            if (b->mobi_comp == 1) memcpy(next_dec, rec_buf, dlen);
                            if (dlen >= tail_need && total + tail_need < cap) {
                                bool all_cont = true;
                                for (size_t k = 0; k < tail_need; k++) {
                                    if ((next_dec[k] & 0xC0) != 0x80) all_cont = false;
                                }
                                if (all_cont) {
                                    memcpy(raw + total, next_dec, tail_need);
                                    total += tail_need;
                                    completed = true;
                                }
                            }
                        }
                    }
                }
                if (!completed) total = tail_start;
            }
        }
        raw[total] = '\0';

        /* For non-first MOBI chunks, skip leading orphaned UTF-8 continuation bytes */
        size_t start_skip = 0;
        if (ch_idx > 0 && b->mobi_enc != 1252) {
            while (start_skip < total && start_skip < 4 && (raw[start_skip] & 0xC0) == 0x80) {
                start_skip++;
            }
        }

        size_t ulen = 0;
        char *utf8 = ensure_utf8(raw + start_skip, total - start_skip, b->mobi_enc == 1252, &ulen);
        free(raw);
        if (!utf8) return NULL;

        /* MOBI payload is almost always HTML; skip split opening-tag tail (e.g. 't="0em">') on chunk boundaries */
        const char *html_ptr = utf8;
        size_t html_len = ulen;
        bool is_html = (strchr(utf8, '<') != NULL);
        if (is_html && ch_idx > 0) {
            const char *lt = memchr(html_ptr, '<', html_len < 256 ? html_len : 256);
            const char *gt = memchr(html_ptr, '>', html_len < 256 ? html_len : 256);
            if (gt && (!lt || gt < lt)) {
                size_t skip = (size_t)(gt - html_ptr) + 1;
                html_ptr += skip;
                html_len -= skip;
            }
        }

        char *styled = is_html
                           ? html_to_styled_text(html_ptr, html_len, out_len)
                           : normalize_plain_txt(html_ptr, html_len, out_len);
        free(utf8);
        return styled;
    }
    return NULL;
}

/* =========================================================================
 * Library browser + Reading progress persistence
 * ========================================================================= */

typedef struct {
    char path[384];
    char title[160];
    BookFormat fmt;
    int ch_idx;
    size_t byte_off;
    int pct;
    long ts;
} ProgRec;

#define KD_MAX_PROG 128

static int load_all_progress(const Config *cfg, ProgRec *out, int max_n) {
    char path[320];
    snprintf(path, sizeof(path), "%s/progress.db", cfg->base_dir);
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    int n = 0;
    char line[768];
    while (fgets(line, sizeof(line), fp) && n < max_n) {
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
        /* Format: ts|fmt|ch|off|pct|title|path */
        char *p1 = strchr(line, '|'); if (!p1) continue; *p1++ = '\0';
        char *p2 = strchr(p1, '|');   if (!p2) continue; *p2++ = '\0';
        char *p3 = strchr(p2, '|');   if (!p3) continue; *p3++ = '\0';
        char *p4 = strchr(p3, '|');   if (!p4) continue; *p4++ = '\0';
        char *p5 = strchr(p4, '|');   if (!p5) continue; *p5++ = '\0';
        char *p6 = strchr(p5, '|');   if (!p6) continue; *p6++ = '\0';

        ProgRec *r = &out[n++];
        r->ts = atol(line);
        r->fmt = (BookFormat)atoi(p1);
        r->ch_idx = atoi(p2);
        r->byte_off = (size_t)strtoul(p3, NULL, 10);
        r->pct = atoi(p4);
        snprintf(r->title, sizeof(r->title), "%s", p5);
        snprintf(r->path, sizeof(r->path), "%s", p6);
    }
    fclose(fp);
    return n;
}

bool library_get_progress(const Config *cfg, const char *book_path,
                          int *ch_idx, size_t *byte_off, int *pct) {
    ProgRec *recs = calloc(KD_MAX_PROG, sizeof(ProgRec));
    if (!recs) return false;
    int n = load_all_progress(cfg, recs, KD_MAX_PROG);
    bool found = false;
    for (int i = 0; i < n; i++) {
        if (strcmp(recs[i].path, book_path) == 0) {
            if (ch_idx) *ch_idx = recs[i].ch_idx;
            if (byte_off) *byte_off = recs[i].byte_off;
            if (pct) *pct = recs[i].pct;
            found = true;
            break;
        }
    }
    free(recs);
    return found;
}

void library_save_progress(const Config *cfg, const char *book_path, const char *title,
                           BookFormat fmt, int ch_idx, size_t byte_off, int pct) {
    ProgRec *recs = calloc(KD_MAX_PROG, sizeof(ProgRec));
    if (!recs) return;
    int n = load_all_progress(cfg, recs, KD_MAX_PROG);

    int slot = -1;
    for (int i = 0; i < n; i++) {
        if (strcmp(recs[i].path, book_path) == 0) { slot = i; break; }
    }
    if (slot < 0) {
        if (n < KD_MAX_PROG) slot = n++;
        else {
            /* Evict oldest */
            slot = 0;
            for (int i = 1; i < n; i++) {
                if (recs[i].ts < recs[slot].ts) slot = i;
            }
        }
    }
    recs[slot].ts = (long)time(NULL);
    recs[slot].fmt = fmt;
    recs[slot].ch_idx = ch_idx;
    recs[slot].byte_off = byte_off;
    recs[slot].pct = pct;
    snprintf(recs[slot].title, sizeof(recs[0].title), "%s", title ? title : "");
    snprintf(recs[slot].path, sizeof(recs[0].path), "%s", book_path);

    char db_path[320], tmp_path[330];
    snprintf(db_path, sizeof(db_path), "%s/progress.db", cfg->base_dir);
    snprintf(tmp_path, sizeof(tmp_path), "%s/progress.db.tmp", cfg->base_dir);
    FILE *fp = fopen(tmp_path, "w");
    if (fp) {
        for (int i = 0; i < n; i++) {
            fprintf(fp, "%ld|%d|%d|%lu|%d|%s|%s\n",
                    recs[i].ts, (int)recs[i].fmt, recs[i].ch_idx,
                    (unsigned long)recs[i].byte_off, recs[i].pct,
                    recs[i].title, recs[i].path);
        }
        fflush(fp);
        fsync(fileno(fp));
        fclose(fp);
        rename(tmp_path, db_path);
    }
    free(recs);
}

static bool skip_dir_name(const char *name) {
    if (!name || name[0] == '.') return true;
    if (strcmp(name, "System Volume Information") == 0) return true;
    return false;
}

static int cmp_lib_entry(const void *a, const void *b) {
    const LibEntry *ea = (const LibEntry *)a;
    const LibEntry *eb = (const LibEntry *)b;
    if (ea->is_dir != eb->is_dir) return ea->is_dir ? -1 : 1;
    return strcasecmp(ea->name, eb->name);
}

void library_init(Library *lib, const char *root_dir, const Config *cfg) {
    memset(lib, 0, sizeof(*lib));
    const char *rd = (root_dir && root_dir[0]) ? root_dir : "/mnt/onboard";
    snprintf(lib->root_dir, sizeof(lib->root_dir), "%s", rd);
    snprintf(lib->cur_dir, sizeof(lib->cur_dir), "%s", rd);
    library_scan(lib, cfg);
}

void library_scan(Library *lib, const Config *cfg) {
    lib->count = 0;
    lib->has_recent = false;

    ProgRec *recs = calloc(KD_MAX_PROG, sizeof(ProgRec));
    int rec_n = recs ? load_all_progress(cfg, recs, KD_MAX_PROG) : 0;

    /* Find most recent existing book */
    long best_ts = -1;
    for (int i = 0; i < rec_n; i++) {
        if (recs[i].ts > best_ts && access(recs[i].path, R_OK) == 0) {
            best_ts = recs[i].ts;
            lib->has_recent = true;
            snprintf(lib->recent_path, sizeof(lib->recent_path), "%s", recs[i].path);
            snprintf(lib->recent_title, sizeof(lib->recent_title), "%s", recs[i].title);
            lib->recent_fmt = recs[i].fmt;
            lib->recent_pct = recs[i].pct;
        }
    }

    /* If at root_dir and external MicroSD (/mnt/sd) is mounted, expose it at top */
    if (strcmp(lib->cur_dir, lib->root_dir) == 0 && strcmp(lib->root_dir, "/mnt/onboard") == 0) {
        struct stat sdst;
        if (stat("/mnt/sd", &sdst) == 0 && S_ISDIR(sdst.st_mode)) {
            DIR *sdd = opendir("/mnt/sd");
            if (sdd) {
                bool has_any = false;
                struct dirent *sde;
                while ((sde = readdir(sdd)) != NULL) {
                    if (sde->d_name[0] != '.') { has_any = true; break; }
                }
                closedir(sdd);
                if (has_any && lib->count < KD_MAX_LIB_ENTRIES) {
                    LibEntry *e = &lib->entries[lib->count++];
                    snprintf(e->name, sizeof(e->name), "MicroSD Card");
                    snprintf(e->full_path, sizeof(e->full_path), "/mnt/sd");
                    e->is_dir = true;
                    e->fmt = BOOK_FMT_NONE;
                    e->progress_pct = -1;
                }
            }
        }
    }

    DIR *dp = opendir(lib->cur_dir);
    if (dp) {
        struct dirent *de;
        while ((de = readdir(dp)) != NULL && lib->count < KD_MAX_LIB_ENTRIES) {
            if (skip_dir_name(de->d_name)) continue;
            char full[384];
            int nw = snprintf(full, sizeof(full), "%s/%s", lib->cur_dir, de->d_name);
            if (nw <= 0 || (size_t)nw >= sizeof(full)) continue;

            struct stat st;
            if (stat(full, &st) != 0) continue;

            if (S_ISDIR(st.st_mode)) {
                LibEntry *e = &lib->entries[lib->count++];
                snprintf(e->name, sizeof(e->name), "%s", de->d_name);
                snprintf(e->full_path, sizeof(e->full_path), "%s", full);
                e->is_dir = true;
                e->fmt = BOOK_FMT_NONE;
                e->progress_pct = -1;
            } else if (S_ISREG(st.st_mode) && st.st_size > 0) {
                BookFormat fmt = book_detect_format(de->d_name);
                if (fmt == BOOK_FMT_NONE) continue;
                LibEntry *e = &lib->entries[lib->count++];
                title_from_filename(de->d_name, e->name, sizeof(e->name));
                snprintf(e->full_path, sizeof(e->full_path), "%s", full);
                e->is_dir = false;
                e->fmt = fmt;
                e->progress_pct = -1;
                for (int i = 0; i < rec_n; i++) {
                    if (strcmp(recs[i].path, full) == 0) {
                        e->progress_pct = recs[i].pct;
                        e->last_read = recs[i].ts;
                        break;
                    }
                }
            }
        }
        closedir(dp);
    }

    if (lib->count > 1) {
        qsort(lib->entries, (size_t)lib->count, sizeof(lib->entries[0]), cmp_lib_entry);
    }
    lib->page = 0;
    free(recs);
}

bool library_enter_dir(Library *lib, const char *dir_path, const Config *cfg) {
    if (!lib || !dir_path || !dir_path[0]) return false;
    struct stat st;
    if (stat(dir_path, &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    snprintf(lib->cur_dir, sizeof(lib->cur_dir), "%s", dir_path);
    library_scan(lib, cfg);
    return true;
}

bool library_go_up(Library *lib, const Config *cfg) {
    if (!lib) return false;
    if (strcmp(lib->cur_dir, lib->root_dir) == 0) return false;
    if (strcmp(lib->cur_dir, "/mnt/sd") == 0) {
        snprintf(lib->cur_dir, sizeof(lib->cur_dir), "%s", lib->root_dir);
        library_scan(lib, cfg);
        return true;
    }
    char *sl = strrchr(lib->cur_dir, '/');
    if (!sl || sl == lib->cur_dir) {
        snprintf(lib->cur_dir, sizeof(lib->cur_dir), "%s", lib->root_dir);
    } else {
        *sl = '\0';
        if (strlen(lib->cur_dir) < strlen(lib->root_dir) &&
            strncmp(lib->cur_dir, "/mnt/sd", 7) != 0) {
            snprintf(lib->cur_dir, sizeof(lib->cur_dir), "%s", lib->root_dir);
        }
    }
    library_scan(lib, cfg);
    return true;
}
