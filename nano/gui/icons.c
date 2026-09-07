/* icons.c - which programs sit on the desktop.
 *
 * Every program the shell can put on the wall, as a grid of tiles: the
 * ones on the desktop are lit and ticked, the rest are dim.  A click puts
 * one on or takes it off, and the choice is written to the settings file
 * at once, so it survives the next boot.
 */
#include <nanolibc.h>
#include "nano.h"
#include "draw.h"
#include "input.h"
#include "shell.h"
#include "guiart.h"

#define PANEL     0x140F0A
#define EDGE      0x3A2C18
#define AMBER     0xF0A020
#define AMBER_HOT 0xFFC65A
#define TEXT      0xD8C8B0
#define TEXT_DIM  0x8A7C68

#define COLS   4
#define CELL_W 132
#define CELL_H 116
#define HEAD   44
#define WIN_W  (COLS * CELL_W + 28)

static int win_id = -1;

static void icons_draw(struct window *w)
{
    int n = icons_catalogue_n(), i;
    text(F_SMALL, w->x + 16, w->y + 14, "The programs on the desktop; a click adds one or takes it away.", TEXT_DIM);
    for (i = 0; i < n; i++) {
        int cx = w->x + 14 + (i % COLS) * CELL_W, cy = w->y + HEAD + (i / COLS) * CELL_H;
        int on = icons_has(i);
        const struct image *art = icons_cat_art(i);
        const char *label = icons_cat_label(i);
        int tw = text_width(F_SMALL, label);
        round_fill(cx, cy, CELL_W - 10, CELL_H - 10, 6, on ? 0x241B10 : 0x120E0A);
        round_frame(cx, cy, CELL_W - 10, CELL_H - 10, 6, on ? AMBER : EDGE);
        if (art) {
            int ax = cx + (CELL_W - 10) / 2 - art->w / 2;
            if (on) image_draw(art, ax, cy + 16);
            else image_draw_tinted(art, ax, cy + 16, 0x000000, 130);
        }
        text(F_SMALL, cx + (CELL_W - 10) / 2 - tw / 2, cy + CELL_H - 34, label, on ? AMBER_HOT : TEXT_DIM);
        if (on) {                                   /* a tick in the corner */
            int tx = cx + CELL_W - 32, ty = cy + 12;
            line(tx, ty + 6, tx + 4, ty + 10, 2, AMBER_HOT);
            line(tx + 4, ty + 10, tx + 12, ty, 2, AMBER_HOT);
        }
    }
    text(F_SMALL, w->x + 16, w->y + w->h - 24,
         "The order is the order they were added; take one off and add it again to move it to the end.", TEXT_DIM);
}

static int icons_event(struct window *w, struct event *e)
{
    if (e->type == EV_MOUSE_DOWN && e->b >= HEAD) {
        int col = (e->a - 14) / CELL_W, row = (e->b - HEAD) / CELL_H;
        int i = row * COLS + col;
        if (col >= 0 && col < COLS && i >= 0 && i < icons_catalogue_n()) {
            icons_toggle(i);
            cfg_write();                            /* kept at once */
            return 1;
        }
    }
    return 0;
}

void icons_closed(int id) { if (id == win_id) win_id = -1; }

void app_icons(void)
{
    int rows = (icons_catalogue_n() + COLS - 1) / COLS;
    if (win_id >= 0) return;
    win_id = win_open("Icons", WIN_W, HEAD + rows * CELL_H + 34, icons_draw, icons_event);
}
