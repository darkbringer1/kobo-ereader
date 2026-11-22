#include "kobodash.h"
#include "fbink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fbfd = -1;
static FBInkConfig g_base_cfg;
static FBInkState g_state;
static bool g_has_ui_ot = false;
static bool g_has_clock_ot = false;
static FBInkOTConfig g_clock_ot;

static void try_load_fonts(const Config *cfg) {
    char p_reg[320], p_bold[320], p_clk[320];
    snprintf(p_reg,  sizeof(p_reg),  "%s/fonts/ui-regular.ttf", cfg->base_dir);
    snprintf(p_bold, sizeof(p_bold), "%s/fonts/ui-bold.ttf",    cfg->base_dir);
    snprintf(p_clk,  sizeof(p_clk),  "%s/fonts/clock.ttf",      cfg->base_dir);

    if (access(p_reg, R_OK) == 0) {
        if (fbink_add_ot_font(p_reg, FNT_REGULAR) == EXIT_SUCCESS) {
            g_has_ui_ot = true;
        }
    }
    if (access(p_bold, R_OK) == 0) {
        if (fbink_add_ot_font(p_bold, FNT_BOLD) == EXIT_SUCCESS) {
            if (!g_has_ui_ot) {
                (void)fbink_add_ot_font(p_bold, FNT_REGULAR);
                g_has_ui_ot = true;
            }
        }
    }

    memset(&g_clock_ot, 0, sizeof(g_clock_ot));
    if (access(p_clk, R_OK) == 0) {
        if (fbink_add_ot_font_v2(p_clk, FNT_REGULAR, &g_clock_ot) == EXIT_SUCCESS) {
            g_has_clock_ot = true;
        }
    }
}

const char *display_backend_version(void) {
    return fbink_version();
}

int display_init(const Config *cfg) {
    g_fbfd = fbink_open();
    if (g_fbfd < 0) return -1;

    memset(&g_base_cfg, 0, sizeof(g_base_cfg));
    g_base_cfg.is_quiet = true;
    g_base_cfg.no_viewport = false;
    g_base_cfg.fontname = SPLEEN;

    if (fbink_init(g_fbfd, &g_base_cfg) != EXIT_SUCCESS) {
        fbink_close(g_fbfd);
        g_fbfd = -1;
        return -1;
    }

    fbink_get_state(&g_base_cfg, &g_state);
    try_load_fonts(cfg);
    return 0;
}

void display_close(void) {
    if (g_has_clock_ot) {
        fbink_free_ot_fonts_v2(&g_clock_ot);
        g_has_clock_ot = false;
    }
    if (g_has_ui_ot) {
        fbink_free_ot_fonts();
        g_has_ui_ot = false;
    }
    if (g_fbfd >= 0) {
        fbink_close(g_fbfd);
        g_fbfd = -1;
    }
}

int display_blit_canvas(const Canvas *c, const KdRect *region) {
    if (g_fbfd < 0 || !c) return -1;

    FBInkConfig cfg = g_base_cfg;
    cfg.no_refresh = true;
    cfg.ignore_alpha = true;

    if (!region || (region->left == 0 && region->top == 0 &&
                    region->width >= KD_WIDTH && region->height >= KD_HEIGHT)) {
        return fbink_print_raw_data(g_fbfd,
                                    (unsigned char *)c->pixels,
                                    KD_WIDTH, KD_HEIGHT,
                                    (size_t)KD_WIDTH * KD_HEIGHT,
                                    0, 0, &cfg);
    }

    int x0 = region->left < 0 ? 0 : region->left;
    int y0 = region->top < 0 ? 0 : region->top;
    int w = region->width;
    int h = region->height;
    if (x0 + w > KD_WIDTH) w = KD_WIDTH - x0;
    if (y0 + h > KD_HEIGHT) h = KD_HEIGHT - y0;
    if (w <= 0 || h <= 0) return 0;

    unsigned char *sub = malloc((size_t)w * (size_t)h);
    if (!sub) return -1;
    for (int row = 0; row < h; row++) {
        memcpy(sub + (size_t)row * (size_t)w,
               &c->pixels[(y0 + row) * KD_WIDTH + x0],
               (size_t)w);
    }
    int rv = fbink_print_raw_data(g_fbfd, sub, w, h, (size_t)w * (size_t)h,
                                  (short int)x0, (short int)y0, &cfg);
    free(sub);
    return rv;
}

