/* viewer.c - pictures.
 *
 * A browser: the folders and pictures of one directory as tiles, folders
 * with a folder icon, pictures as thumbnails made one per pass through
 * the main loop so the window opens at once and fills in.  Tap a folder
 * to go into it, the Up tile to come out; double-click a picture, or
 * Enter, to see it large, with arrows and the keyboard to step through
 * the rest and a button to make it the background.  Files opens a
 * picture here with a double-click too.  BMP, JPEG and PNG.
 */
#include <nanolibc.h>
#include "nano.h"
#include "draw.h"
#include "input.h"
#include "shell.h"
#include "guiart.h"

#define EDGE        0x4A3618
#define AMBER       0xF0A020
#define AMBER_HOT   0xFFC65A
#define TEXT        0xD8C8B0
#define TEXT_DIM    0x8A7C68
#define WELL        0x0A0806
#define CARD        0x1A140D
#define CARD_LIT    0x2E2010

#define MAX_ENTRIES 96
#define TH_W        176                  /* a thumbnail, in the screen's shape */
#define TH_H        99
#define CELL_W      196
#define CELL_H      146
#define COLS        4
#define HEAD        34                   /* the path line at the top */

struct entry { char path[96], name[40]; int is_dir; uint32_t *thumb; int tried; };
static struct entry entries[MAX_ENTRIES];
static int nentries, sel = -1, pending;
static char cur_dir[96] = "\\WALL";
static int browser_id = -1, viewer_id = -1, scroll_rows;

static uint32_t *pic;                    /* the one shown large, screen-sized */
static int shown = -1;

/* ------------------------------------------------------------ the files */
static int is_picture(const char *n)
{
    const char *d = strrchr(n, '.');
    return d && (!strcmp(d, ".BMP") || !strcmp(d, ".JPG") || !strcmp(d, ".JPEG") || !strcmp(d, ".PNG"));
}

static void clear_entries(void)
{
    int i;
    for (i = 0; i < nentries; i++)
        if (entries[i].thumb) { free(entries[i].thumb); entries[i].thumb = 0; }
    nentries = 0;
    pending = 0;
}

static void add_entry(const char *dir, const char *name, const char *longname, int is_dir)
{
    struct entry *e;
    if (nentries >= MAX_ENTRIES) return;
    e = &entries[nentries++];
    if (dir[0]) snprintf(e->path, sizeof e->path, "%s\\%s", dir, name);
    else snprintf(e->path, sizeof e->path, "\\%s", name);
    strncpy(e->name, longname ? longname : name, sizeof e->name - 1);
    e->name[sizeof e->name - 1] = 0;
    e->is_dir = is_dir;
    e->thumb = 0;
    e->tried = 0;
}

/* the folders first, then the pictures */
static void scan(const char *dir)
{
    struct dos_find f;
    char pat[96], lname[84];
    int pass;
    clear_entries();
    if (dir[0]) add_entry("", "..", "Up", 2);              /* 2: the way out */
    for (pass = 0; pass < 2; pass++) {
        snprintf(pat, sizeof pat, "%s\\*.*", dir);
        if (sys_findfirst(pat, &f) != 0) continue;
        do {
            int is_dir = (f.attr & 0x10) != 0;
            if (f.name[0] == '.') continue;
            if (pass == 0 && !is_dir) continue;
            if (pass == 1 && (is_dir || !is_picture(f.name))) continue;
            add_entry(dir, f.name, sys_long_name(lname, sizeof lname) > 0 ? lname : 0, is_dir);
        } while (sys_findnext(&f) == 0);
    }
    sel = nentries ? 0 : -1;
    scroll_rows = 0;
}

static void go(const char *dir)
{
    strncpy(cur_dir, dir, sizeof cur_dir - 1);
    cur_dir[sizeof cur_dir - 1] = 0;
    scan(cur_dir);
}

static void go_up(void)
{
    char *p = strrchr(cur_dir, '\\');
    if (p) *p = 0;
    else cur_dir[0] = 0;
    scan(cur_dir);
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
    if (browser_id < 0) return 0;
    while (pending < nentries && entries[pending].is_dir) pending++;
    if (pending >= nentries) return 0;
    full = wall_decode(entries[pending].path);
    if (full) {
        entries[pending].thumb = shrink(full);
        free(full);
    }
    entries[pending].tried = 1;
    pending++;
    return 1;
}

