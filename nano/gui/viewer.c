/* viewer.c - pictures.
 *
 * A browser of thumbnails for the pictures in \WALL and the root, made
 * one per pass through the main loop so the window opens at once and
 * fills in; a double-click, or Enter, opens one large, with arrows and
 * the keyboard to step through the rest, and a button to make it the
 * background.  Files opens a picture here with a double-click too.
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
#define CARD        0x1A140D
#define CARD_LIT    0x2E2010

#define MAX_PICS    48
#define TH_W        176                  /* a thumbnail, in the screen's shape */
#define TH_H        99
#define CELL_W      196
#define CELL_H      146
#define COLS        3

static char paths[MAX_PICS][96];
static char names[MAX_PICS][40];
static uint32_t *thumbs[MAX_PICS];       /* TH_W x TH_H each, made lazily */
static int npics, sel = -1, pending;     /* pending: the next thumbnail to make */
static int browser_id = -1, viewer_id = -1;

static uint32_t *pic;                    /* the one shown large, screen-sized */
static int shown = -1;                   /* its index */

/* ------------------------------------------------------------ the files */
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
        if (!(f.attr & 0x10) && is_picture(f.name) && npics < MAX_PICS) {
            char lname[84];
            snprintf(paths[npics], 96, "%s\\%s", dir, f.name);
            if (sys_long_name(lname, sizeof lname) > 0) strncpy(names[npics], lname, 39);
            else strncpy(names[npics], f.name, 39);
            names[npics][39] = 0;
            npics++;
        }
    } while (sys_findnext(&f) == 0);
}

static void clear_thumbs(void)
{
    int i;
    for (i = 0; i < MAX_PICS; i++)
        if (thumbs[i]) { free(thumbs[i]); thumbs[i] = 0; }
}

/* a thumbnail from a screen-sized decode: the average of each block */
static uint32_t *shrink(const uint32_t *src)
{
    uint32_t *t = malloc(TH_W * TH_H * 4);
    int x, y;
    if (!t) return 0;
    for (y = 0; y < TH_H; y++)
        for (x = 0; x < TH_W; x++) {
            int sx0 = x * scr_w / TH_W, sx1 = (x + 1) * scr_w / TH_W;
            int sy0 = y * scr_h / TH_H, sy1 = (y + 1) * scr_h / TH_H;
            int r = 0, g = 0, b = 0, n = 0, sx, sy;
            for (sy = sy0; sy < sy1; sy += 2)
                for (sx = sx0; sx < sx1; sx += 2) {
                    uint32_t c = src[(size_t)sy * scr_w + sx];
                    r += (c >> 16) & 255; g += (c >> 8) & 255; b += c & 255; n++;
                }
            if (!n) n = 1;
            t[y * TH_W + x] = ((uint32_t)(r / n) << 16) | ((uint32_t)(g / n) << 8) | (uint32_t)(b / n);
        }
    return t;
}

/* from the main loop: one thumbnail per pass, and 1 when the browser
   should repaint */
int viewer_tick(void)
{
    uint32_t *full;
    if (browser_id < 0 || pending >= npics) return 0;
    full = wall_decode(paths[pending]);
    if (full) {
        thumbs[pending] = shrink(full);
        free(full);
    }
    pending++;
    return 1;
}

