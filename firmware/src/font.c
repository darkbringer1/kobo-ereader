#include "kobodash.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#pragma GCC diagnostic pop

#define NUM_STYLES 5
#define GLYPH_CACHE_SLOTS 256

typedef struct {
    uint32_t cp;
    int size_px;
    int advance;
    int lsb;
    int w;
    int h;
    int xoff;
    int yoff;
    uint8_t *bitmap;
    bool valid;
} CachedGlyph;

typedef struct {
    uint8_t *ttf_data;
    bool owns_data;
    stbtt_fontinfo info;
    bool loaded;
    int ascent;
    int descent;
    int line_gap;
    CachedGlyph cache[GLYPH_CACHE_SLOTS];
} FontFace;

static FontFace g_faces[NUM_STYLES];

static uint8_t *load_file_bytes(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz <= 0 || sz > 16 * 1024 * 1024) { fclose(fp); return NULL; }
    rewind(fp);
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) { fclose(fp); return NULL; }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    return buf;
}

static void clear_face_cache(FontFace *f) {
    for (int i = 0; i < GLYPH_CACHE_SLOTS; i++) {
        if (f->cache[i].bitmap) {
            free(f->cache[i].bitmap);
            f->cache[i].bitmap = NULL;
        }
        f->cache[i].valid = false;
    }
}

static void unload_face(FontFace *f) {
    clear_face_cache(f);
    if (f->owns_data && f->ttf_data) {
        free(f->ttf_data);
    }
    f->ttf_data = NULL;
    f->owns_data = false;
    f->loaded = false;
}

static bool init_face_from_buf(FontFace *f, uint8_t *data, bool owns, int face_idx) {
    int offset = stbtt_GetFontOffsetForIndex(data, face_idx);
    if (offset < 0 || !stbtt_InitFont(&f->info, data, offset)) {
        return false;
    }
    f->ttf_data = data;
    f->owns_data = owns;
    f->loaded = true;
    stbtt_GetFontVMetrics(&f->info, &f->ascent, &f->descent, &f->line_gap);
    return true;
}

static bool load_face(FontFace *f, const char *path) {
    if (!path || !path[0]) return false;
    uint8_t *data = load_file_bytes(path);
    if (!data) return false;
    if (!init_face_from_buf(f, data, true, 0)) {
        free(data);
        return false;
    }
    return true;
}

static bool load_ttc_family(const char *path, int idx_reg, int idx_bold, int idx_ital) {
    if (!path || !path[0]) return false;
    uint8_t *data = load_file_bytes(path);
    if (!data) return false;
    if (!init_face_from_buf(&g_faces[FONT_STYLE_SERIF_REG], data, true, idx_reg)) {
        free(data);
        return false;
    }
    (void)init_face_from_buf(&g_faces[FONT_STYLE_SERIF_BOLD], data, false, idx_bold);
    (void)init_face_from_buf(&g_faces[FONT_STYLE_SERIF_ITALIC], data, false, idx_ital);
    return true;
}

static bool load_ttf_triple(const char *p_reg, const char *p_bold, const char *p_ital) {
    if (!load_face(&g_faces[FONT_STYLE_SERIF_REG], p_reg)) return false;
    (void)load_face(&g_faces[FONT_STYLE_SERIF_BOLD], p_bold);
    (void)load_face(&g_faces[FONT_STYLE_SERIF_ITALIC], p_ital);
    return true;
}

