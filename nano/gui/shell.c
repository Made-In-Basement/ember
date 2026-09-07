/* shell.c - the desktop: icons, windows, the top bar and the crystal.
 *
 * Amber on near-black.  Windows are given depth the way a real object has
 * it: a lit top edge, a graded face, a dark underside and a shadow that
 * falls away from the light.  The whole frame is composed off-screen and
 * only what changed is pushed to the display.
 */
#include <nanolibc.h>
#include "nano.h"
#include "draw.h"
#include "gpu.h"
#include "input.h"
#include "touch.h"
#include "power.h"
#include "shell.h"
#include "guiart.h"

/* ---- the palette everything is built from ---- */
#define BG_TOP      0x0B0806
#define BG_BOTTOM   0x16110A
#define GRID        0x1C150E
#define PANEL       0x1A140D
#define PANEL_LIT   0x2A2114
#define PANEL_DARK  0x0E0A06
#define EDGE        0x3A2C18
#define EDGE_LIT    0x574020
#define AMBER       0xF0A020
#define AMBER_DIM   0x8A5E16
#define AMBER_HOT   0xFFC65A
#define TEXT        0xD8C8B0
#define TEXT_DIM    0x8A7C68

#define BAR_H       36
#define TITLE_H     30

static struct window windows[MAX_WINDOWS];
static int window_count;
static int z_order[MAX_WINDOWS];
static int focused = -1;

static int drag_win = -1, drag_dx, drag_dy;
static int resize_win = -1;
#define drag_active (drag_win >= 0)
static int quit_requested;
struct shell_stats shell_stats;
static char start_arg[24];               /* how we were started, to start again the same */

/* Run a DOS program: the kernel's shell is given the lines to run once
   the desktop has ended - into the program's folder, the program, back
   to the root, and the desktop again - and the desktop ends. */
void shell_launch(const char *path)
{
    char lines[400], dir[128], name[64];
    const char *slash;
    int n;
    if (path[1] == ':') path += 2;                  /* the shell knows its drive */
    slash = strrchr(path, '\\');
    if (slash) {
        n = (int)(slash - path);
        if (n == 0) n = 1;                          /* the root */
        if (n >= (int)sizeof dir) n = sizeof dir - 1;
        memcpy(dir, path, n);
        dir[n] = 0;
        strncpy(name, slash + 1, sizeof name - 1);
    } else {
        strcpy(dir, "\\");
        strncpy(name, path, sizeof name - 1);
    }
    name[sizeof name - 1] = 0;
    {
        char *dot = strrchr(name, '.');             /* typed as a command, without it */
        if (dot && (!strcmp(dot, ".COM") || !strcmp(dot, ".EXE") ||
                    !strcmp(dot, ".N32") || !strcmp(dot, ".BAT")))
            *dot = 0;
    }
    snprintf(lines, sizeof lines, "CD %s\r\n%s\r\nCD \\\r\nEMBER %s\r\n", dir, name, start_arg);
    sys_logf("launch: %s in %s", name, dir);
    sys_run_after(lines);
    quit_requested = 1;
}

/* can the file browser start this? */
int shell_runnable(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!strcmp(dot, ".COM") || !strcmp(dot, ".EXE") ||
                   !strcmp(dot, ".N32") || !strcmp(dot, ".BAT"));
}
static int want_width = 1920, want_height = 1200;

/* ---------------------------------------------------------------- windows */
struct window *win_at(int id) { return &windows[id]; }
int win_count(void) { return window_count; }
int win_focused(void) { return focused; }
void shell_quit(void) { quit_requested = 1; }

int win_open(const char *title, int w, int h, void (*draw)(struct window *),
             int (*event)(struct window *, struct event *))
{
    struct window *win;
    int id;
    for (id = 0; id < MAX_WINDOWS && windows[id].open; id++) ;
    if (id >= MAX_WINDOWS) return -1;
    win = &windows[id];
    memset(win, 0, sizeof *win);
    strncpy(win->title, title, sizeof win->title - 1);
    win->w = w;
    win->h = h;
    win->x = 120 + (id * 34) % 260;
    win->y = BAR_H + 60 + (id * 28) % 160;
    win->open = 1;
    win->draw = draw;
    win->event = event;
    z_order[window_count] = id;
    window_count++;
    focused = id;
    damage_all();
    sys_logf("window: opened %d (%s), %d open", id, title, window_count);
    return id;
}

