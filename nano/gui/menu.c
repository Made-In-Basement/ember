/* menu.c - the menu that appears where you right-click.
 *
 * One small panel, drawn over everything, that closes on the next click.
 * The desktop uses it to offer backgrounds; a window can use it for
 * whatever it has to offer.
 */
#include <nanolibc.h>
#include "nano.h"
#include "draw.h"
#include "input.h"
#include "shell.h"

#define PANEL     0x1E1710
#define PANEL_HI  0x2A2114
#define EDGE      0x4A3618
#define AMBER     0xF0A020
#define AMBER_HOT 0xFFC65A
#define TEXT      0xD8C8B0
#define TEXT_DIM  0x8A7C68

#define ROW_H     28
#define MENU_W    210
#define PAD       8

static int open_flag, mx, my, count, sel = -1;
static const char *labels[16];
static int marks[16];                   /* a dot beside the one in use */
static void (*chosen)(int item);

void popup_open(int x, int y, const char **items, const int *ticks, int n,
                void (*on_choice)(int))
{
    int i;
    if (n > 16) n = 16;
    for (i = 0; i < n; i++) {
        labels[i] = items[i];
        marks[i] = ticks ? ticks[i] : 0;
    }
    count = n;
    chosen = on_choice;
    mx = x;
    my = y;
    if (mx + MENU_W > scr_w) mx = scr_w - MENU_W - 4;
    if (my + n * ROW_H + PAD * 2 > scr_h) my = scr_h - n * ROW_H - PAD * 2 - 4;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    sel = -1;
    open_flag = 1;
    damage_all();
}

void popup_close(void)
{
    if (open_flag) {
        open_flag = 0;
        sel = -1;
        damage_all();
    }
}

int popup_is_open(void) { return open_flag; }

/* the panel and its shadow */
void popup_rect(int *x, int *y, int *w, int *h)
{
    *x = mx - 4;
    *y = my - 4;
    *w = MENU_W + 16;
    *h = count * ROW_H + PAD * 2 + 16;
}

static int hit(int x, int y)
{
    if (!open_flag) return -1;
    if (x < mx || x >= mx + MENU_W) return -1;
    if (y < my + PAD || y >= my + PAD + count * ROW_H) return -1;
    return (y - my - PAD) / ROW_H;
}

void popup_draw(void)
{
    int h = count * ROW_H + PAD * 2, i;
    if (!open_flag) return;
    shadow(mx + 2, my + 3, MENU_W, h, 5, 7);
    round_fill(mx, my, MENU_W, h, 5, PANEL);
    round_frame(mx, my, MENU_W, h, 5, EDGE);
    for (i = 0; i < count; i++) {
        int ry = my + PAD + i * ROW_H;
        uint32_t c = TEXT;
        if (i == sel) {
            fill(mx + 3, ry, MENU_W - 6, ROW_H, PANEL_HI);
            fill(mx + 3, ry, 3, ROW_H, AMBER);
            c = AMBER_HOT;
        }
        if (marks[i])
            fill(mx + 12, ry + ROW_H / 2 - 2, 5, 5, AMBER);
        text(F_NORMAL, mx + 26, ry + (ROW_H - text_height(F_NORMAL)) / 2,
             labels[i], c);
    }
}

/* returns 1 when it has dealt with the event */
int popup_event(struct event *e)
{
    if (!open_flag) return 0;
    if (e->type == EV_MOUSE_MOVE) {
        int was = sel;
        sel = hit(e->a, e->b);
        return was != sel;
    }
    if (e->type == EV_MOUSE_DOWN || e->type == EV_RIGHT_DOWN) {
        int item = hit(e->a, e->b);
        void (*fn)(int) = chosen;
        popup_close();
        if (item >= 0 && fn) fn(item);
        return 1;
    }
    if (e->type == EV_KEY) {
        if (e->a == K_DOWN) { sel = (sel + 1) % count; return 1; }
        if (e->a == K_UP)   { sel = (sel <= 0 ? count : sel) - 1; return 1; }
        if (e->a == K_ENTER && sel >= 0) {
            void (*fn)(int) = chosen;
            int item = sel;
            popup_close();
            if (fn) fn(item);
            return 1;
        }
        popup_close();
        return 1;
    }
    return 0;
}
