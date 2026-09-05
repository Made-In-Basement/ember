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
#include "input.h"
#include "shell.h"

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
static int quit_requested;

/* ---------------------------------------------------------------- windows */
struct window *win_at(int id) { return &windows[id]; }
int win_count(void) { return window_count; }
int win_focused(void) { return focused; }
void shell_quit(void) { quit_requested = 1; }

int win_open(const char *title, int w, int h, void (*draw)(struct window *),
             int (*event)(struct window *, struct event *))
{
    struct window *win;
    int id = window_count;
    if (window_count >= MAX_WINDOWS) return -1;
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
    return id;
}

void win_close(int id)
{
    int i, j;
    music_closed(id);
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

    /* the close mark */
    {
        int cxp = tx + tw - 20, cyp = ty + TITLE_H / 2, i;
        uint32_t c = is_focused ? AMBER : TEXT_DIM;
        for (i = -4; i <= 4; i++) {
            pixel_blend(cxp + i, cyp + i, c, 255);
            pixel_blend(cxp + i, cyp - i, c, 255);
            pixel_blend(cxp + i + 1, cyp + i, c, 90);
            pixel_blend(cxp + i + 1, cyp - i, c, 90);
        }
    }

    /* the body: a face that catches light at the top and falls away */
    vgradient(tx, w->y, tw, w->h, 0x1F1810, PANEL_DARK);
    fill(tx, w->y, tw, 1, 0x33271A);

    if (w->draw) {
        int ox, oy, ow, oh;
        clip_get(&ox, &oy, &ow, &oh);
        clip_set(w->x, w->y, w->w, w->h);
        w->draw(w);
        clip_set(ox, oy, ow, oh);
    }
}

/* ---------------------------------------------------------------- icons */
struct desk_icon { const char *label; int kind; };
static const struct desk_icon icons[] = {
    { "Files",  0 },
    { "Music",  1 },
    { "Text",   2 },
    { "Doom",   3 },
    { "About",  4 },
};
#define ICON_COUNT 5
#define ICON_W     96
#define ICON_H     92
#define ICON_X     28
#define ICON_Y     (BAR_H + 24)
static int icon_sel = -1;

static void icon_art(int kind, int x, int y, int lit)
{
    uint32_t face = lit ? AMBER_HOT : AMBER;
    uint32_t deep = lit ? 0xB07418 : 0x7A5214;
    int p[16];
    switch (kind) {
    case 0:                                     /* a folder */
        p[0] = x;      p[1] = y + 8;
        p[2] = x + 18; p[3] = y + 8;
        p[4] = x + 24; p[5] = y + 15;
        p[6] = x + 46; p[7] = y + 15;
        p[8] = x + 46; p[9] = y + 40;
        p[10] = x;     p[11] = y + 40;
        poly_fill(p, 6, deep);
        fill(x + 3, y + 19, 40, 18, face);
        break;
    case 1:                                     /* a note */
        fill(x + 26, y + 6, 4, 26, face);
        fill(x + 26, y + 6, 16, 5, face);
        round_fill(x + 14, y + 28, 16, 12, 6, deep);
        break;
    case 2:                                     /* a page */
        fill(x + 8, y + 5, 30, 38, deep);
        fill(x + 11, y + 11, 24, 2, face);
        fill(x + 11, y + 17, 24, 2, face);
        fill(x + 11, y + 23, 24, 2, face);
        fill(x + 11, y + 29, 16, 2, face);
        break;
    case 3:                                     /* a chip */
        fill(x + 10, y + 10, 28, 28, deep);
        fill(x + 16, y + 16, 16, 16, face);
        {
            int i;
            for (i = 0; i < 4; i++) {
                fill(x + 14 + i * 7, y + 4, 3, 6, face);
                fill(x + 14 + i * 7, y + 38, 3, 6, face);
                fill(x + 4, y + 14 + i * 7, 6, 3, face);
                fill(x + 38, y + 14 + i * 7, 6, 3, face);
            }
        }
        break;
    default:                                    /* a mark */
        round_fill(x + 10, y + 8, 28, 28, 14, deep);
        fill(x + 22, y + 15, 4, 4, face);
        fill(x + 22, y + 22, 4, 10, face);
        break;
    }
}

static void draw_icons(void)
{
    int i;
    for (i = 0; i < ICON_COUNT; i++) {
        int x = ICON_X, y = ICON_Y + i * ICON_H;
        int lit = (i == icon_sel);
        int tw = text_width(F_SMALL, icons[i].label);
        if (lit) {
            round_fill_alpha(x - 6, y - 4, ICON_W, ICON_H - 12, 6, AMBER, 26);
            round_frame_alpha(x - 6, y - 4, ICON_W, ICON_H - 12, 6, AMBER_DIM, 160);
        }
        icon_art(icons[i].kind, x + 20, y, lit);
        text(F_SMALL, x + (ICON_W - 12) / 2 - tw / 2, y + 50,
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
    int i;
    vgradient(0, 0, scr_w, scr_h, BG_TOP, BG_BOTTOM);
    for (i = 0; i < scr_w; i += 40)
        fill(i, 0, 1, scr_h, GRID);
    for (i = 0; i < scr_h; i += 40)
        fill(0, i, scr_w, 1, GRID);
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
                     bw - 20, w->title, is_focused ? AMBER_HOT : TEXT_DIM);
        bx += bw + 8;
    }

    read_clock(clk, sizeof clk);
    text(F_BOLD, scr_w - 24 - text_width(F_BOLD, clk),
         (BAR_H - text_height(F_BOLD)) / 2, clk, AMBER);
}