void win_close(int id)
{
    int i, j;
    sys_logf("window: closing %d (%s), %d open", id, windows[id].title, window_count);
    music_closed(id);
    monitor_closed(id);
    write_closed(id);
    viewer_closed(id);
    clock_closed(id);
    calendar_closed(id);
    notes_closed(id);
    paint_closed(id);
    windows[id].open = 0;
    for (i = 0, j = 0; i < window_count; i++)
        if (z_order[i] != id) z_order[j++] = z_order[i];
    window_count--;
    focused = window_count ? z_order[window_count - 1] : -1;
    damage_all();
}

static void raise_window(int id)
{
    int i, j;
    for (i = 0, j = 0; i < window_count; i++)
        if (z_order[i] != id) z_order[j++] = z_order[i];
    z_order[window_count - 1] = id;
    focused = id;
}

static int window_hit(int x, int y)
{
    int i;
    for (i = window_count - 1; i >= 0; i--) {
        struct window *w = &windows[z_order[i]];
        if (w->minimized) continue;
        if (x >= w->x - 2 && x < w->x + w->w + 2 &&
            y >= w->y - TITLE_H && y < w->y + w->h + 2)
            return z_order[i];
    }
    return -1;
}

/* ---------------------------------------------------------------- chrome */
static void draw_window(struct window *w, int is_focused, int with_backdrop)
{
    int tx = w->x, ty = w->y - TITLE_H;
    int tw = w->w, th = TITLE_H + w->h;

    /* The shadow and the halo are translucent, so they may only be laid
       down over freshly painted desktop; on a light repaint the window
       redraws its own opaque parts and leaves them alone. */
    if (with_backdrop) {
        shadow(tx + 3, ty + 5, tw, th, 4, 9);
        round_fill(tx - 2, ty - 2, tw + 4, th + 4, 5,
                   is_focused ? 0x4A3618 : 0x2A2114);
        if (is_focused)
            glow(tx - 2, ty - 2, tw + 4, th + 4, 5, AMBER, 4);
    }

    /* the title bar */
    vgradient(tx, ty, tw, TITLE_H,
              is_focused ? 0x3A2A12 : PANEL_LIT,
              is_focused ? 0x21180C : PANEL);
    fill(tx, ty, tw, 1, is_focused ? 0x6A4E20 : 0x3A2E1E);   /* the lit lip */
    fill(tx, ty + TITLE_H - 1, tw, 1, PANEL_DARK);
    text(F_BOLD, tx + 14, ty + (TITLE_H - text_height(F_BOLD)) / 2,
         w->title, is_focused ? AMBER_HOT : TEXT_DIM);

    /* the controls: close, maximize, minimize */
    {
        int cxp = tx + tw - 20, cyp = ty + TITLE_H / 2, i;
        uint32_t c = is_focused ? AMBER : TEXT_DIM;
        for (i = -4; i <= 4; i++) {
            pixel_blend(cxp + i, cyp + i, c, 255);
            pixel_blend(cxp + i, cyp - i, c, 255);
            pixel_blend(cxp + i + 1, cyp + i, c, 90);
            pixel_blend(cxp + i + 1, cyp - i, c, 90);
        }
        if (!w->fixed) {
            round_frame(cxp - 34, cyp - 5, 10, 10, 1, c);            /* maximize: a box */
            if (w->maxed) round_frame(cxp - 31, cyp - 8, 10, 10, 1, c);
        }
        fill(cxp - 63, cyp + 3, 10, 2, c);                           /* minimize: a line */
    }

    /* the body: a face that catches light at the top and falls away */
    vgradient(tx, w->y, tw, w->h, 0x1F1810, PANEL_DARK);
    fill(tx, w->y, tw, 1, 0x33271A);

    if (w->draw) {
        int ox, oy, ow, oh;
        clip_get(&ox, &oy, &ow, &oh);
        clip_shrink(w->x, w->y, w->w, w->h);
        w->draw(w);
        clip_set(ox, oy, ow, oh);
    }
    if (!w->fixed) {                                                 /* the grip */
        int gx = w->x + w->w - 4, gy = w->y + w->h - 4, i;
        for (i = 0; i < 3; i++)
            line(gx - 4 - i * 4, gy, gx, gy - 4 - i * 4, 1, is_focused ? AMBER_DIM : EDGE_LIT);
    }
}

