/* clock.c - the time, large; a stopwatch; an alarm.
 *
 * The time comes from the real-time clock chip, read through its two
 * ports.  The stopwatch runs on the processor's own counter.  The alarm
 * is a time of day: when the clock reaches it the window glows and says
 * so until it is switched off.
 */
#include <nanolibc.h>
#include "nano.h"
#include "draw.h"
#include "input.h"
#include "shell.h"
#include "power.h"

#define PANEL       0x1A140D
#define EDGE        0x4A3618
#define AMBER       0xF0A020
#define AMBER_HOT   0xFFC65A
#define TEXT        0xD8C8B0
#define TEXT_DIM    0x8A7C68
#define WELL        0x100C08
#define WARM        0xF0602A

static const char *day_names[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *month_names[] = { "January", "February", "March", "April", "May", "June", "July",
                                     "August", "September", "October", "November", "December" };

/* ------------------------------------------------------------ the chip */
static int cmos(int reg)
{
    int v;
    outb(0x70, reg);
    v = inb(0x71);
    return (v >> 4) * 10 + (v & 0x0F);          /* it counts in BCD */
}

/* the day of the week, from the date (Zeller) */
static int weekday(int d, int m, int y)
{
    int k, j;
    if (m < 3) { m += 12; y--; }
    k = y % 100;
    j = y / 100;
    return (d + 13 * (m + 1) / 5 + k + k / 4 + j / 4 + 5 * j + 6) % 7;   /* 0 = Sunday */
}

void rtc_read(struct rtc_time *t)
{
    int century;
    /* the chip must not be mid-update; wait for the flag to drop */
    { int n = 100000; outb(0x70, 0x0A); while (n-- && (inb(0x71) & 0x80)) ; }
    t->sec = cmos(0x00);
    t->min = cmos(0x02);
    t->hour = cmos(0x04);
    t->day = cmos(0x07);
    t->month = cmos(0x08);
    t->year = cmos(0x09);
    century = cmos(0x32);
    t->year += (century >= 19 && century <= 21 ? century : 20) * 100;
    if (t->month < 1 || t->month > 12) t->month = 1;
    if (t->day < 1 || t->day > 31) t->day = 1;
    t->wday = weekday(t->day, t->month, t->year);
}

/* ------------------------------------------------------------ the window */
static int win_id = -1;
static int last_sec = -1;
static int sw_running, alarm_on, alarm_h = 7, alarm_m = 0, ringing;
static unsigned sw_start, sw_accum;

static unsigned sw_elapsed(void)
{
    return sw_accum + (sw_running ? now_ms() - sw_start : 0);
}

static void button(int x, int y, int w, const char *label, int lit)
{
    round_fill(x, y, w, 30, 4, lit ? 0x4A3618 : 0x2A2114);
    round_frame(x, y, w, 30, 4, lit ? AMBER : EDGE);
    text(F_SMALL, x + (w - text_width(F_SMALL, label)) / 2, y + (30 - text_height(F_SMALL)) / 2,
         label, lit ? AMBER_HOT : TEXT);
}

static void clock_draw(struct window *w)
{
    struct rtc_time t;
    char b[64];
    int x = w->x, y = w->y, cx;
    unsigned el = sw_elapsed();

    rtc_read(&t);
    if (ringing) {
        round_fill(x + 12, y + 12, w->w - 24, 96, 6, 0x3A2010);
        glow(x + 12, y + 12, w->w - 24, 96, 6, WARM, 3);
    }
    snprintf(b, sizeof b, "%02d:%02d:%02d", t.hour, t.min, t.sec);
    cx = x + (w->w - text_width(F_CLOCK, b)) / 2;
    text(F_CLOCK, cx, y + 18, b, ringing ? WARM : AMBER_HOT);
    snprintf(b, sizeof b, "%s, %d %s %d", day_names[t.wday], t.day, month_names[t.month - 1], t.year);
    text(F_NORMAL, x + (w->w - text_width(F_NORMAL, b)) / 2, y + 18 + text_height(F_CLOCK) + 4, b, TEXT_DIM);

    /* the stopwatch */
    y += 118;
    fill(x + 16, y, w->w - 32, 1, EDGE);
    text(F_BOLD, x + 16, y + 10, "Stopwatch", TEXT_DIM);
    snprintf(b, sizeof b, "%u:%02u.%u", el / 60000, (el / 1000) % 60, (el / 100) % 10);
    text(F_TITLE, x + 16, y + 34, b, sw_running ? AMBER_HOT : TEXT);
    button(x + w->w - 190, y + 36, 80, sw_running ? "Stop" : "Start", sw_running);
    button(x + w->w - 100, y + 36, 80, "Reset", 0);

    /* the alarm */
    y += 84;
    fill(x + 16, y, w->w - 32, 1, EDGE);
    text(F_BOLD, x + 16, y + 10, "Alarm", TEXT_DIM);
    snprintf(b, sizeof b, "%02d:%02d", alarm_h, alarm_m);
    text(F_TITLE, x + 16, y + 34, b, alarm_on ? AMBER_HOT : TEXT);
    button(x + 120, y + 36, 30, "-", 0);  button(x + 154, y + 36, 30, "+", 0);   /* hours */
    button(x + 200, y + 36, 30, "-", 0);  button(x + 234, y + 36, 30, "+", 0);   /* minutes */
    text(F_SMALL, x + 122, y + 70, "hour", TEXT_DIM);
    text(F_SMALL, x + 200, y + 70, "minute", TEXT_DIM);
    button(x + w->w - 100, y + 36, 80, ringing ? "Quiet" : alarm_on ? "On" : "Off", alarm_on);
    if (ringing) text(F_BOLD, x + w->w - 190, y + 74, "Alarm!", WARM);
}

static int clock_event(struct window *w, struct event *e)
{
    if (e->type != EV_MOUSE_DOWN) return 0;
    {
        int x = e->a, y = e->b - 118;
        /* the stopwatch row */
        if (y >= 36 && y < 66) {
            if (x >= w->w - 190 && x < w->w - 110) {
                if (sw_running) { sw_accum += now_ms() - sw_start; sw_running = 0; }
                else { sw_start = now_ms(); sw_running = 1; }
                return 1;
            }
            if (x >= w->w - 100 && x < w->w - 20) { sw_accum = 0; sw_running = 0; return 1; }
        }
        y -= 84;
        if (y >= 36 && y < 66) {
            if (x >= 120 && x < 150) alarm_h = (alarm_h + 23) % 24;
            else if (x >= 154 && x < 184) alarm_h = (alarm_h + 1) % 24;
            else if (x >= 200 && x < 230) alarm_m = (alarm_m + 59) % 60;
            else if (x >= 234 && x < 264) alarm_m = (alarm_m + 1) % 60;
            else if (x >= w->w - 100 && x < w->w - 20) {
                if (ringing) { ringing = 0; alarm_on = 0; }
                else alarm_on = !alarm_on;
            }
            return 1;
        }
    }
    return 1;
}

void clock_closed(int id) { if (id == win_id) win_id = -1; }

void app_clock(void)
{
    if (win_id >= 0) return;
    win_id = win_open("Clock", 440, 330, clock_draw, clock_event);
}

/* from the main loop: 1 when the window should be repainted */
int clock_tick(void)
{
    struct rtc_time t;
    int changed = 0;
    rtc_read(&t);
    if (alarm_on && !ringing && t.hour == alarm_h && t.min == alarm_m && t.sec < 2) {
        ringing = 1;
        if (win_id < 0) app_clock();
        changed = 1;
    }
    if (win_id < 0) return 0;
    if (t.sec != last_sec || sw_running) { last_sec = t.sec; changed = 1; }
    return changed;
}

int clock_window(void) { return win_id; }