/* ------------------------------------------------------------ the browser */
static void browser_draw(struct window *w)
{
    int i, x0 = w->x + 14, y0 = w->y + HEAD + 8, rows = (w->h - HEAD - 8) / CELL_H;
    char b[120];

    vgradient(w->x, w->y, w->w, HEAD, 0x2A2114, 0x1A140D);
    fill(w->x, w->y + HEAD - 1, w->w, 1, EDGE);
    snprintf(b, sizeof b, "C:%s", cur_dir[0] ? cur_dir : "\\");
    text(F_BOLD, w->x + 14, w->y + (HEAD - text_height(F_BOLD)) / 2, b, AMBER_HOT);
    snprintf(b, sizeof b, "%d items", nentries - (cur_dir[0] ? 1 : 0));
    text(F_SMALL, w->x + w->w - 14 - text_width(F_SMALL, b), w->y + (HEAD - text_height(F_SMALL)) / 2, b, TEXT_DIM);

    if (!nentries) {
        text(F_NORMAL, x0, y0 + 8, "Nothing here that looks like a picture.", TEXT_DIM);
        return;
    }
    for (i = scroll_rows * COLS; i < nentries; i++) {
        int k = i - scroll_rows * COLS;
        int cx = x0 + (k % COLS) * CELL_W, cy = y0 + (k / COLS) * CELL_H;
        struct entry *e = &entries[i];
        if (k / COLS >= rows) break;
        if (i == sel) {
            round_fill(cx - 4, cy - 4, CELL_W - 12, CELL_H - 8, 6, CARD_LIT);
            glow(cx - 4, cy - 4, CELL_W - 12, CELL_H - 8, 6, AMBER, 3);
        }
        if (e->is_dir) {
            round_fill(cx, cy, TH_W + 4, TH_H + 4, 3, CARD);
            image_draw_scaled(&art_folder, cx + TH_W / 2 - 34, cy + TH_H / 2 - 34 + 2, 68, 68);
            if (e->is_dir == 2) {                                 /* the way up: an arrow on it */
                int pts[6] = { cx + TH_W / 2, cy + 14, cx + TH_W / 2 - 12, cy + 30, cx + TH_W / 2 + 12, cy + 30 };
                poly_fill(pts, 3, AMBER_HOT);
            }
        } else {
            round_fill(cx, cy, TH_W + 4, TH_H + 4, 3, WELL);
            if (e->thumb) {
                int y, x;
                for (y = 0; y < TH_H; y++) {
                    int py = cy + 2 + y;
                    uint32_t *out = back + (size_t)py * scr_w;
                    if (!clip_intersects(cx + 2, py, TH_W, 1)) continue;
                    for (x = 0; x < TH_W; x++) {
                        int px = cx + 2 + x;
                        if (px >= 0 && px < scr_w && py >= 0 && py < scr_h) out[px] = e->thumb[y * TH_W + x];
                    }
                }
                damage(cx + 2, cy + 2, TH_W, TH_H);
            } else {
                const char *note = e->tried ? "unreadable" : "loading";
                text(F_SMALL, cx + TH_W / 2 - text_width(F_SMALL, note) / 2, cy + TH_H / 2 - 8, note, TEXT_DIM);
            }
        }
        round_frame(cx, cy, TH_W + 4, TH_H + 4, 3, i == sel ? AMBER : EDGE);
        text_clipped(F_SMALL, cx + 2, cy + TH_H + 12, TH_W, e->name, i == sel ? AMBER_HOT : TEXT);
    }
}

static void open_index(int i);

static void activate(int i)
{
    struct entry *e = &entries[i];
    if (e->is_dir == 2) go_up();
    else if (e->is_dir) go(e->path);
    else open_index(i);
}