/* ---------------------------------------------------------------- icons */
struct desk_icon { const char *label; const struct image *art; };
static const struct desk_icon icons[] = {
    { "Files",      &art_folder },
    { "Music",      &art_music },
    { "Prompt",     &art_terminal },
    { "Calculator", &art_calc },
    { "Write",      &art_note },
    { "Doom",       &art_chip },
    { "About",      &art_info },
};
#define ICON_COUNT 7
/* what each icon does */
static const int icon_menu[ICON_COUNT] = { A_FILES, A_MUSIC, A_PROMPT, A_CALC, A_WRITE, A_DOOM, A_ABOUT };
#define ICON_W     96
#define ICON_H     100
#define ICON_X     28
#define ICON_Y     (BAR_H + 24)
static int icon_sel = -1;

static void draw_icons(void)
{
    int i;
    if (!clip_intersects(ICON_X - 8, ICON_Y - 8, ICON_W + 8, ICON_COUNT * ICON_H + 8))
        return;                                 /* the column is not in the region */
    for (i = 0; i < ICON_COUNT; i++) {
        int x = ICON_X, y = ICON_Y + i * ICON_H;
        int lit = (i == icon_sel);
        int tw = text_width(F_SMALL, icons[i].label);
        const struct image *art = icons[i].art;
        int ax = x + (ICON_W - 12) / 2 - art->w / 2;
        if (lit) {
            round_fill_alpha(x - 6, y - 6, ICON_W, ICON_H - 8, 6, AMBER, 30);
            round_frame_alpha(x - 6, y - 6, ICON_W, ICON_H - 8, 6, AMBER_DIM, 170);
            image_draw_tinted(art, ax, y, 0xFFFFFF, 60);
        } else {
            image_draw(art, ax, y);
        }
        text(F_SMALL, x + (ICON_W - 12) / 2 - tw / 2, y + art->h + 6,
             icons[i].label, lit ? AMBER_HOT : TEXT);
    }
}

static int icon_hit(int x, int y)
{
    int i;
    for (i = 0; i < ICON_COUNT; i++) {
        int ix = ICON_X - 6, iy = ICON_Y + i * ICON_H - 4;
        if (x >= ix && x < ix + ICON_W && y >= iy && y < iy + ICON_H - 12)
            return i;
    }
    return -1;
}

/* ---------------------------------------------------------------- desktop */
static void draw_desktop(void)
{
    wall_draw();
    draw_icons();
}

static void read_clock(char *out, int size)
{
    unsigned h, m;
    outb(0x70, 0x04); h = inb(0x71);
    outb(0x70, 0x02); m = inb(0x71);
    h = (h >> 4) * 10 + (h & 0x0F);             /* the clock counts in BCD */
    m = (m >> 4) * 10 + (m & 0x0F);
    snprintf(out, size, "%02u:%02u", h % 24, m % 60);
}

static void draw_bar(void)
{
    int i, bx = 18;
    char clk[16];
    vgradient(0, 0, scr_w, BAR_H, 0x1E1710, 0x0C0906);
    fill(0, BAR_H - 1, scr_w, 1, AMBER_DIM);
    fill(0, 0, scr_w, 1, 0x3A2E1E);

    for (i = 0; i < window_count; i++) {
        struct window *w = &windows[z_order[i]];
        int is_focused = z_order[i] == focused;
        int bw = 160;
        if (bx + bw > crystal_x - 90) break;
        if (is_focused) {
            round_fill(bx, 5, bw, BAR_H - 11, 3, 0x2A1F10);
            fill(bx, 5, 3, BAR_H - 11, AMBER);
        }
        text_clipped(F_SMALL, bx + 12, (BAR_H - text_height(F_SMALL)) / 2,
                     bw - 20, w->title, is_focused ? AMBER_HOT : w->minimized ? 0x5A4E40 : TEXT_DIM);
        bx += bw + 8;
    }

    read_clock(clk, sizeof clk);
    text(F_BOLD, scr_w - 24 - text_width(F_BOLD, clk),
         (BAR_H - text_height(F_BOLD)) / 2, clk, AMBER);

    /* the battery, when the machine has one it will show */
    if (power_known() && power_percent() >= 0) {
        int pct = power_percent(), bx0 = scr_w - 24 - text_width(F_BOLD, clk) - 118;
        int by = BAR_H / 2 - 6, lit = 26 * pct / 100;
        uint32_t c = pct <= 15 ? 0xF0602A : pct <= 40 ? AMBER : 0x9BD27A;
        char pb[8];
        round_frame(bx0 + 40, by, 30, 13, 2, TEXT_DIM);
        fill(bx0 + 70, by + 3, 3, 7, TEXT_DIM);                 /* the nub */
        if (lit > 0) fill(bx0 + 42, by + 2, lit, 9, c);
        if (power_charging() == 1) {                            /* a bolt: two strokes */
            fill(bx0 + 53, by + 2, 2, 5, 0x1A140D);
            fill(bx0 + 55, by + 6, 2, 5, 0x1A140D);
        }
        snprintf(pb, sizeof pb, "%d%%", pct);
        text(F_SMALL, bx0 + 36 - text_width(F_SMALL, pb), (BAR_H - text_height(F_SMALL)) / 2, pb, TEXT_DIM);
    }
}