int font_set_book_family(const Config *cfg, const char *family) {
    unload_face(&g_faces[FONT_STYLE_SERIF_REG]);
    unload_face(&g_faces[FONT_STYLE_SERIF_BOLD]);
    unload_face(&g_faces[FONT_STYLE_SERIF_ITALIC]);

    const char *base = (cfg && cfg->base_dir[0]) ? cfg->base_dir : "/mnt/onboard/.adds/kobodash";
    const char *fam = (family && family[0]) ? family : "georgia";
    char p1[320], p2[320], p3[320];

    bool ok = false;
    if (strcmp(fam, "charter") == 0) {
        snprintf(p1, sizeof(p1), "%s/fonts/book-charter.ttc", base);
        ok = load_ttc_family(p1, 0, 3, 1) ||
             load_ttc_family("/System/Library/Fonts/Supplemental/Charter.ttc", 0, 3, 1);
    } else if (strcmp(fam, "palatino") == 0) {
        snprintf(p1, sizeof(p1), "%s/fonts/book-palatino.ttc", base);
        ok = load_ttc_family(p1, 0, 2, 1) ||
             load_ttc_family("/System/Library/Fonts/Palatino.ttc", 0, 2, 1);
    } else if (strcmp(fam, "baskerville") == 0) {
        snprintf(p1, sizeof(p1), "%s/fonts/book-baskerville.ttc", base);
        ok = load_ttc_family(p1, 0, 1, 2) ||
             load_ttc_family("/System/Library/Fonts/Supplemental/Baskerville.ttc", 0, 1, 2);
    } else if (strcmp(fam, "times") == 0) {
        snprintf(p1, sizeof(p1), "%s/fonts/book-times-reg.ttf", base);
        snprintf(p2, sizeof(p2), "%s/fonts/book-times-bold.ttf", base);
        snprintf(p3, sizeof(p3), "%s/fonts/book-times-italic.ttf", base);
        ok = load_ttf_triple(p1, p2, p3) ||
             load_ttf_triple("/System/Library/Fonts/Supplemental/Times New Roman.ttf",
                             "/System/Library/Fonts/Supplemental/Times New Roman Bold.ttf",
                             "/System/Library/Fonts/Supplemental/Times New Roman Italic.ttf");
    } else if (strcmp(fam, "verdana") == 0) {
        snprintf(p1, sizeof(p1), "%s/fonts/book-verdana-reg.ttf", base);
        snprintf(p2, sizeof(p2), "%s/fonts/book-verdana-bold.ttf", base);
        snprintf(p3, sizeof(p3), "%s/fonts/book-verdana-italic.ttf", base);
        ok = load_ttf_triple(p1, p2, p3) ||
             load_ttf_triple("/System/Library/Fonts/Supplemental/Verdana.ttf",
                             "/System/Library/Fonts/Supplemental/Verdana Bold.ttf",
                             "/System/Library/Fonts/Supplemental/Verdana Italic.ttf");
    } else if (strcmp(fam, "trebuchet") == 0) {
        snprintf(p1, sizeof(p1), "%s/fonts/book-trebuchet-reg.ttf", base);
        snprintf(p2, sizeof(p2), "%s/fonts/book-trebuchet-bold.ttf", base);
        snprintf(p3, sizeof(p3), "%s/fonts/book-trebuchet-italic.ttf", base);
        ok = load_ttf_triple(p1, p2, p3) ||
             load_ttf_triple("/System/Library/Fonts/Supplemental/Trebuchet MS.ttf",
                             "/System/Library/Fonts/Supplemental/Trebuchet MS Bold.ttf",
                             "/System/Library/Fonts/Supplemental/Trebuchet MS Italic.ttf");
    }

    /* Fallback / default: Georgia */
    if (!ok) {
        snprintf(p1, sizeof(p1), "%s/fonts/book-regular.ttf", base);
        snprintf(p2, sizeof(p2), "%s/fonts/book-bold.ttf", base);
        snprintf(p3, sizeof(p3), "%s/fonts/book-italic.ttf", base);
        ok = load_ttf_triple(p1, p2, p3) ||
             load_ttf_triple("/System/Library/Fonts/Supplemental/Georgia.ttf",
                             "/System/Library/Fonts/Supplemental/Georgia Bold.ttf",
                             "/System/Library/Fonts/Supplemental/Georgia Italic.ttf");
    }
    return ok ? 0 : -1;
}

int font_init(const Config *cfg) {
    memset(g_faces, 0, sizeof(g_faces));
    char path[320];

    const char *base = (cfg && cfg->base_dir[0]) ? cfg->base_dir : "/mnt/onboard/.adds/kobodash";
    (void)font_set_book_family(cfg, cfg ? cfg->reader_font_family : "georgia");

    snprintf(path, sizeof(path), "%s/fonts/ui-regular.ttf", base);
    if (!load_face(&g_faces[FONT_STYLE_UI_REG], path)) {
        (void)load_face(&g_faces[FONT_STYLE_UI_REG], "/System/Library/Fonts/Supplemental/Arial.ttf");
    }

    snprintf(path, sizeof(path), "%s/fonts/ui-bold.ttf", base);
    if (!load_face(&g_faces[FONT_STYLE_UI_BOLD], path)) {
        (void)load_face(&g_faces[FONT_STYLE_UI_BOLD], "/System/Library/Fonts/Supplemental/Arial Bold.ttf");
    }

    return font_is_ready() ? 0 : -1;
}

