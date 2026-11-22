#include "kobodash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Host-side display mock for unit testing and PGM/JSON layout export */
static Canvas g_fb;
static FILE *g_text_log = NULL;
static int g_refresh_count = 0;
static KdRefreshMode g_last_mode = KD_REFRESH_FULL_FLASH;
static KdRect g_last_rect = {0, 0, KD_WIDTH, KD_HEIGHT};

int display_host_get_refresh_count(void) { return g_refresh_count; }
KdRefreshMode display_host_get_last_mode(void) { return g_last_mode; }
KdRect display_host_get_last_rect(void) { return g_last_rect; }

void display_host_begin_capture(const char *text_log_path) {
    if (g_text_log) fclose(g_text_log);
    g_text_log = text_log_path ? fopen(text_log_path, "w") : NULL;
    g_refresh_count = 0;
}

int display_host_save_pgm(const char *pgm_path) {
    FILE *fp = fopen(pgm_path, "wb");
    if (!fp) return -1;
    fprintf(fp, "P5\n%d %d\n255\n", KD_WIDTH, KD_HEIGHT);
    fwrite(g_fb.pixels, 1, sizeof(g_fb.pixels), fp);
    fclose(fp);
    if (g_text_log) {
        fclose(g_text_log);
        g_text_log = NULL;
    }
    return 0;
}

const char *display_backend_version(void) {
    return "host-mock";
}

int display_init(const Config *cfg) {
    (void)cfg;
    canvas_clear(&g_fb, 0xFF);
    g_refresh_count = 0;
    return 0;
}

void display_close(void) {
    if (g_text_log) {
        fclose(g_text_log);
        g_text_log = NULL;
    }
}

int display_blit_canvas(const Canvas *c, const KdRect *region) {
    if (!c) return -1;
    if (!region) {
        memcpy(g_fb.pixels, c->pixels, sizeof(g_fb.pixels));
        return 0;
    }
    for (int y = region->top; y < region->top + region->height && y < KD_HEIGHT; y++) {
        if (y < 0) continue;
        for (int x = region->left; x < region->left + region->width && x < KD_WIDTH; x++) {
            if (x < 0) continue;
            g_fb.pixels[y * KD_WIDTH + x] = c->pixels[y * KD_WIDTH + x];
        }
    }
    return 0;
}

int display_text(const char *text, KdFont font, int size_px, KdAlign align,
                 int x, int y, int w, int h, uint8_t fg_gray, bool bgless, uint8_t bg_gray) {
    (void)bgless;
    (void)bg_gray;
    if (g_text_log && text) {
        /* Tab-separated log for Python Pillow preview compositor */
        fprintf(g_text_log, "%d\t%d\t%d\t%d\t%d\t%d\t%d\t%u\t",
                (int)font, size_px, (int)align, x, y, w, h, (unsigned)fg_gray);
        for (const char *p = text; *p; p++) {
            if (*p == '\n') fputs("\\n", g_text_log);
            else if (*p == '\t') fputc(' ', g_text_log);
            else fputc(*p, g_text_log);
        }
        fputc('\n', g_text_log);
    }
    return 0;
}

int display_image_file(const char *path, int x, int y, int max_w, int max_h,
                       bool dither, bool invert) {
    (void)path; (void)x; (void)y; (void)max_w; (void)max_h; (void)dither; (void)invert;
    return -1;
}

int display_refresh(const KdRect *region, KdRefreshMode mode) {
    g_refresh_count++;
    g_last_mode = mode;
    if (region) g_last_rect = *region;
    else g_last_rect = (KdRect){0, 0, KD_WIDTH, KD_HEIGHT};
    return 0;
}

void display_wait_complete(void) {}

void display_show_banner(const char *line1, const char *line2, bool invert) {
    (void)line1; (void)line2; (void)invert;
}