/* ------------------------------------------------------------ the browser */
static void browser_draw(struct window *w)
{
    int i, x0 = w->x + 14, y0 = w->y + 12;
    if (!npics) {
        text(F_NORMAL, x0, y0 + 8, "No pictures in \\WALL or the root.", TEXT_DIM);
        text(F_SMALL, x0, y0 + 36, "BMP and JPEG files are shown here.", TEXT_DIM);
        return;
    }
    for (i = 0; i < npics; i++) {
        int cx = x0 + (i % COLS) * CELL_W, cy = y0 + (i / COLS) * CELL_H;
        if (cy + CELL_H > w->y + w->h) break;
        if (i == sel) {
            round_fill(cx - 4, cy - 4, CELL_W - 12, CELL_H - 8, 6, CARD_LIT);
            glow(cx - 4, cy - 4, CELL_W - 12, CELL_H - 8, 6, AMBER, 3);
        }
        round_fill(cx, cy, TH_W + 4, TH_H + 4, 3, WELL);
        if (thumbs[i]) {
            int y, x;
            for (y = 0; y < TH_H; y++) {
                int py = cy + 2 + y;
                uint32_t *out = back + (size_t)py * scr_w;
                if (!clip_intersects(cx + 2, py, TH_W, 1)) continue;
                for (x = 0; x < TH_W; x++) {
                    int px = cx + 2 + x;
                    if (px >= 0 && px < scr_w && py >= 0 && py < scr_h) out[px] = thumbs[i][y * TH_W + x];
                }
            }
            damage(cx + 2, cy + 2, TH_W, TH_H);
        } else {
            text(F_SMALL, cx + TH_W / 2 - text_width(F_SMALL, i < pending ? "unreadable" : "loading") / 2,
                 cy + TH_H / 2 - 8, i < pending ? "unreadable" : "loading", TEXT_DIM);
        }
        round_frame(cx, cy, TH_W + 4, TH_H + 4, 3, i == sel ? AMBER : EDGE);
        text_clipped(F_SMALL, cx + 2, cy + TH_H + 12, TH_W, names[i], i == sel ? AMBER_HOT : TEXT);
    }
}

static void open_index(int i);

static int browser_event(struct window *w, struct event *e)
{
    (void)w;
    if (e->type == EV_MOUSE_DOWN) {
        int col = (e->a - 14) / CELL_W, row = (e->b - 12) / CELL_H, i;
        if (e->a < 14 || col >= COLS) return 1;
        i = row * COLS + col;
        if (i >= 0 && i < npics) {
            if (e->dbl && i == sel) open_index(i);
            else sel = i;
        }
        return 1;
    }
    if (e->type == EV_KEY) {
        int k = e->a & ~K_SHIFT;
        if (k == K_RIGHT && sel + 1 < npics) { sel++; return 1; }
        if (k == K_LEFT && sel > 0) { sel--; return 1; }
        if (k == K_DOWN && sel + COLS < npics) { sel += COLS; return 1; }
        if (k == K_UP && sel - COLS >= 0) { sel -= COLS; return 1; }
        if (k == K_ENTER && sel >= 0) { open_index(sel); return 1; }
    }
    return 0;
}

/* ------------------------------------------------------------ one large */
static void viewer_draw(struct window *w)
{
    int ax = w->x + 12, ay = w->y + 12, aw = w->w - 24, ah = w->h - 64;
    int dw, dh, dx, dy, y, pts[6];
    char b[120];

    round_fill(ax, ay, aw, ah, 4, WELL);
    if (pic) {
        dw = aw;
        dh = (int)((long)aw * scr_h / scr_w);
        if (dh > ah) { dh = ah; dw = (int)((long)ah * scr_w / scr_h); }
        dx = ax + (aw - dw) / 2;
        dy = ay + (ah - dh) / 2;
        for (y = 0; y < dh; y++) {
            int sy = (int)((long)y * scr_h / dh), x;
            const uint32_t *src = pic + (size_t)sy * scr_w;
            uint32_t *out = back + (size_t)(dy + y) * scr_w;
            if (dy + y < 0 || dy + y >= scr_h || !clip_intersects(dx, dy + y, dw, 1)) continue;
            for (x = 0; x < dw; x++) {
                int px = dx + x;
                if (px >= 0 && px < scr_w) out[px] = src[(long)x * scr_w / dw];
            }
        }
        damage(dx, dy, dw, dh);
    } else {
        text(F_NORMAL, ax + 16, ay + 16, "This file could not be read.", TEXT_DIM);
    }
    round_frame(ax, ay, aw, ah, 4, EDGE);

    /* the arrows, over the picture's edges */
    if (shown > 0) {
        round_fill_alpha(ax + 8, ay + ah / 2 - 22, 32, 44, 6, 0x000000, 140);
        pts[0] = ax + 28; pts[1] = ay + ah / 2 - 12; pts[2] = ax + 16; pts[3] = ay + ah / 2; pts[4] = ax + 28; pts[5] = ay + ah / 2 + 12;
        poly_fill(pts, 3, AMBER_HOT);
    }
    if (shown >= 0 && shown + 1 < npics) {
        round_fill_alpha(ax + aw - 40, ay + ah / 2 - 22, 32, 44, 6, 0x000000, 140);
        pts[0] = ax + aw - 28; pts[1] = ay + ah / 2 - 12; pts[2] = ax + aw - 16; pts[3] = ay + ah / 2; pts[4] = ax + aw - 28; pts[5] = ay + ah / 2 + 12;
        poly_fill(pts, 3, AMBER_HOT);
    }

    if (shown >= 0) snprintf(b, sizeof b, "%s   %d of %d", names[shown], shown + 1, npics);
    else snprintf(b, sizeof b, "%s", paths[0]);
    text(F_SMALL, w->x + 14, w->y + w->h - 38, b, TEXT_DIM);
    round_fill(w->x + w->w - 170, w->y + w->h - 44, 156, 30, 4, 0x2A2114);
    round_frame(w->x + w->w - 170, w->y + w->h - 44, 156, 30, 4, EDGE);
    text(F_SMALL, w->x + w->w - 170 + (156 - text_width(F_SMALL, "Use as background")) / 2,
         w->y + w->h - 44 + (30 - text_height(F_SMALL)) / 2, "Use as background", TEXT);
}