void font_close(void) {
    for (int i = 0; i < NUM_STYLES; i++) {
        unload_face(&g_faces[i]);
    }
}

bool font_is_ready(void) {
    for (int i = 0; i < NUM_STYLES; i++) {
        if (g_faces[i].loaded) return true;
    }
    return false;
}

static FontFace *pick_face(FontStyle style) {
    if ((int)style >= 0 && (int)style < NUM_STYLES && g_faces[style].loaded) {
        return &g_faces[style];
    }
    if (g_faces[FONT_STYLE_SERIF_REG].loaded) return &g_faces[FONT_STYLE_SERIF_REG];
    if (g_faces[FONT_STYLE_UI_REG].loaded) return &g_faces[FONT_STYLE_UI_REG];
    for (int i = 0; i < NUM_STYLES; i++) {
        if (g_faces[i].loaded) return &g_faces[i];
    }
    return NULL;
}

static uint32_t next_utf8(const char *s, size_t len, size_t *pos) {
    if (*pos >= len) return 0;
    unsigned char c = (unsigned char)s[*pos];
    if (c < 0x80) {
        (*pos)++;
        return c;
    }
    if ((c & 0xE0) == 0xC0 && *pos + 1 < len) {
        uint32_t cp = ((uint32_t)(c & 0x1F) << 6) | ((unsigned char)s[*pos + 1] & 0x3F);
        *pos += 2;
        return cp;
    }
    if ((c & 0xF0) == 0xE0 && *pos + 2 < len) {
        uint32_t cp = ((uint32_t)(c & 0x0F) << 12) |
                      (((uint32_t)(unsigned char)s[*pos + 1] & 0x3F) << 6) |
                      ((unsigned char)s[*pos + 2] & 0x3F);
        *pos += 3;
        return cp;
    }
    if ((c & 0xF8) == 0xF0 && *pos + 3 < len) {
        uint32_t cp = ((uint32_t)(c & 0x07) << 18) |
                      (((uint32_t)(unsigned char)s[*pos + 1] & 0x3F) << 12) |
                      (((uint32_t)(unsigned char)s[*pos + 2] & 0x3F) << 6) |
                      ((unsigned char)s[*pos + 3] & 0x3F);
        *pos += 4;
        return cp;
    }
    (*pos)++;
    return '?';
}

static CachedGlyph *get_glyph(FontFace *f, uint32_t cp, int size_px) {
    uint32_t slot = ((cp * 2654435761u) ^ (uint32_t)(size_px * 31)) & (GLYPH_CACHE_SLOTS - 1);
    CachedGlyph *g = &f->cache[slot];
    if (g->valid && g->cp == cp && g->size_px == size_px) {
        return g;
    }

    if (g->bitmap) {
        free(g->bitmap);
        g->bitmap = NULL;
    }

    float scale = stbtt_ScaleForPixelHeight(&f->info, (float)size_px);
    int glyph_idx = stbtt_FindGlyphIndex(&f->info, (int)cp);
    if (glyph_idx == 0 && cp > 127) {
        /* Fallback for uncommon typographic chars */
        if (cp == 0x2018 || cp == 0x2019 || cp == 0x201A) glyph_idx = stbtt_FindGlyphIndex(&f->info, '\'');
        else if (cp == 0x201C || cp == 0x201D || cp == 0x201E || cp == 0x00AB || cp == 0x00BB) glyph_idx = stbtt_FindGlyphIndex(&f->info, '"');
        else if (cp == 0x2013 || cp == 0x2014 || cp == 0x2212) glyph_idx = stbtt_FindGlyphIndex(&f->info, '-');
        else if (cp == 0x2026) glyph_idx = stbtt_FindGlyphIndex(&f->info, '.');
        else if (cp == 0x00A0) glyph_idx = stbtt_FindGlyphIndex(&f->info, ' ');
    }

    int adv = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&f->info, glyph_idx, &adv, &lsb);

    int w = 0, h = 0, xoff = 0, yoff = 0;
    uint8_t *bm = NULL;
    if (cp != ' ' && cp != '\t') {
        bm = stbtt_GetGlyphBitmap(&f->info, scale, scale, glyph_idx, &w, &h, &xoff, &yoff);
    }

    g->cp = cp;
    g->size_px = size_px;
    g->advance = (int)lroundf((float)adv * scale);
    g->lsb = (int)lroundf((float)lsb * scale);
    g->w = w;
    g->h = h;
    g->xoff = xoff;
    g->yoff = yoff;
    g->bitmap = bm;
    g->valid = true;
    return g;
}