#define CUR_W 12
#define CUR_H 18
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

static void draw_cursor(int x, int y)
{
    static const char *shape[] = {
        "X.........", "XX........", "XoX.......", "XooX......",
        "XoooX.....", "XooooX....", "XoooooX...", "XooooooX..",
        "XoooooooX.", "XooooXXXXX", "XooXoX....", "XoX.XoX...",
        "XX..XoX...", "X....XoX..", ".....XoX..", "......XX..", 0
    };
    int row, col;
    for (row = 0; shape[row]; row++)
        for (col = 0; shape[row][col]; col++) {
            char c = shape[row][col];
            if (c == 'X') pixel_blend(x + col, y + row, 0x000000, 210);
            else if (c == 'o') pixel_blend(x + col, y + row, AMBER_HOT, 255);
        }
    damage(x, y, 12, 18);
}

static void draw_all(void)
{
    int i;
    draw_desktop();
    for (i = 0; i < window_count; i++)
        draw_window(&windows[z_order[i]], z_order[i] == focused, 1);
    draw_bar();
    crystal_draw();
}

/* ---------------------------------------------------------------- events */
void shell_run_menu(int item)
{
    switch (item) {
    case 0: app_files(); break;
    case 1: app_music(); break;
    case 2: app_text(); break;
    case 3: app_doom(); break;
    case 4: app_help(); break;
    case 5: app_about(); break;
    case 6: quit_requested = 1; break;
    default: break;
    }
}

static void handle(struct event *e)
{
    int id;
    if (crystal_event(e))
        return;

    switch (e->type) {
    case EV_MOUSE_DOWN:
        if (e->b < BAR_H) {                             /* a window button */
            int bx = 18, i;
            for (i = 0; i < window_count; i++) {
                if (e->a >= bx && e->a < bx + 160) { raise_window(z_order[i]); break; }
                bx += 168;
            }
            break;
        }
        id = window_hit(e->a, e->b);
        if (id < 0) {
            int ic = icon_hit(e->a, e->b);
            if (ic >= 0) {
                if (icon_sel == ic) shell_run_menu(ic);  /* a second click opens */
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
                if (e->a > w->x + w->w - 30) { win_close(id); break; }
                drag_win = id;
                drag_dx = e->a - w->x;
                drag_dy = e->b - w->y;
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
        break;
    case EV_MOUSE_MOVE:
        if (drag_win >= 0) {
            struct window *w = &windows[drag_win];
            w->x = e->a - drag_dx;
            w->y = e->b - drag_dy;
            if (w->y < BAR_H + TITLE_H) w->y = BAR_H + TITLE_H;
            if (w->x < -w->w + 80) w->x = -w->w + 80;
            if (w->x > scr_w - 80) w->x = scr_w - 80;
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
        if (e->a == K_F10) { quit_requested = 1; break; }
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
    (void)argc; (void)argv;

    if (draw_open(1280, 1024) != 0) {
        printf("The graphics card offers no true-colour mode with a linear\n"
               "framebuffer; the desktop needs one.\n");
        return 1;
    }
    clock_start();
    input_open(scr_w, scr_h);
    input_start_keyboard();
    crystal_x = scr_w / 2;
    music_chime();
    app_about();

    while (!quit_requested) {
        int full = 0, light = 0;
        while (next_event(&e)) { handle(&e); full = 1; }
        if (crystal_busy()) full = 1;                   /* the gem is moving */
        if (now_ms() - last_clock > 20000) { last_clock = now_ms(); full = 1; }
        if (music_tick()) light = 1;                    /* only its own window */
        if (mouse_x != last_x || mouse_y != last_y) light = 1;

        if (full || light) {
            /* Redrawing the whole desktop thirty times a second would push
               five megabytes a frame at the screen; when only a window's
               contents changed, repaint that window and nothing else. */
            cursor_lift();
            if (full) {
                draw_all();
            } else if (focused >= 0) {
                int i;
                for (i = 0; i < window_count; i++)
                    if (windows[z_order[i]].draw)
                        draw_window(&windows[z_order[i]], z_order[i] == focused, 0);
            }
            cursor_save(mouse_x, mouse_y);
            draw_cursor(mouse_x, mouse_y);
            last_x = mouse_x;
            last_y = mouse_y;
            draw_present();
        } else if (!music_active()) {
            /* Nothing to draw and nothing to feed: wait for the next
               interrupt rather than spinning.  With sound playing there is
               always the ring to keep ahead of, so we stay awake. */
            __asm__ volatile("hlt");
        }
    }

    input_close();
    draw_close();
    return 0;
}
