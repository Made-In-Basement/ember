/* calendar.c - a month at a time, with today marked. */
#include <nanolibc.h>
#include "nano.h"
#include "draw.h"
#include "input.h"
#include "shell.h"
#include "power.h"

#define EDGE        0x4A3618
#define AMBER       0xF0A020
#define AMBER_HOT   0xFFC65A
#define TEXT        0xD8C8B0
#define TEXT_DIM    0x8A7C68
#define WELL        0x100C08

static const char *month_names[] = { "January", "February", "March", "April", "May", "June", "July",
                                     "August", "September", "October", "November", "December" };
static const char *day_abbr[] = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" };

static int win_id = -1;
static int show_month, show_year;       /* the month on display */

static int leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static int days_in(int m, int y)
{
    static const int d[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return m == 2 && leap(y) ? 29 : d[m - 1];
}

static int first_weekday(int m, int y)      /* 0 = Sunday, for the 1st */
{
    int k, j;
    if (m < 3) { m += 12; y--; }
    k = y % 100;
    j = y / 100;
    return (1 + 13 * (m + 1) / 5 + k + k / 4 + j / 4 + 5 * j + 6) % 7;
}

#define CELL_W   58
#define CELL_H   46
#define GRID_X   16
#define GRID_Y   80

static void calendar_draw(struct window *w)
{
    struct rtc_time t;
    char b[48];
    int x = w->x, y = w->y, i, d, col, row, first, n;

    rtc_read(&t);
    snprintf(b, sizeof b, "%s %d", month_names[show_month - 1], show_year);
    text(F_TITLE, x + (w->w - text_width(F_TITLE, b)) / 2, y + 14, b, AMBER_HOT);
    /* the arrows */
    round_fill(x + 16, y + 16, 34, 30, 4, 0x2A2114);
    round_frame(x + 16, y + 16, 34, 30, 4, EDGE);
    text(F_BOLD, x + 16 + (34 - text_width(F_BOLD, "<")) / 2, y + 16 + (30 - text_height(F_BOLD)) / 2, "<", TEXT);
    round_fill(x + w->w - 50, y + 16, 34, 30, 4, 0x2A2114);
    round_frame(x + w->w - 50, y + 16, 34, 30, 4, EDGE);
    text(F_BOLD, x + w->w - 50 + (34 - text_width(F_BOLD, ">")) / 2, y + 16 + (30 - text_height(F_BOLD)) / 2, ">", TEXT);

    for (i = 0; i < 7; i++)
        text(F_SMALL, x + GRID_X + i * CELL_W + (CELL_W - text_width(F_SMALL, day_abbr[i])) / 2,
             y + GRID_Y - 22, day_abbr[i], TEXT_DIM);
    fill(x + GRID_X, y + GRID_Y - 4, 7 * CELL_W, 1, EDGE);

    first = first_weekday(show_month, show_year);
    n = days_in(show_month, show_year);
    for (d = 1; d <= n; d++) {
        int idx = first + d - 1;
        int today = (d == t.day && show_month == t.month && show_year == t.year);
        col = idx % 7;
        row = idx / 7;
        {
            int cx = x + GRID_X + col * CELL_W, cy = y + GRID_Y + row * CELL_H;
            if (today) {
                round_fill(cx + 3, cy + 2, CELL_W - 6, CELL_H - 4, 6, AMBER);
                glow(cx + 3, cy + 2, CELL_W - 6, CELL_H - 4, 6, AMBER, 3);
            }
            snprintf(b, sizeof b, "%d", d);
            text(F_NORMAL, cx + (CELL_W - text_width(F_NORMAL, b)) / 2, cy + (CELL_H - text_height(F_NORMAL)) / 2,
                 b, today ? 0x1A140D : (col == 0 || col == 6) ? TEXT_DIM : TEXT);
        }
    }
    snprintf(b, sizeof b, "Today is %d %s %d", t.day, month_names[t.month - 1], t.year);
    text(F_SMALL, x + GRID_X, y + GRID_Y + 6 * CELL_H + 8, b, TEXT_DIM);
}

static int calendar_event(struct window *w, struct event *e)
{
    if (e->type != EV_MOUSE_DOWN) return 0;
    if (e->b >= 16 && e->b < 46) {
        if (e->a >= 16 && e->a < 50) {
            if (--show_month < 1) { show_month = 12; show_year--; }
            return 1;
        }
        if (e->a >= w->w - 50 && e->a < w->w - 16) {
            if (++show_month > 12) { show_month = 1; show_year++; }
            return 1;
        }
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
    win_id = win_open("Calendar", 7 * CELL_W + GRID_X * 2, GRID_Y + 6 * CELL_H + 36, calendar_draw, calendar_event);
}