int display_text(const char *text, KdFont font, int size_px, KdAlign align,
                 int x, int y, int w, int h, uint8_t fg_gray, bool bgless, uint8_t bg_gray) {
    if (g_fbfd < 0 || !text || !text[0]) return 0;
    if (w <= 0 || h <= 0) return 0;

    fbink_set_fg_pen_gray(fg_gray, false, false);
    fbink_set_bg_pen_gray(bg_gray, false, false);

    FBInkConfig fcfg = g_base_cfg;
    fcfg.no_refresh = true;
    fcfg.is_bgless = bgless;
    fcfg.valign = CENTER;
    if (align == KD_ALIGN_CENTER) {
        fcfg.halign = CENTER;
        fcfg.is_centered = true;
    } else if (align == KD_ALIGN_RIGHT) {
        fcfg.halign = EDGE;
    } else {
        fcfg.halign = NONE;
    }

    int sw = (int)(g_state.view_width ? g_state.view_width : KD_WIDTH);
    int sh = (int)(g_state.view_height ? g_state.view_height : KD_HEIGHT);

    if (g_has_ui_ot || g_has_clock_ot) {
        FBInkOTConfig ot;
        if (font == KD_FONT_CLOCK && g_has_clock_ot) {
            ot = g_clock_ot;
            ot.style = FNT_REGULAR;
        } else {
            memset(&ot, 0, sizeof(ot));
            ot.style = (font == KD_FONT_BOLD || font == KD_FONT_CLOCK) ? FNT_BOLD : FNT_REGULAR;
        }

        int left = x < 0 ? 0 : x;
        int top = y < 0 ? 0 : y;
        int right = sw - (x + w);
        int bottom = sh - (y + h);
        if (right < 0) right = 0;
        if (bottom < 0) bottom = 0;
        if (left + right >= sw) return 0;
        if (top + bottom >= sh) return 0;

        ot.margins.left = (short int)left;
        ot.margins.right = (short int)right;
        ot.margins.top = (short int)top;
        ot.margins.bottom = (short int)bottom;
        ot.size_px = (unsigned short int)(size_px > 6 ? size_px : 12);
        ot.is_centered = (align == KD_ALIGN_CENTER);

        int rv = fbink_print_ot(g_fbfd, text, &ot, &fcfg, NULL);
        if (rv >= 0) return 0;
    }

    /* Fallback to built-in bitmap font if TTF unavailable */
    fcfg.hoffset = (short int)x;
    fcfg.voffset = (short int)y;
    fcfg.row = 0;
    fcfg.col = 0;
    return fbink_print(g_fbfd, text, &fcfg);
}

int display_image_file(const char *path, int x, int y, int max_w, int max_h,
                       bool dither, bool invert) {
    if (g_fbfd < 0 || !path || !path[0]) return -1;
    FBInkConfig cfg = g_base_cfg;
    cfg.no_refresh = true;
    cfg.ignore_alpha = true;
    cfg.sw_dithering = dither;
    cfg.is_inverted = invert;
    cfg.halign = CENTER;
    cfg.valign = CENTER;
    if (max_w == KD_WIDTH && max_h == KD_HEIGHT && x == 0 && y == 0) {
        /* Fit to screen while respecting aspect ratio */
        cfg.scaled_width = 0;
        cfg.scaled_height = -2;
    } else {
        cfg.scaled_width = (short int)max_w;
        cfg.scaled_height = (short int)max_h;
    }
    return fbink_print_image(g_fbfd, path, (short int)x, (short int)y, &cfg);
}

int display_refresh(const KdRect *region, KdRefreshMode mode) {
    if (g_fbfd < 0) return -1;

    /* NOTE: On Kobo Touch A (Mk. 3), ONLY WFM_DU and WFM_GC16 are reliable (never WFM_GL16/A2). */
    FBInkConfig rcfg = g_base_cfg;
    if (mode == KD_REFRESH_FAST_DU) {
        rcfg.wfm_mode = WFM_DU;
        rcfg.is_flashing = false;
    } else if (mode == KD_REFRESH_GRAY_PARTIAL) {
        rcfg.wfm_mode = WFM_GC16;
        rcfg.is_flashing = false;
    } else {
        rcfg.wfm_mode = WFM_GC16;
        rcfg.is_flashing = true;
    }

    if (!region || region->width <= 0 || region->height <= 0 ||
        (region->left == 0 && region->top == 0 &&
         region->width >= KD_WIDTH && region->height >= KD_HEIGHT)) {
        return fbink_refresh(g_fbfd, 0, 0, 0, 0, &rcfg);
    }

    uint32_t left = (uint32_t)(region->left < 0 ? 0 : region->left);
    uint32_t top = (uint32_t)(region->top < 0 ? 0 : region->top);
    uint32_t w = (uint32_t)region->width;
    uint32_t h = (uint32_t)region->height;
    if (left + w > KD_WIDTH) w = KD_WIDTH - left;
    if (top + h > KD_HEIGHT) h = KD_HEIGHT - top;
    return fbink_refresh(g_fbfd, top, left, w, h, &rcfg);
}

void display_wait_complete(void) {
    if (g_fbfd >= 0) {
        (void)fbink_wait_for_complete(g_fbfd, LAST_MARKER);
    }
}

void display_show_banner(const char *line1, const char *line2, bool invert) {
    static Canvas c;
    uint8_t bg = invert ? 0x10 : 0xFF;
    uint8_t fg = invert ? 0xFF : 0x00;
    uint8_t card = invert ? 0x22 : 0xF2;
    canvas_clear(&c, bg);
    canvas_fill_rrect(&c, 60, 310, 480, 170, 14, card);
    canvas_stroke_rrect(&c, 60, 310, 480, 170, 14, 2, fg);
    display_blit_canvas(&c, NULL);
    display_text(line1 ? line1 : "KoboDash", KD_FONT_BOLD, 30, KD_ALIGN_CENTER,
                 76, 340, 448, 52, fg, true, card);
    if (line2 && line2[0]) {
        display_text(line2, KD_FONT_REGULAR, 20, KD_ALIGN_CENTER,
                     76, 404, 448, 44, invert ? 0xCC : 0x44, true, card);
    }
    display_refresh(NULL, KD_REFRESH_FULL_FLASH);
    display_wait_complete();
}