#define CUR_W 13
#define CUR_H 20
static uint32_t cursor_under[CUR_W * CUR_H];
static int cursor_saved_x = -1, cursor_saved_y;

static void cursor_lift(void)
{
    int row, col;
    if (cursor_saved_x < 0) return;
    for (row = 0; row < CUR_H; row++) {
        int py = cursor_saved_y + row;
        if (py < 0 || py >= scr_h) continue;
        for (col = 0; col < CUR_W; col++) {
            int px = cursor_saved_x + col;
            if (px < 0 || px >= scr_w) continue;
            back[(size_t)py * scr_w + px] = cursor_under[row * CUR_W + col];
        }
    }
    damage(cursor_saved_x, cursor_saved_y, CUR_W, CUR_H);
    cursor_saved_x = -1;
}

static void cursor_save(int x, int y)
{
    int row, col;
    for (row = 0; row < CUR_H; row++) {
        int py = y + row;
        for (col = 0; col < CUR_W; col++) {
            int px = x + col;
            cursor_under[row * CUR_W + col] =
                (px >= 0 && px < scr_w && py >= 0 && py < scr_h)
                    ? back[(size_t)py * scr_w + px] : 0;
        }
    }
    cursor_saved_x = x;
    cursor_saved_y = y;
}

static const char *cursor_shape[] = {
    "X............", "XX...........", "XoX..........", "XooX.........",
    "XoooX........", "XooooX.......", "XoooooX......", "XooooooX.....",
    "XoooooooX....", "XooooooooX...", "XoooooooooX..", "XooooooXXXXX.",
    "XoooXooX.....", "XooX.XooX....", "XoX...XooX...", "XX....XooX...",
    "X......XooX..", ".......XooX..", "........XX...", 0
};

/* the software pointer: blended into the back buffer, lifted before a repaint */
static void draw_cursor(int x, int y)
{
    int row, col;
    for (row = 0; cursor_shape[row]; row++)
        for (col = 0; cursor_shape[row][col]; col++) {
            char c = cursor_shape[row][col];
            if (c == 'X') pixel_blend(x + col, y + row, 0x000000, 210);
            else if (c == 'o') pixel_blend(x + col, y + row, AMBER_HOT, 255);
        }
    damage(x, y, CUR_W, CUR_H);
}

/* the same pointer as a sprite for the display engine, twice the size: the
   panel is dense, and a sprite costs nothing to draw however large */
#define SPRITE_SCALE 2
static void cursor_sprite(void)
{
    static uint32_t px[CUR_W * SPRITE_SCALE * CUR_H * SPRITE_SCALE];
    int row, col, sx, sy, w = CUR_W * SPRITE_SCALE;
    for (row = 0; cursor_shape[row]; row++)
        for (col = 0; cursor_shape[row][col]; col++) {
            char c = cursor_shape[row][col];
            uint32_t v = c == 'X' ? 0xD2000000u : c == 'o' ? 0xFF000000u | AMBER_HOT : 0;
            for (sy = 0; sy < SPRITE_SCALE; sy++)
                for (sx = 0; sx < SPRITE_SCALE; sx++)
                    px[(row * SPRITE_SCALE + sy) * w + col * SPRITE_SCALE + sx] = v;
        }
    gpu_cursor_image(px, w, CUR_H * SPRITE_SCALE, 0, 0);
    gpu_cursor_move(mouse_x, mouse_y);
    gpu_cursor_show(1);
}