static int browser_event(struct window *w, struct event *e)
{
    int rows = (w->h - HEAD - 8) / CELL_H;
    if (e->type == EV_MOUSE_DOWN) {
        int col, row, i;
        if (e->b < HEAD) { if (cur_dir[0]) go_up(); return 1; }  /* the path line: up */
        col = (e->a - 14) / CELL_W;
        row = (e->b - HEAD - 8) / CELL_H + scroll_rows;
        if (e->a < 14 || col >= COLS) return 1;
        i = row * COLS + col;
        if (i >= 0 && i < nentries) {
            if (entries[i].is_dir) { sel = i; activate(i); }     /* one tap for a folder */
            else if (e->dbl && i == sel) open_index(i);
            else sel = i;
        }
        return 1;
    }
    if (e->type == EV_KEY) {
        int k = e->a & ~K_SHIFT;
        if (k == K_RIGHT && sel + 1 < nentries) sel++;
        else if (k == K_LEFT && sel > 0) sel--;
        else if (k == K_DOWN && sel + COLS < nentries) sel += COLS;
        else if (k == K_UP && sel - COLS >= 0) sel -= COLS;
        else if (k == K_ENTER && sel >= 0) { activate(sel); return 1; }
        else if (k == 0x0E && cur_dir[0]) { go_up(); return 1; }       /* Backspace */
        else return 0;
        if (sel / COLS < scroll_rows) scroll_rows = sel / COLS;
        if (sel / COLS >= scroll_rows + rows) scroll_rows = sel / COLS - rows + 1;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------ one large */
static int next_picture(int from, int step)
{
    int i = from + step;
    while (i >= 0 && i < nentries && entries[i].is_dir) i += step;
    return (i >= 0 && i < nentries) ? i : -1;
}

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

    if (next_picture(shown, -1) >= 0) {
        round_fill_alpha(ax + 8, ay + ah / 2 - 22, 32, 44, 6, 0x000000, 140);
        pts[0] = ax + 28; pts[1] = ay + ah / 2 - 12; pts[2] = ax + 16; pts[3] = ay + ah / 2; pts[4] = ax + 28; pts[5] = ay + ah / 2 + 12;
        poly_fill(pts, 3, AMBER_HOT);
    }
    if (next_picture(shown, 1) >= 0) {
        round_fill_alpha(ax + aw - 40, ay + ah / 2 - 22, 32, 44, 6, 0x000000, 140);
        pts[0] = ax + aw - 28; pts[1] = ay + ah / 2 - 12; pts[2] = ax + aw - 16; pts[3] = ay + ah / 2; pts[4] = ax + aw - 28; pts[5] = ay + ah / 2 + 12;
        poly_fill(pts, 3, AMBER_HOT);
    }

    if (shown >= 0) snprintf(b, sizeof b, "%s", entries[shown].path);
    else b[0] = 0;
    text(F_SMALL, w->x + 14, w->y + w->h - 38, b, TEXT_DIM);
    round_fill(w->x + w->w - 170, w->y + w->h - 44, 156, 30, 4, 0x2A2114);
    round_frame(w->x + w->w - 170, w->y + w->h - 44, 156, 30, 4, EDGE);
    text(F_SMALL, w->x + w->w - 170 + (156 - text_width(F_SMALL, "Use as background")) / 2,
         w->y + w->h - 44 + (30 - text_height(F_SMALL)) / 2, "Use as background", TEXT);
}

static void show_index(int i)
{
    uint32_t *p;
    if (i < 0 || i >= nentries || entries[i].is_dir) return;
    p = wall_decode(entries[i].path);
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
            if (wall_load(entries[shown].path) == 0) wall_save();
            return 1;
        }
        if (e->b >= 12 + ah / 2 - 22 && e->b < 12 + ah / 2 + 22) {
            if (e->a >= 20 && e->a < 52) show_index(next_picture(shown, -1));
            else if (e->a >= w->w - 52 && e->a < w->w - 20) show_index(next_picture(shown, 1));
        }
        return 1;
    }
    if (e->type == EV_KEY) {
        int k = e->a & ~K_SHIFT;
        if (k == K_LEFT || k == K_UP) { show_index(next_picture(shown, -1)); return 1; }
        if (k == K_RIGHT || k == K_DOWN || k == 0x39) { show_index(next_picture(shown, 1)); return 1; }
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

void app_viewer(void)
{
    struct dos_find f;
    if (browser_id >= 0) return;
    if (!cur_dir[0] || sys_findfirst(cur_dir, &f) != 0) strcpy(cur_dir, "\\WALL");
    if (sys_findfirst(cur_dir, &f) != 0) cur_dir[0] = 0;
    scan(cur_dir);
    browser_id = win_open("Pictures", COLS * CELL_W + 20, HEAD + 3 * CELL_H + 20, browser_draw, browser_event);
}

void app_viewer_open(const char *path)
{
    char dir[96];
    const char *slash = strrchr(path, '\\');
    int i;
    /* into the picture's folder, then the picture itself */
    if (slash && slash > path) { int n = (int)(slash - path); if (n > 95) n = 95; memcpy(dir, path, n); dir[n] = 0; }
    else dir[0] = 0;
    if (strcmp(dir, cur_dir) || !nentries) go(dir);
    for (i = 0; i < nentries; i++)
        if (!strcmp(entries[i].path, path)) { open_index(i); return; }
    if (nentries < MAX_ENTRIES) {
        add_entry(dir, slash ? slash + 1 : path, 0, 0);
        open_index(nentries - 1);
    }
}
