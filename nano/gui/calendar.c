/* calendar.c - a month at a time, with today marked.
 *
 * Shaded cells, weekends set apart, today glowing, and a light or dark
 * page chosen with a button.  Arrows move through the months; a click on
 * the title comes back to today.
 */
#include <nanolibc.h>
#include "nano.h"
#include "draw.h"
#include "input.h"
#include "shell.h"
#include "power.h"

#define AMBER       0xF0A020
#define AMBER_HOT   0xFFC65A

struct theme {
    uint32_t page, head_top, head_bottom, cell_top, cell_bottom, weekend, edge, text, dim, today_ink;
};
static const struct theme themes[2] = {
    { 0x14100B, 0x2A2114, 0x1A140D, 0x221B12, 0x1A140D, 0x1C1610, 0x3A2C18, 0xD8C8B0, 0x8A7C68, 0x1A140D },
    { 0xF4EEE2, 0xE6D8C0, 0xF4EEE2, 0xFFFFFF, 0xF0E8DA, 0xEDE3D0, 0xC8B89C, 0x2A2014, 0x8A7C68, 0xFFFFFF },
};
static int dark = 1;
#define T (themes[dark ? 0 : 1])

static const char *month_names[] = { "January", "February", "March", "April", "May", "June", "July",
                                     "August", "September", "October", "November", "December" };
static const char *day_abbr[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *day_names[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };

static int win_id = -1;
static int show_month, show_year;

static int leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static int days_in(int m, int y)
{
    static const int d[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return m == 2 && leap(y) ? 29 : d[m - 1];
}
static int first_weekday(int m, int y)          /* 0 = Sunday */
{
    int k, j;
    if (m < 3) { m += 12; y--; }
    k = y % 100;
    j = y / 100;
    return (1 + 13 * (m + 1) / 5 + k + k / 4 + j / 4 + 5 * j + 6) % 7;
}

#define CELL_W   64
#define CELL_H   54
#define GRID_X   18
#define GRID_Y   96
#define HEAD_H   56

static void arrow(int x, int y, int left)
{
    int pts[6];
    round_fill(x, y, 34, 30, 5, T.cell_top);
    round_frame(x, y, 34, 30, 5, T.edge);
    if (left) { pts[0] = x + 21; pts[1] = y + 8; pts[2] = x + 12; pts[3] = y + 15; pts[4] = x + 21; pts[5] = y + 22; }
    else      { pts[0] = x + 13; pts[1] = y + 8; pts[2] = x + 22; pts[3] = y + 15; pts[4] = x + 13; pts[5] = y + 22; }
    poly_fill(pts, 3, AMBER);
}

static void calendar_draw(struct window *w)
{
    struct rtc_time t;
    char b[48];
    int x = w->x, y = w->y, i, d, first, n;

    rtc_read(&t);
    round_fill(x, y, w->w, w->h, 0, T.page);
    vgradient(x, y, w->w, HEAD_H, T.head_top, T.head_bottom);
    fill(x, y + HEAD_H - 1, w->w, 1, T.edge);

    snprintf(b, sizeof b, "%s %d", month_names[show_month - 1], show_year);
    text(F_TITLE, x + (w->w - text_width(F_TITLE, b)) / 2, y + 12, b, AMBER_HOT);
    arrow(x + 16, y + 13, 1);
    arrow(x + w->w - 50, y + 13, 0);

    /* the theme button, small, at the foot */
    round_fill(x + w->w - 82, y + w->h - 36, 66, 24, 4, T.cell_top);
    round_frame(x + w->w - 82, y + w->h - 36, 66, 24, 4, T.edge);
    text(F_SMALL, x + w->w - 82 + (66 - text_width(F_SMALL, dark ? "Light" : "Dark")) / 2,
         y + w->h - 36 + (24 - text_height(F_SMALL)) / 2, dark ? "Light" : "Dark", T.text);

    for (i = 0; i < 7; i++)
        text(F_SMALL, x + GRID_X + i * CELL_W + (CELL_W - text_width(F_SMALL, day_abbr[i])) / 2,
             y + GRID_Y - 24, day_abbr[i], (i == 0 || i == 6) ? AMBER : T.dim);

    first = first_weekday(show_month, show_year);
    n = days_in(show_month, show_year);
    for (i = 0; i < 42; i++) {
        int col = i % 7, row = i / 7;
        int cx = x + GRID_X + col * CELL_W, cy = y + GRID_Y + row * CELL_H;
        int weekend = (col == 0 || col == 6);
        d = i - first + 1;
        if (d < 1 || d > n) {
            round_fill_alpha(cx + 2, cy + 2, CELL_W - 4, CELL_H - 4, 5, T.cell_bottom, 90);
            continue;
        }
        if (d == t.day && show_month == t.month && show_year == t.year) {
            round_fill(cx + 2, cy + 2, CELL_W - 4, CELL_H - 4, 6, AMBER);
            glow(cx + 2, cy + 2, CELL_W - 4, CELL_H - 4, 6, AMBER, 3);
            snprintf(b, sizeof b, "%d", d);
            text(F_BOLD, cx + (CELL_W - text_width(F_BOLD, b)) / 2, cy + (CELL_H - text_height(F_BOLD)) / 2,
                 b, T.today_ink);
            continue;
        }
        vgradient(cx + 2, cy + 2, CELL_W - 4, CELL_H - 4, weekend ? T.weekend : T.cell_top, T.cell_bottom);
        round_frame(cx + 2, cy + 2, CELL_W - 4, CELL_H - 4, 5, T.edge);
        snprintf(b, sizeof b, "%d", d);
        text(F_NORMAL, cx + (CELL_W - text_width(F_NORMAL, b)) / 2, cy + (CELL_H - text_height(F_NORMAL)) / 2,
             b, weekend ? T.dim : T.text);
    }
    snprintf(b, sizeof b, "Today is %s, %d %s %d", day_names[t.wday], t.day, month_names[t.month - 1], t.year);
    text(F_SMALL, x + GRID_X, y + w->h - 32, b, T.dim);
}

static int calendar_event(struct window *w, struct event *e)
{
    struct rtc_time t;
    if (e->type != EV_MOUSE_DOWN) return 0;
    if (e->b >= 13 && e->b < 43) {
        if (e->a >= 16 && e->a < 50) {
            if (--show_month < 1) { show_month = 12; show_year--; }
            return 1;
        }
        if (e->a >= w->w - 50 && e->a < w->w - 16) {
            if (++show_month > 12) { show_month = 1; show_year++; }
            return 1;
        }
        rtc_read(&t);                                            /* the title: back to today */
        show_month = t.month;
        show_year = t.year;
        return 1;
    }
    if (e->b >= w->h - 36 && e->b < w->h - 12 && e->a >= w->w - 82 && e->a < w->w - 16) {
        dark = !dark;
        return 1;
    }
    return 1;
}

void calendar_closed(int id) { if (id == win_id) win_id = -1; }

void app_calendar(void)
{
    struct rtc_time t;
    if (win_id >= 0) return;
    rtc_read(&t);
    show_month = t.month;
    show_year = t.year;
    win_id = win_open("Calendar", 7 * CELL_W + GRID_X * 2, GRID_Y + 6 * CELL_H + 44, calendar_draw, calendar_event);
}
