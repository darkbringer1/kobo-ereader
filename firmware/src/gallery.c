#include "kobodash.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static bool has_image_ext(const char *name) {
    if (!name || name[0] == '.') return false;
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    char ext[12];
    size_t i = 0;
    for (const char *p = dot + 1; *p && i + 1 < sizeof(ext); p++) {
        ext[i++] = (char)tolower((unsigned char)*p);
    }
    ext[i] = '\0';
    return strcmp(ext, "png") == 0 ||
           strcmp(ext, "jpg") == 0 ||
           strcmp(ext, "jpeg") == 0 ||
           strcmp(ext, "bmp") == 0 ||
           strcmp(ext, "gif") == 0;
}

static bool already_added(const Gallery *g, const char *path) {
    for (int i = 0; i < g->count; i++) {
        if (strcmp(g->paths[i], path) == 0) return true;
    }
    return false;
}

static void scan_dir(Gallery *g, const char *dir) {
    if (!dir || !dir[0] || g->count >= KD_MAX_GALLERY) return;
    DIR *dp = opendir(dir);
    if (!dp) return;

    struct dirent *de;
    while ((de = readdir(dp)) != NULL && g->count < KD_MAX_GALLERY) {
        if (!has_image_ext(de->d_name)) continue;
        char full[256];
        int n = snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
        if (n <= 0 || (size_t)n >= sizeof(full)) continue;

        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) continue;
        if (already_added(g, full)) continue;

        snprintf(g->paths[g->count], sizeof(g->paths[0]), "%s", full);
        g->count++;
    }
    closedir(dp);
}

static int cmp_path(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

void gallery_scan(Gallery *g, const Config *cfg) {
    char prev[256] = {0};
    if (g->count > 0 && g->index >= 0 && g->index < g->count) {
        snprintf(prev, sizeof(prev), "%s", g->paths[g->index]);
    }
    g->count = 0;

    if (cfg && cfg->screensaver_dir[0]) {
        scan_dir(g, cfg->screensaver_dir);
    }
    scan_dir(g, "/mnt/sd/screensavers");
    scan_dir(g, "/mnt/sd/images");
    scan_dir(g, "/mnt/sd");
    if (cfg && cfg->base_dir[0]) {
        char local_dir[300];
        snprintf(local_dir, sizeof(local_dir), "%s/screensavers", cfg->base_dir);
        scan_dir(g, local_dir);
    }

    if (g->count > 1) {
        qsort(g->paths, (size_t)g->count, sizeof(g->paths[0]), cmp_path);
    }

    g->index = 0;
    if (prev[0] && g->count > 0) {
        for (int i = 0; i < g->count; i++) {
            if (strcmp(g->paths[i], prev) == 0) {
                g->index = i;
                break;
            }
        }
    }
}

const char *gallery_current(const Gallery *g) {
    if (!g || g->count <= 0) return NULL;
    int idx = g->index;
    if (idx < 0 || idx >= g->count) idx = 0;
    return g->paths[idx];
}

void gallery_advance(Gallery *g, int delta) {
    if (!g || g->count <= 0) return;
    g->index = (g->index + delta) % g->count;
    if (g->index < 0) g->index += g->count;
}
