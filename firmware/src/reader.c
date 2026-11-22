#include "kobodash.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PAGE_WORDS 512
#define MAX_CH_PAGES   512

typedef struct {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
    char word[48];
} PageWord;

struct ReaderState {
    Book *book;
    DictSet *dicts;
    int pref_dict_idx;
    int active_dict_idx;

    int cur_ch;
    char *ch_text;
    size_t ch_len;

    size_t page_starts[MAX_CH_PAGES + 1];
    int page_count;
    int cur_page;

    PageWord words[MAX_PAGE_WORDS];
    int word_count;

    bool toolbar_open;
    bool lookup_tap_mode; /* When true, even quick tap looks up word! */
    int pages_since_flash;

    /* Active dictionary popup */
    bool dict_open;
    int dict_word_idx;
    char dict_query[64];
    char dict_matched[64];
    char dict_def[420];
    bool dict_found;
    bool dict_popup_top; /* Anchor popup at top if tapped word is near bottom */
};

typedef struct {
    uint8_t bg;
    uint8_t fg;
    uint8_t muted;
    uint8_t card_bg;
    uint8_t border;
    uint8_t subtle;
    uint8_t pill_bg;
    uint8_t pill_fg;
} RTheme;

static RTheme get_rtheme(const Config *cfg) {
    RTheme t;
    if (cfg && cfg->invert_mode) {
        t.bg = 0x00; t.fg = 0xFF; t.muted = 0xAA; t.card_bg = 0x1A;
        t.border = 0xBB; t.subtle = 0x44; t.pill_bg = 0xEE; t.pill_fg = 0x00;
    } else {
        t.bg = 0xFF; t.fg = 0x00; t.muted = 0x55; t.card_bg = 0xF4;
        t.border = 0x22; t.subtle = 0xCC; t.pill_bg = 0x11; t.pill_fg = 0xFF;
    }
    return t;
}

/* Reader layout geometry */
#define RD_MARGIN_L 38
#define RD_MARGIN_R 38
#define RD_TOP_Y    54
#define RD_BOT_Y    744
#define RD_TEXT_W   (KD_WIDTH - RD_MARGIN_L - RD_MARGIN_R)

static int reader_extra_lead(const Config *cfg) {
    int sp = cfg ? cfg->reader_line_spacing : 1;
    if (sp <= 0) return 3;
    if (sp == 1) return 7;
    return 12;
}

static int reader_font_size(const Config *cfg) {
    int px = cfg ? cfg->reader_font_px : 24;
    if (px < 16) px = 16;
    if (px > 36) px = 36;
    return px;
}

/* Typeset a single page starting at `start_off`, optionally drawing to Canvas and recording word hitboxes.
 * Returns the byte offset where the NEXT page begins. */
static size_t typeset_page(const char *text, size_t len, size_t start_off,
                           const Config *cfg, Canvas *c, const RTheme *th,
                           PageWord *words, int *word_count) {
    if (word_count) *word_count = 0;
    if (!text || start_off >= len) return len;

    int base_px = reader_font_size(cfg);
    int lead = reader_extra_lead(cfg);
    int asc = 0, desc = 0, lh = 0;
    font_line_metrics(FONT_STYLE_SERIF_REG, base_px, &asc, &desc, &lh);
    int step = lh + lead;
    int para_gap = step / 2 + 2;
    int space_w = font_measure_utf8(FONT_STYLE_SERIF_REG, base_px, " ", 1);
    if (space_w < 4) space_w = base_px / 4;

    bool bold = false;
    bool italic = false;
    bool heading = false;

    /* Recover active inline style if starting mid-chapter */
    for (size_t k = 0; k < start_off && k < len; k++) {
        char ch = text[k];
        if (ch == '\x01') bold = true;
        else if (ch == '\x02') bold = false;
        else if (ch == '\x03') italic = true;
        else if (ch == '\x04') italic = false;
        else if (ch == '\n') { bold = false; italic = false; heading = false; }
    }

    int cur_y = RD_TOP_Y;
    int cur_x = RD_MARGIN_L;
    size_t pos = start_off;

    while (pos < len) {
        /* Consume style control bytes */
        if (text[pos] == '\x01') { bold = true; pos++; continue; }
        if (text[pos] == '\x02') { bold = false; pos++; continue; }
        if (text[pos] == '\x03') { italic = true; pos++; continue; }
        if (text[pos] == '\x04') { italic = false; pos++; continue; }
        if (text[pos] == '\x05') { heading = true; pos++; continue; }

        if (text[pos] == '\n') {
            pos++;
            bool is_para = (pos < len && text[pos] == '\n');
            if (is_para) pos++;
            heading = false;
            bold = false;
            italic = false;
            if (cur_x > RD_MARGIN_L || cur_y > RD_TOP_Y) {
                cur_y += step + (is_para ? para_gap : 0);
                cur_x = RD_MARGIN_L;
            }
            if (cur_y + lh > RD_BOT_Y) break;
            continue;
        }

        if (text[pos] == ' ' || text[pos] == '\t') {
            pos++;
            continue;
        }

        /* Scan next word token */
        size_t wstart = pos;
        while (pos < len && text[pos] != ' ' && text[pos] != '\t' &&
               text[pos] != '\n' && (unsigned char)text[pos] >= 32) {
            pos++;
        }
        size_t wlen = pos - wstart;
        if (wlen == 0) continue;

        int use_px = heading ? (base_px + 4) : base_px;
        FontStyle st = FONT_STYLE_SERIF_REG;
        if (heading || bold) st = FONT_STYLE_SERIF_BOLD;
        else if (italic) st = FONT_STYLE_SERIF_ITALIC;

        int ww = font_measure_utf8(st, use_px, text + wstart, wlen);
        int need_space = (cur_x > RD_MARGIN_L) ? space_w : 0;

        if (cur_x > RD_MARGIN_L && cur_x + need_space + ww > RD_MARGIN_L + RD_TEXT_W) {
            cur_y += step;
            cur_x = RD_MARGIN_L;
            need_space = 0;
        }
        if (cur_y + lh > RD_BOT_Y) {
            /* Word does not fit on this page; rewind to start of this word */
            pos = wstart;
            break;
        }

        cur_x += need_space;
        if (c && th) {
            int draw_asc = asc;
            if (heading) font_line_metrics(st, use_px, &draw_asc, NULL, NULL);
            font_draw_utf8(c, st, use_px, cur_x, cur_y + draw_asc, text + wstart, wlen, th->fg);
        }
        if (words && word_count && *word_count < MAX_PAGE_WORDS) {
            PageWord *pw = &words[(*word_count)++];
            pw->x = (int16_t)cur_x;
            pw->y = (int16_t)cur_y;
            pw->w = (int16_t)(ww > 0 ? ww : 12);
            pw->h = (int16_t)lh;
            size_t cpy = wlen < sizeof(pw->word) - 1 ? wlen : sizeof(pw->word) - 1;
            memcpy(pw->word, text + wstart, cpy);
            pw->word[cpy] = '\0';
        }
        cur_x += ww;
    }

    if (pos == start_off && pos < len) {
        /* Guarantee forward progress even on absurdly huge unbreakable token */
        pos++;
    }
    return pos;
}

