/* viewer.c - pictures.
 *
 * Shows a BMP or JPEG scaled to fit its window, with a button to make it
 * the background.  Opened from Files with a double-click on a picture,
 * or from the menu, which offers the pictures in \WALL and the root.
 */
#include <nanolibc.h>
#include "nano.h"
#include "draw.h"
#include "input.h"
#include "shell.h"

#define EDGE        0x4A3618
#define AMBER       0xF0A020
#define AMBER_HOT   0xFFC65A
#define TEXT        0xD8C8B0
#define TEXT_DIM    0x8A7C68
#define WELL        0x0A0806

static int win_id = -1, list_id = -1;
static uint32_t *pic;                   /* scr_w x scr_h, as the wallpaper code makes it */
static char pic_path[96];
static int pic_ok;

/* ------------------------------------------------------------ the picture */
static void viewer_draw(struct window *w)
{
    int ax = w->x + 12, ay = w->y + 12, aw = w->w - 24, ah = w->h - 64;
    int dw, dh, dx, dy, y;
    char b[120];

    round_fill(ax, ay, aw, ah, 4, WELL);
    if (pic_ok && pic) {
        /* fit, keeping the screen's shape, which the decoded picture has */
        dw = aw;
        dh = (int)((long)aw * scr_h / scr_w);
        if (dh > ah) { dh = ah; dw = (int)((long)ah * scr_w / scr_h); }
        dx = ax + (aw - dw) / 2;
        dy = ay + (ah - dh) / 2;
        for (y = 0; y < dh; y++) {
            int sy = (int)((long)y * scr_h / dh), x;
            const uint32_t *src = pic + (size_t)sy * scr_w;
            uint32_t *out = back + (size_t)(dy + y) * scr_w;
            if (dy + y < 0 || dy + y >= scr_h) continue;
            if (!clip_intersects(dx, dy + y, dw, 1)) continue;
            for (x = 0; x < dw; x++) {
                int px = dx + x;
                if (px >= 0 && px < scr_w) out[px] = src[(long)x * scr_w / dw];
            }
        }
        damage(dx, dy, dw, dh);
    } else {
        text(F_NORMAL, ax + 16, ay + 16, "Nothing to show: this file could not be read.", TEXT_DIM);
    }
    round_frame(ax, ay, aw, ah, 4, EDGE);

    snprintf(b, sizeof b, "%s", pic_path);
    text(F_SMALL, w->x + 14, w->y + w->h - 38, b, TEXT_DIM);
    round_fill(w->x + w->w - 170, w->y + w->h - 44, 156, 30, 4, 0x2A2114);
    round_frame(w->x + w->w - 170, w->y + w->h - 44, 156, 30, 4, EDGE);
    text(F_SMALL, w->x + w->w - 170 + (156 - text_width(F_SMALL, "Use as background")) / 2,
         w->y + w->h - 44 + (30 - text_height(F_SMALL)) / 2, "Use as background", TEXT);
}

static int viewer_event(struct window *w, struct event *e)
{
    if (e->type != EV_MOUSE_DOWN) return 0;
    if (e->b >= w->h - 44 && e->b < w->h - 14 && e->a >= w->w - 170 && e->a < w->w - 14 && pic_ok) {
        if (wall_load(pic_path) == 0) wall_save();
        return 1;
    }
    return 1;
}

void viewer_closed(int id)
{
    if (id == win_id) win_id = -1;
    if (id == list_id) list_id = -1;
}

void app_viewer_open(const char *path)
{
    uint32_t *p = wall_decode(path);
    if (pic) free(pic);
    pic = p;
    pic_ok = p != 0;
    strncpy(pic_path, path, sizeof pic_path - 1);
    if (win_id < 0) {
        int w = scr_w * 3 / 5, h = w * scr_h / scr_w + 64;
        if (w < 480) w = 480;
        win_id = win_open("Pictures", w, h, viewer_draw, viewer_event);
    }
}

/* ------------------------------------------------------------ the list */
#define MAX_PICS 64
static char pics[MAX_PICS][96];
static int npics, list_sel = -1;

static int is_picture(const char *n)
{
    const char *d = strrchr(n, '.');
    return d && (!strcmp(d, ".BMP") || !strcmp(d, ".JPG") || !strcmp(d, ".JPEG"));
}

static void scan(const char *dir)
{
    struct dos_find f;
    char pat[96];
    snprintf(pat, sizeof pat, "%s\\*.*", dir);
    if (sys_findfirst(pat, &f) != 0) return;
    do {
        if (!(f.attr & 0x10) && is_picture(f.name) && npics < MAX_PICS)
            snprintf(pics[npics++], 96, "%s\\%s", dir, f.name);
    } while (sys_findnext(&f) == 0);
}

static void list_draw(struct window *w)
{
    int i, y = w->y + 12;
    if (!npics) {
        text(F_NORMAL, w->x + 16, y, "No pictures in \\WALL or the root.", TEXT_DIM);
        text(F_SMALL, w->x + 16, y + 28, "BMP and JPEG files are shown; double-click one.", TEXT_DIM);
        return;
    }
    for (i = 0; i < npics && y < w->y + w->h - 24; i++, y += 24) {
        if (i == list_sel) fill(w->x + 8, y - 2, w->w - 16, 24, 0x2E2010);
        text(F_NORMAL, w->x + 20, y, pics[i], i == list_sel ? AMBER_HOT : TEXT);
    }
}

static int list_event(struct window *w, struct event *e)
{
    (void)w;
    if (e->type == EV_MOUSE_DOWN) {
        int idx = (e->b - 12 + 2) / 24;
        if (idx >= 0 && idx < npics) {
            if (e->dbl && idx == list_sel) app_viewer_open(pics[idx]);
            else list_sel = idx;
        }
        return 1;
    }
    if (e->type == EV_KEY) {
        if (e->a == K_DOWN && list_sel + 1 < npics) { list_sel++; return 1; }
        if (e->a == K_UP && list_sel > 0) { list_sel--; return 1; }
        if (e->a == K_ENTER && list_sel >= 0) { app_viewer_open(pics[list_sel]); return 1; }
    }
    return 0;
}

void app_viewer(void)
{
    if (list_id >= 0) return;
    npics = 0;
    list_sel = -1;
    scan("\\WALL");
    scan("");
    list_id = win_open("Pictures", 460, 24 * (npics > 4 ? npics : 4) + 40, list_draw, list_event);
}