/* a window's whole footprint: frame, title, shadow and halo */
static int window_shows(struct window *w)
{
    return !w->minimized && clip_intersects(w->x - 8, w->y - TITLE_H - 8, w->w + 32, w->h + TITLE_H + 40);
}

/* the frame's controls: minimize, maximize and close, right to left, and
   a grip at the bottom-right corner to resize by */
static void set_maximized(struct window *w, int on)
{
    if (w->fixed) return;
    if (on && !w->maxed) {
        w->sx = w->x; w->sy = w->y; w->sw = w->w; w->sh = w->h;
        w->x = 6;
        w->y = BAR_H + TITLE_H + 4;
        w->w = scr_w - 12;
        w->h = scr_h - BAR_H - TITLE_H - 10;
        w->maxed = 1;
    } else if (!on && w->maxed) {
        w->x = w->sx; w->y = w->sy; w->w = w->sw; w->h = w->sh;
        w->maxed = 0;
    }
    damage_all();
}

static void set_minimized(int id, int on)
{
    windows[id].minimized = on;
    if (on && focused == id) {
        int i;
        focused = -1;
        for (i = window_count - 1; i >= 0; i--)
            if (!windows[z_order[i]].minimized) { focused = z_order[i]; break; }
    }
    damage_all();
}

static void draw_all(void)
{
    int i, cx, cy, cw, ch;
    draw_desktop();
    for (i = 0; i < window_count; i++)
        if (window_shows(&windows[z_order[i]]))
            draw_window(&windows[z_order[i]], z_order[i] == focused, 1);
    if (clip_intersects(0, 0, scr_w, BAR_H))
        draw_bar();
    crystal_rect(&cx, &cy, &cw, &ch);
    if (clip_intersects(cx, cy, cw, ch))
        crystal_draw();
    if (osk_visible) {
        osk_rect(&cx, &cy, &cw, &ch);
        if (clip_intersects(cx, cy, cw, ch))
            osk_draw();
    }
    popup_draw();
}

/* ---------------------------------------------------------------- events */
void shell_run_menu(int action)
{
    switch (action) {
    case A_FILES:    app_files(); break;
    case A_MUSIC:    app_music(); break;
    case A_PROMPT:   app_prompt(); break;
    case A_CALC:     app_calc(); break;
    case A_WRITE:    app_write(); break;
    case A_DOOM:     app_doom(); break;
    case A_MONITOR:  app_monitor(); break;
    case A_KEYBOARD: osk_toggle(); break;       /* the caller repaints everything */
    case A_HELP:     app_help(); break;
    case A_ABOUT:    app_about(); break;
    case A_EXIT:     quit_requested = 1; break;
    case A_VIEWER:   app_viewer(); break;
    case A_CLOCK:    app_clock(); break;
    case A_CALENDAR: app_calendar(); break;
    case A_NOTES:    app_notes(); break;
    case A_SHOT:     app_screenshot(); break;
    case A_PAINT:    app_paint(); break;
    default: break;
    }
}

static void wall_chosen(int item)
{
    if (item == wall_choice_count()) {
        app_pictures();                         /* a picture off the disk */
    } else {
        wall_set(item);
        wall_save();
    }
}

static void desktop_menu(int x, int y)
{
    static const char *items[8];
    static int ticks[8];
    int n = wall_choice_count(), i;
    for (i = 0; i < n; i++) {
        items[i] = wall_name(i);
        ticks[i] = (wall_kind == i);
    }
    items[n] = wall_name(WALL_PICTURE);
    ticks[n] = (wall_kind == WALL_PICTURE);
    popup_open(x, y, items, ticks, n + 1, wall_chosen);
}

/* How much has to be repainted: nothing, the windows, or all of it.  A
   mouse move on its own needs none of it -- only the pointer moves -- and
   that is the difference between a smooth pointer and a crawling one. */
enum { REDRAW_NONE, REDRAW_WINDOWS, REDRAW_RECT, REDRAW_ALL };
static int rect_x0, rect_y0, rect_x1, rect_y1;  /* what REDRAW_RECT covers */
static int redraw_level;