void font_line_metrics(FontStyle style, int size_px, int *ascent, int *descent, int *line_h) {
    FontFace *f = pick_face(style);
    if (!f) {
        if (ascent) *ascent = (size_px * 4) / 5;
        if (descent) *descent = -(size_px / 5);
        if (line_h) *line_h = size_px + 4;
        return;
    }
    float scale = stbtt_ScaleForPixelHeight(&f->info, (float)size_px);
    int a = (int)ceilf((float)f->ascent * scale);
    int d = (int)floorf((float)f->descent * scale);
    int g = (int)ceilf((float)f->line_gap * scale);
    if (ascent) *ascent = a;
    if (descent) *descent = d;
    if (line_h) *line_h = a - d + g;
}

int font_measure_utf8(FontStyle style, int size_px, const char *utf8, size_t len) {
    FontFace *f = pick_face(style);
    if (!f || !utf8 || len == 0) return 0;
    int total = 0;
    size_t pos = 0;
    while (pos < len) {
        uint32_t cp = next_utf8(utf8, len, &pos);
        if (cp < 32) continue;
        CachedGlyph *g = get_glyph(f, cp, size_px);
        total += g->advance;
    }
    return total;
}

int font_draw_utf8(Canvas *c, FontStyle style, int size_px, int x, int baseline_y,
                   const char *utf8, size_t len, uint8_t fg_gray) {
    FontFace *f = pick_face(style);
    if (!c || !f || !utf8 || len == 0) return x;

    int cur_x = x;
    size_t pos = 0;
    while (pos < len) {
        uint32_t cp = next_utf8(utf8, len, &pos);
        if (cp < 32) continue;
        CachedGlyph *g = get_glyph(f, cp, size_px);
        if (g->bitmap && g->w > 0 && g->h > 0) {
            int gx0 = cur_x + g->xoff;
            int gy0 = baseline_y + g->yoff;
            for (int row = 0; row < g->h; row++) {
                int py = gy0 + row;
                if ((unsigned)py >= KD_HEIGHT) continue;
                uint8_t *dst_row = &c->pixels[py * KD_WIDTH];
                const uint8_t *src_row = &g->bitmap[row * g->w];
                for (int col = 0; col < g->w; col++) {
                    uint8_t alpha = src_row[col];
                    if (alpha == 0) continue;
                    int px = gx0 + col;
                    if ((unsigned)px >= KD_WIDTH) continue;
                    if (alpha == 255) {
                        dst_row[px] = fg_gray;
                    } else {
                        uint8_t bg = dst_row[px];
                        int blended = ((int)fg_gray * alpha + (int)bg * (255 - alpha)) / 255;
                        dst_row[px] = (uint8_t)blended;
                    }
                }
            }
        }
        cur_x += g->advance;
    }
    return cur_x;
}

int font_draw_wrapped(Canvas *c, FontStyle style, int size_px, int x, int y,
                      int max_w, int max_h, int extra_lead, const char *utf8, uint8_t fg_gray) {
    if (!c || !utf8 || !utf8[0] || max_w <= 20 || max_h <= 10) return y;
    int asc = 0, desc = 0, lh = 0;
    font_line_metrics(style, size_px, &asc, &desc, &lh);
    int step = lh + extra_lead;
    int space_w = font_measure_utf8(style, size_px, " ", 1);
    if (space_w < 3) space_w = size_px / 4;

    int cur_y = y;
    int cur_x = x;
    const char *p = utf8;
    while (*p && (cur_y + lh <= y + max_h)) {
        if (*p == '\n') {
            cur_y += step;
            cur_x = x;
            p++;
            continue;
        }
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        const char *ws = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        size_t wlen = (size_t)(p - ws);
        int ww = font_measure_utf8(style, size_px, ws, wlen);

        if (cur_x > x && cur_x + space_w + ww > x + max_w) {
            cur_y += step;
            cur_x = x;
            if (cur_y + lh > y + max_h) break;
        }
        if (cur_x > x) cur_x += space_w;

        /* Truncate overly long single word cleanly if wider than line */
        if (ww > max_w) {
            while (wlen > 1 && font_measure_utf8(style, size_px, ws, wlen) > max_w) {
                wlen--;
            }
        }
        cur_x = font_draw_utf8(c, style, size_px, cur_x, cur_y + asc, ws, wlen, fg_gray);
    }
    return cur_y + step;
}