static void paginate_chapter(ReaderState *rs, const Config *cfg, size_t target_byte_off) {
    rs->page_count = 0;
    rs->cur_page = 0;
    if (!rs->ch_text || rs->ch_len == 0) {
        rs->page_starts[0] = 0;
        rs->page_starts[1] = 0;
        rs->page_count = 1;
        return;
    }

    size_t off = 0;
    while (off < rs->ch_len && rs->page_count < MAX_CH_PAGES) {
        rs->page_starts[rs->page_count++] = off;
        size_t next = typeset_page(rs->ch_text, rs->ch_len, off, cfg, NULL, NULL, NULL, NULL);
        if (next <= off) break;
        off = next;
    }
    rs->page_starts[rs->page_count] = rs->ch_len;
    if (rs->page_count == 0) {
        rs->page_starts[0] = 0;
        rs->page_starts[1] = 0;
        rs->page_count = 1;
    }

    for (int i = 0; i < rs->page_count; i++) {
        if (rs->page_starts[i] <= target_byte_off) {
            rs->cur_page = i;
        } else {
            break;
        }
    }
}

static int compute_book_pct(const ReaderState *rs) {
    if (!rs || !rs->book) return 0;
    int chs = book_chapter_count(rs->book);
    if (chs <= 0) return 0;
    double ch_frac = (rs->page_count > 0)
                         ? ((double)(rs->cur_page + 1) / (double)rs->page_count)
                         : 0.0;
    double overall = ((double)rs->cur_ch + ch_frac) / (double)chs;
    int pct = (int)(overall * 100.0 + 0.5);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

/* Store book path on ReaderState so saving progress always has exact path */
static char g_active_book_path[384] = {0};

static void persist_progress(const ReaderState *rs, const Config *cfg) {
    if (!rs || !rs->book || !cfg || !g_active_book_path[0]) return;
    size_t boff = (rs->cur_page < rs->page_count) ? rs->page_starts[rs->cur_page] : 0;
    int pct = compute_book_pct(rs);
    library_save_progress(cfg, g_active_book_path, book_title(rs->book),
                          book_format(rs->book), rs->cur_ch, boff, pct);
}

static bool load_chapter_idx(ReaderState *rs, int ch_idx, size_t byte_off, const Config *cfg) {
    if (!rs || !rs->book) return false;
    int chs = book_chapter_count(rs->book);
    if (ch_idx < 0 || ch_idx >= chs) return false;

    free(rs->ch_text);
    rs->ch_text = NULL;
    rs->ch_len = 0;
    rs->cur_ch = ch_idx;
    rs->ch_text = book_load_chapter(rs->book, ch_idx, &rs->ch_len);
    if (!rs->ch_text) {
        rs->ch_text = strdup(" ");
        rs->ch_len = 1;
    }
    paginate_chapter(rs, cfg, byte_off);
    rs->dict_open = false;
    return true;
}

ReaderState *reader_create(const Config *cfg, DictSet *dicts) {
    ReaderState *rs = calloc(1, sizeof(*rs));
    if (!rs) return NULL;
    rs->dicts = dicts;
    rs->pref_dict_idx = (cfg && dicts) ? dictset_find_by_id(dicts, cfg->dict_lang) : 0;
    rs->active_dict_idx = rs->pref_dict_idx;
    rs->dict_word_idx = -1;
    return rs;
}

void reader_destroy(ReaderState *rs) {
    if (!rs) return;
    free(rs->ch_text);
    book_close(rs->book);
    free(rs);
}

bool reader_has_book(const ReaderState *rs) {
    return rs && rs->book != NULL;
}

bool reader_open_book(ReaderState *rs, const char *path, const Config *cfg) {
    if (!rs || !path) return false;
    Book *b = book_open(path);
    if (!b) return false;

    if (rs->book) {
        persist_progress(rs, cfg);
        book_close(rs->book);
    }
    rs->book = b;
    snprintf(g_active_book_path, sizeof(g_active_book_path), "%s", path);
    rs->toolbar_open = false;
    rs->dict_open = false;
    rs->pages_since_flash = 0;

    int saved_ch = 0, saved_pct = 0;
    size_t saved_off = 0;
    if (!library_get_progress(cfg, path, &saved_ch, &saved_off, &saved_pct)) {
        saved_ch = 0;
        saved_off = 0;
    }
    if (saved_ch >= book_chapter_count(b)) saved_ch = 0;

    /* Skip leading empty cover/blank chapters if starting fresh */
    load_chapter_idx(rs, saved_ch, saved_off, cfg);
    if (saved_ch == 0 && saved_off == 0) {
        int tries = 0;
        while (rs->ch_len < 30 && rs->cur_ch + 1 < book_chapter_count(b) && tries < 4) {
            load_chapter_idx(rs, rs->cur_ch + 1, 0, cfg);
            tries++;
        }
    }
    persist_progress(rs, cfg);
    return true;
}

void reader_close_book(ReaderState *rs, const Config *cfg) {
    if (!rs || !rs->book) return;
    persist_progress(rs, cfg);
    free(rs->ch_text);
    rs->ch_text = NULL;
    rs->ch_len = 0;
    book_close(rs->book);
    rs->book = NULL;
    rs->toolbar_open = false;
    rs->dict_open = false;
    g_active_book_path[0] = '\0';
}

/* =========================================================================
 * Render Book Page (+ Dictionary Popup + Reader Toolbar)
 * ========================================================================= */

static void draw_centered_in_box(Canvas *c, FontStyle st, int px,
                                 int bx, int by, int bw, int bh,
                                 const char *text, uint8_t fg) {
    int asc = 0, desc = 0, lh = 0;
    font_line_metrics(st, px, &asc, &desc, &lh);
    int tw = font_measure_utf8(st, px, text, strlen(text));
    int tx = bx + (bw - tw) / 2;
    if (tx < bx + 4) tx = bx + 4;
    int ty = by + (bh - lh) / 2 + asc;
    font_draw_utf8(c, st, px, tx, ty, text, strlen(text), fg);
}

static void draw_left_in_box(Canvas *c, FontStyle st, int px,
                             int bx, int by, int bw, int bh,
                             const char *text, uint8_t fg) {
    int asc = 0, desc = 0, lh = 0;
    font_line_metrics(st, px, &asc, &desc, &lh);
    size_t len = strlen(text);
    char buf[160];
    snprintf(buf, sizeof(buf), "%s", text);
    len = strlen(buf);
    while (len > 3 && font_measure_utf8(st, px, buf, len) > bw) {
        len--;
        buf[len - 2] = '.';
        buf[len - 1] = '.';
        buf[len] = '\0';
    }
    int ty = by + (bh - lh) / 2 + asc;
    font_draw_utf8(c, st, px, bx, ty, buf, strlen(buf), fg);
}

static void draw_right_in_box(Canvas *c, FontStyle st, int px,
                              int bx, int by, int bw, int bh,
                              const char *text, uint8_t fg) {
    int asc = 0, desc = 0, lh = 0;
    font_line_metrics(st, px, &asc, &desc, &lh);
    int tw = font_measure_utf8(st, px, text, strlen(text));
    int tx = bx + bw - tw;
    if (tx < bx) tx = bx;
    int ty = by + (bh - lh) / 2 + asc;
    font_draw_utf8(c, st, px, tx, ty, text, strlen(text), fg);
}

void reader_render_page(Canvas *c, ReaderState *rs, const Config *cfg,
                        const Telemetry *tel, const struct tm *now_tm, KdRefreshMode mode) {
    RTheme th = get_rtheme(cfg);
    canvas_clear(c, th.bg);

    if (!rs || !rs->book) {
        draw_centered_in_box(c, FONT_STYLE_UI_BOLD, 24, 40, 340, 520, 60,
                             "No book open", th.fg);
        display_blit_canvas(c, NULL);
        display_refresh(NULL, mode);
        return;
    }

    /* Subtle header */
    draw_left_in_box(c, FONT_STYLE_UI_REG, 15, RD_MARGIN_L, 12, 410, 26,
                     book_title(rs->book), th.muted);
    char tbuf[32];
    if (now_tm) strftime(tbuf, sizeof(tbuf), "%H:%M", now_tm);
    else snprintf(tbuf, sizeof(tbuf), "--:--");
    if (rs->lookup_tap_mode) {
        char mtag[48];
        snprintf(mtag, sizeof(mtag), "LOOKUP  •  %s", tbuf);
        draw_right_in_box(c, FONT_STYLE_UI_BOLD, 15, 360, 12, 202, 26, mtag, th.fg);
    } else {
        draw_right_in_box(c, FONT_STYLE_UI_REG, 15, 450, 12, 112, 26, tbuf, th.muted);
    }
    canvas_hline(c, RD_MARGIN_L, 42, RD_TEXT_W, 1, th.subtle);

    /* Typeset & render book text directly onto Canvas */
    size_t start_off = (rs->cur_page < rs->page_count) ? rs->page_starts[rs->cur_page] : 0;
    (void)typeset_page(rs->ch_text, rs->ch_len, start_off, cfg, c, &th,
                       rs->words, &rs->word_count);

    /* Subtle footer */
    canvas_hline(c, RD_MARGIN_L, 756, RD_TEXT_W, 1, th.subtle);
    char foot_l[64], foot_r[64];
    int pct = compute_book_pct(rs);
    snprintf(foot_l, sizeof(foot_l), "Ch %d/%d  •  %d%%",
             rs->cur_ch + 1, book_chapter_count(rs->book), pct);
    if (tel && tel->bat_pct >= 0) {
        snprintf(foot_r, sizeof(foot_r), "p. %d/%d  •  %d%% bat",
                 rs->cur_page + 1, rs->page_count, tel->bat_pct);
    } else {
        snprintf(foot_r, sizeof(foot_r), "p. %d / %d",
                 rs->cur_page + 1, rs->page_count);
    }
    draw_left_in_box(c, FONT_STYLE_UI_REG, 15, RD_MARGIN_L, 762, 280, 28, foot_l, th.muted);
    draw_right_in_box(c, FONT_STYLE_UI_REG, 15, 300, 762, 262, 28, foot_r, th.muted);

    /* Highlight looked-up word & draw crisp dictionary card */
    if (rs->dict_open) {
        if (rs->dict_word_idx >= 0 && rs->dict_word_idx < rs->word_count) {
            const PageWord *pw = &rs->words[rs->dict_word_idx];
            canvas_hline(c, pw->x - 2, pw->y + pw->h + 1, pw->w + 4, 3, th.fg);
            canvas_stroke_rrect(c, pw->x - 4, pw->y - 2, pw->w + 8, pw->h + 6, 4, 2, th.fg);
        }

        int py = rs->dict_popup_top ? 52 : 504;
        int ph = 244;
        int px = 22, pw = 556;
        /* Crisp halo margin so underlying book lines never touch the popup border */
        canvas_fill_rrect(c, px - 6, py - 6, pw + 12, ph + 12, 18, th.bg);
        canvas_fill_rrect(c, px, py, pw, ph, 14, th.bg);
        canvas_stroke_rrect(c, px, py, pw, ph, 14, 3, th.fg);
        canvas_hline(c, px + 18, py + 48, pw - 36, 1, th.subtle);

        char head[128];
        if (rs->dict_found && rs->dict_matched[0] &&
            strcasecmp(rs->dict_matched, rs->dict_query) != 0) {
            snprintf(head, sizeof(head), "%s  (%s)", rs->dict_matched, rs->dict_query);
        } else {
            snprintf(head, sizeof(head), "%s", rs->dict_query);
        }
        draw_left_in_box(c, FONT_STYLE_UI_BOLD, 23, px + 20, py + 8, pw - 216, 36, head, th.fg);

        /* 1-tap Dictionary Language Switcher pill inside popup! */
        if (dictset_count(rs->dicts) > 0) {
            int lbx = px + pw - 190, lby = py + 8, lbw = 96, lbh = 34;
            canvas_fill_rrect(c, lbx, lby, lbw, lbh, 8, th.pill_bg);
            draw_centered_in_box(c, FONT_STYLE_UI_BOLD, 15, lbx, lby, lbw, lbh,
                                 dictset_label(rs->dicts, rs->active_dict_idx), th.pill_fg);
        }
        draw_right_in_box(c, FONT_STYLE_UI_REG, 15, px + pw - 88, py + 12, 70, 28,
                          "Close X", th.muted);

        const char *body = rs->dict_found ? rs->dict_def
                           : "No entry in this dictionary.\nTap the language badge above to try another language.";
        font_draw_wrapped(c, FONT_STYLE_SERIF_REG, 20, px + 20, py + 58,
                          pw - 40, ph - 72, 4, body, th.fg);
    }

    /* Sleek Reader Toolbar overlay */
    if (rs->toolbar_open) {
        /* Top bar (y=0..64) */
        canvas_fill_rect(c, 0, 0, KD_WIDTH, 66, th.card_bg);
        canvas_hline(c, 0, 66, KD_WIDTH, 2, th.fg);

        canvas_fill_rrect(c, 12, 12, 118, 42, 10, th.pill_bg);
        draw_centered_in_box(c, FONT_STYLE_UI_BOLD, 16, 12, 12, 118, 42, "< LIBRARY", th.pill_fg);

        canvas_fill_rrect(c, 138, 12, 136, 42, 10, rs->lookup_tap_mode ? th.pill_bg : th.bg);
        if (!rs->lookup_tap_mode) canvas_stroke_rrect(c, 138, 12, 136, 42, 10, 2, th.border);
        draw_centered_in_box(c, FONT_STYLE_UI_BOLD, 15, 138, 12, 136, 42,
                             rs->lookup_tap_mode ? "TAP: LOOKUP" : "HOLD: LOOKUP",
                             rs->lookup_tap_mode ? th.pill_fg : th.fg);

        char dlbl[32];
        snprintf(dlbl, sizeof(dlbl), "DICT: %s", dictset_label(rs->dicts, rs->pref_dict_idx));
        canvas_fill_rrect(c, 282, 12, 114, 42, 10, th.bg);
        canvas_stroke_rrect(c, 282, 12, 114, 42, 10, 2, th.border);
        draw_centered_in_box(c, FONT_STYLE_UI_BOLD, 15, 282, 12, 114, 42, dlbl, th.fg);

        canvas_fill_rrect(c, 404, 12, 86, 42, 10, th.bg);
        canvas_stroke_rrect(c, 404, 12, 86, 42, 10, 2, th.border);
        draw_centered_in_box(c, FONT_STYLE_UI_BOLD, 15, 404, 12, 86, 42, "FLASH", th.fg);

        canvas_fill_rrect(c, 498, 12, 90, 42, 10, th.bg);
        canvas_stroke_rrect(c, 498, 12, 90, 42, 10, 2, th.border);
        draw_centered_in_box(c, FONT_STYLE_UI_BOLD, 15, 498, 12, 90, 42, "CLOCK", th.fg);

        /* Bottom panel (y=588..800): 3 crisp rows (Font/Size/Theme, Spacing/Flash, Chapter Nav) */
        canvas_fill_rect(c, 0, 588, KD_WIDTH, 212, th.card_bg);
        canvas_hline(c, 0, 588, KD_WIDTH, 2, th.fg);

        /* Row 1 (y=602..650): A-, size, A+, Font Family, Light/Dark */
        canvas_fill_rrect(c, 20, 602, 68, 48, 10, th.bg);
        canvas_stroke_rrect(c, 20, 602, 68, 48, 10, 2, th.border);
        draw_centered_in_box(c, FONT_STYLE_UI_BOLD, 20, 20, 602, 68, 48, "A-", th.fg);

        char szb[24];
        snprintf(szb, sizeof(szb), "%dpx", reader_font_size(cfg));
        draw_centered_in_box(c, FONT_STYLE_UI_BOLD, 17, 90, 602, 64, 48, szb, th.fg);

        canvas_fill_rrect(c, 156, 602, 68, 48, 10, th.bg);
        canvas_stroke_rrect(c, 156, 602, 68, 48, 10, 2, th.border);
        draw_centered_in_box(c, FONT_STYLE_UI_BOLD, 20, 156, 602, 68, 48, "A+", th.fg);

        char flbl[48];
        snprintf(flbl, sizeof(flbl), "Font: %s", config_font_family_label(cfg->reader_font_family));
        canvas_fill_rrect(c, 236, 602, 192, 48, 10, th.bg);
        canvas_stroke_rrect(c, 236, 602, 192, 48, 10, 2, th.border);
        draw_centered_in_box(c, FONT_STYLE_UI_BOLD, 16, 236, 602, 192, 48, flbl, th.fg);

        canvas_fill_rrect(c, 440, 602, 140, 48, 10, cfg->invert_mode ? th.pill_bg : th.bg);
        if (!cfg->invert_mode) canvas_stroke_rrect(c, 440, 602, 140, 48, 10, 2, th.border);
        draw_centered_in_box(c, FONT_STYLE_UI_BOLD, 16, 440, 602, 140, 48,
                             cfg->invert_mode ? "Dark Mode" : "Light Mode",
                             cfg->invert_mode ? th.pill_fg : th.fg);

        /* Row 2 (y=664..712): Line spacing, Auto-Flash cadence (1p..20p), Instant Flash Once */
        const char *sp_lbl = (cfg->reader_line_spacing == 0) ? "Space: Tight"
                           : (cfg->reader_line_spacing == 2) ? "Space: Wide" : "Space: Norm";
        canvas_fill_rrect(c, 20, 664, 176, 48, 10, th.bg);
        canvas_stroke_rrect(c, 20, 664, 176, 48, 10, 2, th.border);
        draw_centered_in_box(c, FONT_STYLE_UI_BOLD, 16, 20, 664, 176, 48, sp_lbl, th.fg);

        char frb[48];
        if (cfg->reader_flash_every <= 1) {
            snprintf(frb, sizeof(frb), "Flash: Every Page");
        } else {
            snprintf(frb, sizeof(frb), "Flash: Every %dp", cfg->reader_flash_every);
        }
        canvas_fill_rrect(c, 208, 664, 220, 48, 10, cfg->reader_flash_every <= 1 ? th.pill_bg : th.bg);
        if (cfg->reader_flash_every > 1) canvas_stroke_rrect(c, 208, 664, 220, 48, 10, 2, th.border);
        draw_centered_in_box(c, FONT_STYLE_UI_BOLD, 16, 208, 664, 220, 48, frb,
                             cfg->reader_flash_every <= 1 ? th.pill_fg : th.fg);

        canvas_fill_rrect(c, 440, 664, 140, 48, 10, th.pill_bg);
        draw_centered_in_box(c, FONT_STYLE_UI_BOLD, 16, 440, 664, 140, 48, "Flash Once", th.pill_fg);

        /* Row 3 (y=726..778): Chapter navigation */
        canvas_fill_rrect(c, 20, 726, 140, 52, 10, th.bg);
        canvas_stroke_rrect(c, 20, 726, 140, 52, 10, 2, th.border);
        draw_centered_in_box(c, FONT_STYLE_UI_BOLD, 17, 20, 726, 140, 52, "<< PREV CH", th.fg);

        char chb[64];
        snprintf(chb, sizeof(chb), "Ch %d / %d  •  %d%%",
                 rs->cur_ch + 1, book_chapter_count(rs->book), pct);
        draw_centered_in_box(c, FONT_STYLE_UI_BOLD, 18, 172, 726, 256, 52, chb, th.fg);

        canvas_fill_rrect(c, 440, 726, 140, 52, 10, th.bg);
        canvas_stroke_rrect(c, 440, 726, 140, 52, 10, 2, th.border);
        draw_centered_in_box(c, FONT_STYLE_UI_BOLD, 17, 440, 726, 140, 52, "NEXT CH >>", th.fg);
    }

    display_blit_canvas(c, NULL);
    display_refresh(NULL, mode);
}

/* =========================================================================
 * Render Library Screen (Fast Folder Browser + Continue Reading Card)
 * ========================================================================= */

#define LIB_ROW_H      58
#define LIB_ROW_GAP    8
#define LIB_ROW_STRIDE (LIB_ROW_H + LIB_ROW_GAP)
#define LIB_ROWS_ROOT  8
#define LIB_ROWS_SUB   9

static int lib_rows_per_page(const Library *lib) {
    bool at_root = (strcmp(lib->cur_dir, lib->root_dir) == 0);
    return (at_root && lib->has_recent) ? LIB_ROWS_ROOT : LIB_ROWS_SUB;
}

static int lib_list_top_y(const Library *lib) {
    bool at_root = (strcmp(lib->cur_dir, lib->root_dir) == 0);
    return (at_root && lib->has_recent) ? 168 : 104;
}

void reader_render_library(Canvas *c, const Library *lib, const Config *cfg,
                           const Telemetry *tel, const SyncState *sync,
                           const Gallery *gal, KdRefreshMode mode) {
    (void)tel; (void)sync;
    RTheme th = get_rtheme(cfg);
    canvas_clear(c, th.bg);

    /* Top header */
    canvas_hline(c, 24, 58, 552, 2, th.subtle);
    draw_left_in_box(c, FONT_STYLE_UI_BOLD, 21, 24, 10, 320, 42, "LIBRARY", th.fg);

    bool at_root = (strcmp(lib->cur_dir, lib->root_dir) == 0);
    if (!at_root) {
        canvas_fill_rrect(c, 412, 12, 102, 36, 10, th.pill_bg);
        draw_centered_in_box(c, FONT_STYLE_UI_BOLD, 16, 412, 12, 102, 36, "^ UP", th.pill_fg);
    } else {
        canvas_draw_refresh_icon(c, 504, 30, 11, th.fg, th.bg);
    }
    canvas_draw_gear(c, 558, 30, 10, th.fg, th.bg);

    /* Breadcrumb path bar */
    const char *rel = lib->cur_dir;
    if (strncmp(rel, lib->root_dir, strlen(lib->root_dir)) == 0) {
        rel += strlen(lib->root_dir);
        if (*rel == '/') rel++;
    }
    char crumb[160];
    snprintf(crumb, sizeof(crumb), "%s  (%d items)",
             (*rel ? rel : "All Books & Folders"), lib->count);
    draw_left_in_box(c, FONT_STYLE_UI_REG, 16, 24, 66, 552, 28, crumb, th.muted);

    /* Continue Reading card (only on root if a recent book exists) */
    if (at_root && lib->has_recent) {
        canvas_fill_rrect(c, 24, 98, 552, 58, 12, th.pill_bg);
        char cont[160];
        snprintf(cont, sizeof(cont), "CONTINUE:  %s", lib->recent_title);
        draw_left_in_box(c, FONT_STYLE_UI_BOLD, 18, 40, 106, 444, 42, cont, th.pill_fg);
        char pctb[24];
        snprintf(pctb, sizeof(pctb), "%d%%  >", lib->recent_pct);
        draw_right_in_box(c, FONT_STYLE_UI_BOLD, 18, 488, 106, 74, 42, pctb, th.pill_fg);
    }

    int per_page = lib_rows_per_page(lib);
    int top_y = lib_list_top_y(lib);
    int total_pages = (lib->count + per_page - 1) / per_page;
    if (total_pages < 1) total_pages = 1;
    int page = lib->page;
    if (page < 0) page = 0;
    if (page >= total_pages) page = total_pages - 1;

    int start_idx = page * per_page;
    if (lib->count == 0) {
        canvas_fill_rrect(c, 40, 260, 520, 160, 14, th.card_bg);
        canvas_stroke_rrect(c, 40, 260, 520, 160, 14, 2, th.border);
        draw_centered_in_box(c, FONT_STYLE_UI_BOLD, 22, 56, 290, 488, 40,
                             "No EPUB / MOBI / TXT books here", th.fg);
        draw_centered_in_box(c, FONT_STYLE_UI_REG, 17, 56, 340, 488, 40,
                             "Copy books to KOBOeReader or MicroSD card", th.muted);
    } else {
        for (int r = 0; r < per_page && (start_idx + r) < lib->count; r++) {
            const LibEntry *e = &lib->entries[start_idx + r];
            int y = top_y + r * LIB_ROW_STRIDE;

            canvas_fill_rrect(c, 24, y, 552, LIB_ROW_H, 10, th.card_bg);
            canvas_stroke_rrect(c, 24, y, 552, LIB_ROW_H, 10, 1, th.border);

            /* Left badge (DIR / EPUB / MOBI / TXT) */
            int bx = 36, by = y + 13, bw = 62, bh = 32;
            if (e->is_dir) {
                canvas_fill_rrect(c, bx, by, bw, bh, 7, th.pill_bg);
                draw_centered_in_box(c, FONT_STYLE_UI_BOLD, 14, bx, by, bw, bh, "DIR", th.pill_fg);
            } else {
                canvas_fill_rrect(c, bx, by, bw, bh, 7, th.bg);
                canvas_stroke_rrect(c, bx, by, bw, bh, 7, 1, th.border);
                draw_centered_in_box(c, FONT_STYLE_UI_BOLD, 14, bx, by, bw, bh,
                                     book_format_tag(e->fmt), th.fg);
            }

            /* Title */
            draw_left_in_box(c, e->is_dir ? FONT_STYLE_UI_BOLD : FONT_STYLE_UI_REG,
                             19, 112, y + 8, 392, 42, e->name, th.fg);

            /* Right chevron or progress % */
            if (e->is_dir) {
                draw_right_in_box(c, FONT_STYLE_UI_BOLD, 20, 512, y + 8, 48, 42, ">", th.muted);
            } else if (e->progress_pct >= 0) {
                char pb[16];
                snprintf(pb, sizeof(pb), "%d%%", e->progress_pct);
                draw_right_in_box(c, FONT_STYLE_UI_BOLD, 16, 506, y + 8, 56, 42, pb, th.fg);
            }
        }
    }

    /* Bottom bar: list pagination + screen switcher */
    canvas_hline(c, 24, 728, 552, 1, th.subtle);
    if (total_pages > 1) {
        if (page > 0) {
            canvas_fill_rrect(c, 24, 738, 128, 46, 10, th.bg);
            canvas_stroke_rrect(c, 24, 738, 128, 46, 10, 2, th.border);
            draw_centered_in_box(c, FONT_STYLE_UI_BOLD, 16, 24, 738, 128, 46, "< PAGE", th.fg);
        }
        if (page + 1 < total_pages) {
            canvas_fill_rrect(c, 448, 738, 128, 46, 10, th.bg);
            canvas_stroke_rrect(c, 448, 738, 128, 46, 10, 2, th.border);
            draw_centered_in_box(c, FONT_STYLE_UI_BOLD, 16, 448, 738, 128, 46, "PAGE >", th.fg);
        }
        char pinfo[48];
        snprintf(pinfo, sizeof(pinfo), "Page %d / %d  •  Swipe for Clock", page + 1, total_pages);
        draw_centered_in_box(c, FONT_STYLE_UI_REG, 15, 156, 742, 288, 38, pinfo, th.muted);
    } else {
        draw_left_in_box(c, FONT_STYLE_UI_BOLD, 17, 24, 742, 120, 40, "< CLOCK", th.muted);
        draw_right_in_box(c, FONT_STYLE_UI_BOLD, 17, 456, 742, 120, 40, "NEXT >", th.muted);
        draw_centered_in_box(c, FONT_STYLE_UI_REG, 15, 156, 742, 288, 40,
                             "Hold HOME 2s for Stock UI", th.muted);
    }
    (void)gal;

    display_blit_canvas(c, NULL);
    display_refresh(NULL, mode);
}

/* =========================================================================
 * Input Handling for Library & Book Reader
 * ========================================================================= */

static bool hit_box(int x, int y, int rx, int ry, int rw, int rh) {
    return (x >= rx && x < rx + rw && y >= ry && y < ry + rh);
}

bool reader_handle_library_input(Library *lib, ReaderState *rs, const InputEvent *ev,
                                 const Config *cfg, ScreenId *out_screen,
                                 bool *out_open_settings, bool *out_exit_stock,
                                 bool *out_prev_screen, bool *out_next_screen,
                                 bool *out_sync_now, bool *out_need_full) {
    *out_screen = SCREEN_LIBRARY;
    *out_open_settings = false;
    *out_exit_stock = false;
    *out_prev_screen = false;
    *out_next_screen = false;
    *out_sync_now = false;
    *out_need_full = false;

    if (!ev || ev->type == INPUT_NONE) return false;

    if (ev->type == INPUT_HOME_LONG) { *out_exit_stock = true; return true; }
    if (ev->type == INPUT_HOME_SHORT) {
        if (strcmp(lib->cur_dir, lib->root_dir) != 0) {
            library_go_up(lib, cfg);
            *out_need_full = true;
            return true;
        }
        *out_next_screen = true;
        return true;
    }
    if (ev->type == INPUT_SWIPE_LEFT) { *out_next_screen = true; return true; }
    if (ev->type == INPUT_SWIPE_RIGHT) { *out_prev_screen = true; return true; }
    if (ev->type == INPUT_SWIPE_UP) { *out_open_settings = true; return true; }
    if (ev->type == INPUT_SWIPE_DOWN || ev->type == INPUT_POWER_SHORT) {
        library_scan(lib, cfg);
        *out_need_full = true;
        return true;
    }
    if (ev->type != INPUT_TAP && ev->type != INPUT_LONG_TAP) return false;

    int x = ev->x, y = ev->y;
    bool at_root = (strcmp(lib->cur_dir, lib->root_dir) == 0);

    /* Top-right gear -> Settings */
    if (hit_box(x, y, 526, 0, 74, 68)) {
        *out_open_settings = true;
        return true;
    }
    /* ^ UP button when inside subfolder */
    if (!at_root && hit_box(x, y, 400, 0, 124, 68)) {
        library_go_up(lib, cfg);
        *out_need_full = true;
        return true;
    }
    /* Refresh icon at root */
    if (at_root && hit_box(x, y, 464, 0, 62, 68)) {
        library_scan(lib, cfg);
        *out_sync_now = true;
        *out_need_full = true;
        return true;
    }

    /* Continue Reading card */
    if (at_root && lib->has_recent && hit_box(x, y, 24, 96, 552, 62)) {
        if (reader_open_book(rs, lib->recent_path, cfg)) {
            *out_screen = SCREEN_READER;
            *out_need_full = true;
            return true;
        }
    }

    /* Bottom pagination or screen nav */
    int per_page = lib_rows_per_page(lib);
    int total_pages = (lib->count + per_page - 1) / per_page;
    if (total_pages < 1) total_pages = 1;

    if (hit_box(x, y, 0, 728, 175, 72)) {
        if (total_pages > 1 && lib->page > 0) {
            lib->page--;
            return true;
        }
        if (total_pages <= 1) {
            *out_prev_screen = true;
            return true;
        }
        return false;
    }
    if (hit_box(x, y, 425, 728, 175, 72)) {
        if (total_pages > 1 && lib->page + 1 < total_pages) {
            lib->page++;
            return true;
        }
        if (total_pages <= 1) {
            *out_next_screen = true;
            return true;
        }
        return false;
    }

    /* List row taps */
    int top_y = lib_list_top_y(lib);
    int start_idx = lib->page * per_page;
    for (int r = 0; r < per_page && (start_idx + r) < lib->count; r++) {
        int ry = top_y + r * LIB_ROW_STRIDE;
        if (!hit_box(x, y, 24, ry, 552, LIB_ROW_H)) continue;
        const LibEntry *e = &lib->entries[start_idx + r];
        if (e->is_dir) {
            char target[384];
            snprintf(target, sizeof(target), "%s", e->full_path);
            library_enter_dir(lib, target, cfg);
            *out_need_full = true;
            return true;
        } else {
            if (reader_open_book(rs, e->full_path, cfg)) {
                *out_screen = SCREEN_READER;
                *out_need_full = true;
                return true;
            }
        }
    }
    return false;
}

static int find_word_at(const ReaderState *rs, int x, int y) {
    int best = -1;
    int best_dist = 1000000;
    for (int i = 0; i < rs->word_count; i++) {
        const PageWord *pw = &rs->words[i];
        int dx = 0, dy = 0;
        if (x < pw->x) dx = pw->x - x;
        else if (x > pw->x + pw->w) dx = x - (pw->x + pw->w);
        if (y < pw->y) dy = pw->y - y;
        else if (y > pw->y + pw->h) dy = y - (pw->y + pw->h);
        if (dx <= 16 && dy <= 16) {
            int d = dx * dx + dy * dy;
            if (d < best_dist) {
                best_dist = d;
                best = i;
            }
        }
    }
    return best;
}

static void run_dict_query(ReaderState *rs, const char *word, bool allow_fallback) {
    rs->dict_matched[0] = '\0';
    rs->dict_def[0] = '\0';
    int used = rs->pref_dict_idx;
    rs->dict_found = dictset_lookup(rs->dicts, rs->pref_dict_idx, allow_fallback,
                                    word, &used,
                                    rs->dict_matched, sizeof(rs->dict_matched),
                                    rs->dict_def, sizeof(rs->dict_def));
    rs->active_dict_idx = used;
}

static void trigger_word_lookup(ReaderState *rs, int word_idx) {
    if (word_idx < 0 || word_idx >= rs->word_count) return;
    const PageWord *pw = &rs->words[word_idx];
    rs->dict_open = true;
    rs->dict_word_idx = word_idx;
    snprintf(rs->dict_query, sizeof(rs->dict_query), "%s", pw->word);
    run_dict_query(rs, pw->word, true);
    rs->dict_popup_top = (pw->y > 440);
}

void reader_debug_lookup_word(ReaderState *rs, const char *word) {
    if (!rs || !word) return;
    for (int i = 0; i < rs->word_count; i++) {
        if (strncasecmp(rs->words[i].word, word, strlen(word)) == 0) {
            trigger_word_lookup(rs, i);
            return;
        }
    }
    rs->dict_open = true;
    rs->dict_word_idx = -1;
    snprintf(rs->dict_query, sizeof(rs->dict_query), "%s", word);
    run_dict_query(rs, word, true);
    rs->dict_popup_top = false;
}

static KdRefreshMode turn_page(ReaderState *rs, int delta, const Config *cfg) {
    if (!rs || !rs->book) return KD_REFRESH_GRAY_PARTIAL;
    int chs = book_chapter_count(rs->book);

    if (delta > 0) {
        if (rs->cur_page + 1 < rs->page_count) {
            rs->cur_page++;
        } else if (rs->cur_ch + 1 < chs) {
            load_chapter_idx(rs, rs->cur_ch + 1, 0, cfg);
        } else {
            return KD_REFRESH_GRAY_PARTIAL;
        }
    } else if (delta < 0) {
        if (rs->cur_page > 0) {
            rs->cur_page--;
        } else if (rs->cur_ch > 0) {
            load_chapter_idx(rs, rs->cur_ch - 1, 0, cfg);
            rs->cur_page = rs->page_count > 0 ? rs->page_count - 1 : 0;
        } else {
            return KD_REFRESH_GRAY_PARTIAL;
        }
    }

    persist_progress(rs, cfg);
    rs->pages_since_flash++;
    int flash_every = (cfg && cfg->reader_flash_every > 0) ? cfg->reader_flash_every : 5;
    if (rs->pages_since_flash >= flash_every) {
        rs->pages_since_flash = 0;
        return KD_REFRESH_FULL_FLASH;
    }
    /* Use WFM_GC16 non-flashing grayscale (never 1-bit WFM_DU!) so anti-aliased serif glyphs stay razor-sharp */
    return KD_REFRESH_GRAY_PARTIAL;
}

bool reader_handle_book_input(ReaderState *rs, Library *lib, const InputEvent *ev,
                              Config *cfg, bool *cfg_dirty,
                              ScreenId *out_screen, bool *out_exit_stock,
                              KdRefreshMode *out_refresh_mode) {
    *cfg_dirty = false;
    *out_screen = SCREEN_READER;
    *out_exit_stock = false;
    *out_refresh_mode = KD_REFRESH_FAST_DU;

    if (!rs || !ev || ev->type == INPUT_NONE) return false;

    if (ev->type == INPUT_HOME_LONG) {
        persist_progress(rs, cfg);
        *out_exit_stock = true;
        return true;
    }
    if (ev->type == INPUT_POWER_SHORT) {
        rs->pages_since_flash = 0;
        *out_refresh_mode = KD_REFRESH_FULL_FLASH;
        return true;
    }
    if (ev->type == INPUT_HOME_SHORT) {
        if (rs->dict_open) {
            rs->dict_open = false;
            *out_refresh_mode = KD_REFRESH_GRAY_PARTIAL;
            return true;
        }
        if (rs->toolbar_open) {
            rs->toolbar_open = false;
            *out_refresh_mode = KD_REFRESH_GRAY_PARTIAL;
            return true;
        }
        persist_progress(rs, cfg);
        library_scan(lib, cfg);
        *out_screen = SCREEN_LIBRARY;
        *out_refresh_mode = KD_REFRESH_FULL_FLASH;
        return true;
    }

    /* If dictionary popup is open: tap language pill switches language inline; any other tap closes */
    if (rs->dict_open) {
        if ((ev->type == INPUT_TAP || ev->type == INPUT_LONG_TAP) && dictset_count(rs->dicts) > 1) {
            int py = rs->dict_popup_top ? 52 : 504;
            int px = 22, pw = 556;
            if (hit_box(ev->x, ev->y, px + pw - 200, py, 114, 52)) {
                rs->pref_dict_idx = (rs->active_dict_idx + 1) % dictset_count(rs->dicts);
                snprintf(cfg->dict_lang, sizeof(cfg->dict_lang), "%s",
                         dictset_id(rs->dicts, rs->pref_dict_idx));
                *cfg_dirty = true;
                run_dict_query(rs, rs->dict_query, false);
                *out_refresh_mode = KD_REFRESH_FAST_DU;
                return true;
            }
        }
        rs->dict_open = false;
        *out_refresh_mode = KD_REFRESH_GRAY_PARTIAL;
        return true;
    }

    /* Toolbar interaction */
    if (rs->toolbar_open) {
        if (ev->type == INPUT_SWIPE_DOWN) {
            rs->toolbar_open = false;
            *out_refresh_mode = KD_REFRESH_GRAY_PARTIAL;
            return true;
        }
        if (ev->type != INPUT_TAP && ev->type != INPUT_LONG_TAP) return false;
        int x = ev->x, y = ev->y;

        /* Top bar buttons */
        if (hit_box(x, y, 8, 0, 126, 66)) {
            persist_progress(rs, cfg);
            rs->toolbar_open = false;
            library_scan(lib, cfg);
            *out_screen = SCREEN_LIBRARY;
            *out_refresh_mode = KD_REFRESH_FULL_FLASH;
            return true;
        }
        if (hit_box(x, y, 134, 0, 144, 66)) {
            rs->lookup_tap_mode = !rs->lookup_tap_mode;
            *out_refresh_mode = KD_REFRESH_FAST_DU;
            return true;
        }
        if (hit_box(x, y, 278, 0, 122, 66)) {
            if (dictset_count(rs->dicts) > 1) {
                rs->pref_dict_idx = (rs->pref_dict_idx + 1) % dictset_count(rs->dicts);
                rs->active_dict_idx = rs->pref_dict_idx;
                snprintf(cfg->dict_lang, sizeof(cfg->dict_lang), "%s",
                         dictset_id(rs->dicts, rs->pref_dict_idx));
                *cfg_dirty = true;
                *out_refresh_mode = KD_REFRESH_FAST_DU;
                return true;
            }
            return false;
        }
        if (hit_box(x, y, 400, 0, 94, 66)) {
            rs->toolbar_open = false;
            rs->pages_since_flash = 0;
            *out_refresh_mode = KD_REFRESH_FULL_FLASH;
            return true;
        }
        if (hit_box(x, y, 494, 0, 106, 66)) {
            persist_progress(rs, cfg);
            rs->toolbar_open = false;
            *out_screen = SCREEN_CLOCK;
            *out_refresh_mode = KD_REFRESH_FULL_FLASH;
            return true;
        }

        /* Bottom panel row 1 (y=602..650): A- / A+ / Font Family / Theme */
        size_t cur_off = (rs->cur_page < rs->page_count) ? rs->page_starts[rs->cur_page] : 0;
        if (hit_box(x, y, 16, 594, 76, 62)) {
            if (cfg->reader_font_px > 18) {
                cfg->reader_font_px -= 2;
                *cfg_dirty = true;
                paginate_chapter(rs, cfg, cur_off);
                *out_refresh_mode = KD_REFRESH_GRAY_PARTIAL;
                return true;
            }
            return false;
        }
        if (hit_box(x, y, 152, 594, 76, 62)) {
            if (cfg->reader_font_px < 34) {
                cfg->reader_font_px += 2;
                *cfg_dirty = true;
                paginate_chapter(rs, cfg, cur_off);
                *out_refresh_mode = KD_REFRESH_GRAY_PARTIAL;
                return true;
            }
            return false;
        }
        if (hit_box(x, y, 232, 594, 200, 62)) {
            snprintf(cfg->reader_font_family, sizeof(cfg->reader_font_family), "%s",
                     config_next_font_family(cfg->reader_font_family));
            (void)font_set_book_family(cfg, cfg->reader_font_family);
            *cfg_dirty = true;
            paginate_chapter(rs, cfg, cur_off);
            *out_refresh_mode = KD_REFRESH_FULL_FLASH;
            return true;
        }
        if (hit_box(x, y, 436, 594, 148, 62)) {
            cfg->invert_mode = !cfg->invert_mode;
            *cfg_dirty = true;
            *out_refresh_mode = KD_REFRESH_FULL_FLASH;
            return true;
        }

        /* Bottom panel row 2 (y=664..712): Spacing / Auto-Flash cadence / Flash Once */
        if (hit_box(x, y, 16, 658, 184, 60)) {
            cfg->reader_line_spacing = (cfg->reader_line_spacing + 1) % 3;
            *cfg_dirty = true;
            paginate_chapter(rs, cfg, cur_off);
            *out_refresh_mode = KD_REFRESH_GRAY_PARTIAL;
            return true;
        }
        if (hit_box(x, y, 204, 658, 228, 60)) {
            cfg->reader_flash_every = config_next_reader_flash(cfg->reader_flash_every);
            rs->pages_since_flash = 0;
            *cfg_dirty = true;
            *out_refresh_mode = KD_REFRESH_GRAY_PARTIAL;
            return true;
        }
        if (hit_box(x, y, 436, 658, 148, 60)) {
            rs->toolbar_open = false;
            rs->pages_since_flash = 0;
            *out_refresh_mode = KD_REFRESH_FULL_FLASH;
            return true;
        }

        /* Bottom panel row 3 (y=726..778): Prev / Next chapter */
        if (hit_box(x, y, 16, 720, 152, 66)) {
            if (rs->cur_ch > 0) {
                load_chapter_idx(rs, rs->cur_ch - 1, 0, cfg);
                persist_progress(rs, cfg);
                *out_refresh_mode = KD_REFRESH_FULL_FLASH;
                return true;
            }
            return false;
        }
        if (hit_box(x, y, 432, 720, 152, 66)) {
            if (rs->cur_ch + 1 < book_chapter_count(rs->book)) {
                load_chapter_idx(rs, rs->cur_ch + 1, 0, cfg);
                persist_progress(rs, cfg);
                *out_refresh_mode = KD_REFRESH_FULL_FLASH;
                return true;
            }
            return false;
        }

        /* Tap middle area closes toolbar */
        rs->toolbar_open = false;
        *out_refresh_mode = KD_REFRESH_GRAY_PARTIAL;
        return true;
    }

    /* Normal reading gestures */
    if (ev->type == INPUT_SWIPE_LEFT) {
        *out_refresh_mode = turn_page(rs, +1, cfg);
        return true;
    }
    if (ev->type == INPUT_SWIPE_RIGHT) {
        *out_refresh_mode = turn_page(rs, -1, cfg);
        return true;
    }
    if (ev->type == INPUT_SWIPE_UP || ev->type == INPUT_SWIPE_DOWN) {
        rs->toolbar_open = true;
        *out_refresh_mode = KD_REFRESH_GRAY_PARTIAL;
        return true;
    }

    /* Long-press anywhere on text -> instant (< 0.5 ms) word lookup! */
    if (ev->type == INPUT_LONG_TAP) {
        int idx = find_word_at(rs, ev->x, ev->y);
        if (idx >= 0) {
            trigger_word_lookup(rs, idx);
            *out_refresh_mode = KD_REFRESH_GRAY_PARTIAL;
            return true;
        }
        rs->toolbar_open = true;
        *out_refresh_mode = KD_REFRESH_GRAY_PARTIAL;
        return true;
    }

    if (ev->type == INPUT_TAP) {
        /* Top strip opens toolbar */
        if (ev->y < 68) {
            rs->toolbar_open = true;
            *out_refresh_mode = KD_REFRESH_GRAY_PARTIAL;
            return true;
        }
        /* If Lookup Tap mode is enabled, tapping any word looks it up directly */
        if (rs->lookup_tap_mode && ev->x >= 48 && ev->x <= 552 && ev->y >= 68 && ev->y <= 740) {
            int idx = find_word_at(rs, ev->x, ev->y);
            if (idx >= 0) {
                trigger_word_lookup(rs, idx);
                *out_refresh_mode = KD_REFRESH_GRAY_PARTIAL;
                return true;
            }
        }
        /* Left 28% = prev page, Right 72% = next page */
        if (ev->x < 168) {
            *out_refresh_mode = turn_page(rs, -1, cfg);
        } else {
            *out_refresh_mode = turn_page(rs, +1, cfg);
        }
        return true;
    }

    return false;
}