static void need(int level)
{
    if (level > redraw_level) redraw_level = level;
}

/* everything within one region: a menu highlight, the gem's glow */
static void need_rect(int x, int y, int w, int h)
{
    if (redraw_level == REDRAW_RECT) {
        if (x < rect_x0) rect_x0 = x;
        if (y < rect_y0) rect_y0 = y;
        if (x + w > rect_x1) rect_x1 = x + w;
        if (y + h > rect_y1) rect_y1 = y + h;
    } else if (redraw_level < REDRAW_RECT) {
        rect_x0 = x; rect_y0 = y; rect_x1 = x + w; rect_y1 = y + h;
        redraw_level = REDRAW_RECT;
    }
}

/* a window and its frame, title and shadow */
static void need_window(struct window *w, int x, int y)
{
    need_rect(x - 12, y - TITLE_H - 12, w->w + 48, w->h + TITLE_H + 56);
}

/* an application asking for a region to be repainted, from its event handler */
void shell_repaint(int x, int y, int w, int h) { need_rect(x, y, w, h); }

static void need_crystal(void)
{
    int x, y, w, h;
    crystal_rect(&x, &y, &w, &h);
    need_rect(x, y, w, h);
}

static void handle(struct event *e)
{
    int id;
    if (popup_event(e)) {
        if (e->type == EV_MOUSE_MOVE) {         /* a highlight moved: just the panel */
            int x, y, w, h;
            popup_rect(&x, &y, &w, &h);
            need_rect(x, y, w, h);
        } else
            need(REDRAW_ALL);
        return;
    }
    if (e->type == EV_RIGHT_DOWN) {
        int id = e->b >= BAR_H ? window_hit(e->a, e->b) : -1;
        if (id < 0) {
            if (e->b >= BAR_H) {
                crystal_close();
                desktop_menu(e->a, e->b);
                need(REDRAW_ALL);
            }
        } else if (windows[id].event && e->b >= windows[id].y) {
            struct window *w = &windows[id];
            struct event local = *e;
            raise_window(id);
            local.a -= w->x;
            local.b -= w->y;
            w->event(w, &local);
            need(REDRAW_ALL);
        }
        return;
    }
    if (osk_visible && (e->type == EV_MOUSE_MOVE || e->type == EV_MOUSE_DOWN ||
                        e->type == EV_MOUSE_UP || e->type == EV_RIGHT_DOWN)) {
        int handled = osk_event(e);
        if (handled) {
            int x, y, w, h;
            osk_rect(&x, &y, &w, &h);
            need_rect(x, y, w, h);
            return;
        }
    }
    if (crystal_event(e)) {
        if (e->type == EV_MOUSE_MOVE) need_crystal();   /* hover: the gem's region */
        else need(REDRAW_ALL);
        return;
    }
    if (e->type != EV_MOUSE_MOVE)
        need(REDRAW_ALL);

    switch (e->type) {
    case EV_MOUSE_DOWN:
        if (e->b < BAR_H) {                             /* a window button */
            int bx = 18, i;
            for (i = 0; i < window_count; i++) {
                if (e->a >= bx && e->a < bx + 160) {
                    int id2 = z_order[i];
                    if (windows[id2].minimized) set_minimized(id2, 0);
                    raise_window(id2);
                    break;
                }
                bx += 168;
            }
            break;
        }
        id = window_hit(e->a, e->b);
        if (id < 0) {
            int ic = icon_hit(e->a, e->b);
            if (ic >= 0) {
                if (e->dbl && icon_sel == ic) shell_run_menu(icon_menu[ic]);  /* a double-click opens */
                else icon_sel = ic;
            } else {
                icon_sel = -1;
            }
            break;
        }
        icon_sel = -1;
        raise_window(id);
        {
            struct window *w = &windows[id];
            if (e->b < w->y) {
                int from_right = w->x + w->w - e->a;
                if (from_right < 30) { win_close(id); break; }
                if (from_right < 60 && !w->fixed) { set_maximized(w, !w->maxed); break; }
                if (from_right < 90) { set_minimized(id, 1); break; }
                if (e->dbl && !w->fixed) { set_maximized(w, !w->maxed); break; }
                if (w->maxed) break;                                 /* a maximized window stays put */
                drag_win = id;
                drag_dx = e->a - w->x;
                drag_dy = e->b - w->y;
            } else if (!w->fixed && !w->maxed && e->a > w->x + w->w - 20 && e->b > w->y + w->h - 20) {
                resize_win = id;                                     /* the grip */
            } else if (w->event) {
                struct event local = *e;
                local.a -= w->x;
                local.b -= w->y;
                w->event(w, &local);
            }
        }
        break;
    case EV_MOUSE_UP:
        drag_win = -1;
        resize_win = -1;
        if (focused >= 0 && windows[focused].event) {
            struct window *w = &windows[focused];
            struct event local = *e;
            local.a -= w->x;
            local.b -= w->y;
            w->event(w, &local);
        }
        break;
    case EV_MOUSE_MOVE:
        if (resize_win >= 0) {
            struct window *w = &windows[resize_win];
            int nw = e->a - w->x, nh = e->b - w->y;
            if (nw < 220) nw = 220;
            if (nh < 120) nh = 120;
            need_window(w, w->x, w->y);
            w->w = nw;
            w->h = nh;
            need_window(w, w->x, w->y);
            break;
        }
        if (drag_win >= 0) {
            struct window *w = &windows[drag_win];
            int ox = w->x, oy = w->y;
            w->x = e->a - drag_dx;
            w->y = e->b - drag_dy;
            if (w->y < BAR_H + TITLE_H) w->y = BAR_H + TITLE_H;
            if (w->x < -w->w + 80) w->x = -w->w + 80;
            if (w->x > scr_w - 80) w->x = scr_w - 80;
            need_window(w, ox, oy);             /* where it was: the desktop behind it */
            need_window(w, w->x, w->y);         /* and where it is now */
        } else if ((mouse_buttons & 1) && focused >= 0 && windows[focused].event) {
            /* a drag inside a window: a selection being made */
            struct window *w = &windows[focused];
            struct event local = *e;
            local.a -= w->x;
            local.b -= w->y;
            if (w->event(w, &local)) need_window(w, w->x, w->y);
        }
        break;
    case EV_KEY:
        if (e->a == K_WIN || e->a == K_WIN_R || e->a == K_MENU) {
            crystal_toggle();                           /* the crystal opens */
            break;
        }
        if (e->a == K_ESC) {
            if (crystal_is_open()) crystal_close();
            else if (focused >= 0) win_close(focused);
            break;
        }
        if (e->a == K_F1) { app_help(); break; }
        if ((e->a & ~K_SHIFT) == 0x137) { app_screenshot(); break; }   /* Print Screen */
        if (e->a == K_F10) { quit_requested = 1; break; }
        if (focused < 0 || !windows[focused].event) {
            /* nothing is listening: the arrows walk the desktop icons */
            if (e->a == K_DOWN) {
                icon_sel = (icon_sel + 1) % ICON_COUNT;
                break;
            }
            if (e->a == K_UP) {
                icon_sel = (icon_sel <= 0 ? ICON_COUNT : icon_sel) - 1;
                break;
            }
            if (e->a == K_ENTER && icon_sel >= 0) {
                shell_run_menu(icon_menu[icon_sel]);
                break;
            }
        }
        if (e->a == K_TAB && window_count > 1) {        /* through the windows */
            raise_window(z_order[0]);
            break;
        }
        if (focused >= 0 && windows[focused].event)
            windows[focused].event(&windows[focused], e);
        break;
    default:
        break;
    }
}