static void show_index(int i)
{
    uint32_t *p;
    if (i < 0 || i >= npics) return;
    p = wall_decode(paths[i]);
    if (pic) free(pic);
    pic = p;
    shown = i;
    sel = i;
}

static int viewer_event(struct window *w, struct event *e)
{
    int ah = w->h - 64;
    if (e->type == EV_MOUSE_DOWN) {
        if (e->b >= w->h - 44 && e->b < w->h - 14 && e->a >= w->w - 170 && e->a < w->w - 14 && pic && shown >= 0) {
            if (wall_load(paths[shown]) == 0) wall_save();
            return 1;
        }
        if (e->b >= 12 + ah / 2 - 22 && e->b < 12 + ah / 2 + 22) {
            if (e->a >= 20 && e->a < 52) show_index(shown - 1);
            else if (e->a >= w->w - 52 && e->a < w->w - 20) show_index(shown + 1);
        }
        return 1;
    }
    if (e->type == EV_KEY) {
        int k = e->a & ~K_SHIFT;
        if (k == K_LEFT || k == K_UP)    { show_index(shown - 1); return 1; }
        if (k == K_RIGHT || k == K_DOWN || k == 0x39) { show_index(shown + 1); return 1; }
    }
    return 0;
}

static void open_index(int i)
{
    show_index(i);
    if (viewer_id < 0) {
        int w = scr_w * 3 / 5, h = w * scr_h / scr_w + 64;
        if (w < 480) w = 480;
        viewer_id = win_open("Picture", w, h, viewer_draw, viewer_event);
    }
}

/* ------------------------------------------------------------ opening */
void viewer_closed(int id)
{
    if (id == viewer_id) viewer_id = -1;
    if (id == browser_id) browser_id = -1;
}

static void rescan(void)
{
    clear_thumbs();
    npics = 0;
    pending = 0;
    scan("\\WALL");
    scan("");
}

void app_viewer(void)
{
    int rows;
    if (browser_id >= 0) return;
    rescan();
    sel = npics ? 0 : -1;
    rows = (npics + COLS - 1) / COLS;
    if (rows < 1) rows = 1;
    if (rows > 4) rows = 4;
    browser_id = win_open("Pictures", COLS * CELL_W + 20, rows * CELL_H + 24, browser_draw, browser_event);
}

void app_viewer_open(const char *path)
{
    int i;
    if (!npics) rescan();
    for (i = 0; i < npics; i++)
        if (!strcmp(paths[i], path)) { open_index(i); return; }
    /* not in the folders we list: show it on its own */
    if (npics < MAX_PICS) {
        const char *slash = strrchr(path, '\\');
        strncpy(paths[npics], path, 95);
        strncpy(names[npics], slash ? slash + 1 : path, 39);
        npics++;
        open_index(npics - 1);
    }
}