int main(int argc, char **argv)
{
    struct event e;
    int last_x = -1, last_y = -1;
    unsigned last_clock = 0;

    /* "EMBER 1024" or "EMBER 1920x1080" asks for a particular size */
    {
        int w = 1920, h = 1200, n = 0, i;
        const char *a = argc > 1 ? argv[1] : 0;
        if (a) {
            strncpy(start_arg, a, sizeof start_arg - 1);
            for (i = 0; a[i] >= '0' && a[i] <= '9'; i++) n = n * 10 + (a[i] - '0');
            if (n >= 640) {
                w = n;
                h = n * 3 / 4;
                if (a[i] == 'x' || a[i] == 'X') {
                    int m = 0;
                    for (i++; a[i] >= '0' && a[i] <= '9'; i++) m = m * 10 + (a[i] - '0');
                    if (m >= 480) h = m;
                }
            }
        }
        want_width = w;
        want_height = h;
    }
    if (draw_open(want_width, want_height) != 0) {
        printf("The graphics card offers no true-colour mode with a linear\n"
               "framebuffer; the desktop needs one.\n");
        return 1;
    }
    clock_start();
    /* Paint something the instant the mode is set: until the first frame
       reaches it, the card is showing whatever happened to be in its
       memory, and everything below here takes a moment. */
    crystal_x = scr_w / 2;
    draw_begin(1);
    wall_draw();
    draw_present();

    input_start_keyboard();
    touch_open();                               /* a laptop's pad, over I2C */
    input_open(scr_w, scr_h, touch_present);
    if (draw_direct()) cursor_sprite();         /* the pointer as the display engine's own sprite */
    wall_load_config();
    app_about();
    draw_begin(1);
    draw_all();
    draw_present();
    music_chime();                              /* the sound chip is slow to wake */

    while (!quit_requested) {
        int moved;
        redraw_level = REDRAW_NONE;
        touch_poll();
        while (next_event(&e)) handle(&e);
        if (crystal_busy()) need_crystal();             /* the gem is moving */
        if (now_ms() - last_clock > 20000) {
            last_clock = now_ms();
            need(REDRAW_ALL);
        }
        shell_stats.loops++;
        if (music_tick()) {                             /* its display moved */
            int x, y, w, h;
            music_display_rect(&x, &y, &w, &h);
            need_rect(x, y, w, h);
        }
        if (monitor_tick()) {
            int id = monitor_window();
            if (id >= 0) need_window(&windows[id], windows[id].x, windows[id].y);
        }
        if (osk_tick()) {
            int x, y, w, h;
            osk_rect(&x, &y, &w, &h);
            need_rect(x, y, w, h);
        }
        power_tick();
        if (viewer_tick()) need(REDRAW_WINDOWS);
        if (clock_tick()) {
            int id = clock_window();
            if (id >= 0) need_window(&windows[id], windows[id].x, windows[id].y);
        }
        moved = (mouse_x != last_x || mouse_y != last_y);
        if (moved && draw_direct()) {               /* a sprite: one register, no repaint */
            gpu_cursor_move(mouse_x, mouse_y);
            last_x = mouse_x;
            last_y = mouse_y;
            moved = 0;
        }

        if (redraw_level != REDRAW_NONE || moved) {
            unsigned t_draw = now_us(), t_present;
            draw_begin(redraw_level == REDRAW_ALL);     /* the buffer to draw: off screen, caught up */
            shell_stats.idle_us += now_us() - t_draw;   /* waiting for the display counts as idle */
            t_draw = now_us();
            cursor_lift();
            if (redraw_level == REDRAW_RECT) {
                /* One region: everything is drawn, but only within it,
                   and only that much reaches the screen. */
                clip_set(rect_x0, rect_y0, rect_x1 - rect_x0, rect_y1 - rect_y0);
                draw_all();
                clip_none();
            } else if (redraw_level == REDRAW_ALL) {
                draw_all();
            } else if (redraw_level == REDRAW_WINDOWS) {
                int i;
                for (i = 0; i < window_count; i++)
                    if (windows[z_order[i]].draw)
                        draw_window(&windows[z_order[i]], z_order[i] == focused, 0);
            }
            if (!draw_direct()) {
                cursor_save(mouse_x, mouse_y);
                draw_cursor(mouse_x, mouse_y);
            }
            last_x = mouse_x;
            last_y = mouse_y;
            t_present = now_us();
            draw_present();
            shell_stats.frames++;
            shell_stats.draw_us += t_present - t_draw;
            shell_stats.present_us += now_us() - t_present;
            shell_stats.present_bytes = draw_present_bytes;
            /* A full repaint at this size takes long enough that the sound
               chip can run dry while it happens; top the ring up again the
               moment the frame is out. */
            if (music_active()) music_tick();
        } else if (!music_active()) {
            /* Nothing to draw and nothing to feed: wait for the next
               interrupt rather than spinning.  With sound playing there is
               always the ring to keep ahead of, so we stay awake. */
            unsigned t_idle = now_us();
            __asm__ volatile("hlt");
            shell_stats.idle_us += now_us() - t_idle;
        }
    }

    touch_close();
    input_close();
    draw_close();
    return 0;
}
