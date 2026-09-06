/* clock.c - the time, three ways; a stopwatch; an alarm.
 *
 * Digital in the big face, analogue with hands, or seven-segment; and a
 * slim form that is just the time, small enough to leave in a corner.
 * The time comes from the real-time clock chip, the stopwatch from the
 * processor's counter.  The alarm is a time of day: when the clock
 * reaches it the window glows, says so, and sounds the chime every few
 * seconds until it is quieted.
 */
#include <nanolibc.h>
#include "nano.h"
#include "draw.h"
#include "input.h"
#include "shell.h"
#include "power.h"

#define EDGE        0x4A3618
#define AMBER       0xF0A020
#define AMBER_HOT   0xFFC65A
#define AMBER_DEEP  0x8A5E16
#define TEXT        0xD8C8B0
#define TEXT_DIM    0x8A7C68
#define WELL        0x0E0A06
#define FACE        0x1A140D
#define WARM        0xF0602A
#define SEG_OFF     0x241A0E

enum { M_DIGITAL, M_ANALOG, M_SEGMENTS, M_COUNT };
static const char *mode_names[] = { "Digital", "Analogue", "Segments" };
static const char *day_names[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *month_names[] = { "January", "February", "March", "April", "May", "June", "July",
                                     "August", "September", "October", "November", "December" };

#define FULL_W 460
#define FULL_H 470
#define SLIM_W 340
#define SLIM_H 112

/* ------------------------------------------------------------ the chip */
static int cmos(int reg)
{
    int v;
    outb(0x70, reg);
    v = inb(0x71);
    return (v >> 4) * 10 + (v & 0x0F);          /* it counts in BCD */
}

static int weekday(int d, int m, int y)         /* Zeller: 0 = Sunday */
{
    int k, j;
    if (m < 3) { m += 12; y--; }
    k = y % 100;
    j = y / 100;
    return (d + 13 * (m + 1) / 5 + k + k / 4 + j / 4 + 5 * j + 6) % 7;
}

void rtc_read(struct rtc_time *t)
{
    int century, n = 100000;
    outb(0x70, 0x0A);
    while (n-- && (inb(0x71) & 0x80)) ;         /* not while the chip updates */
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

/* ------------------------------------------------------------ state */
static int win_id = -1, mode = M_DIGITAL, slim;
static int last_sec = -1;
static int sw_running, alarm_on, alarm_h = 7, alarm_m = 0, ringing;
static unsigned sw_start, sw_accum, last_ring_ms;

static unsigned sw_elapsed(void) { return sw_accum + (sw_running ? now_ms() - sw_start : 0); }

/* sine in 6-degree steps, in 1024ths: enough for hands */
static const int sin6[16] = { 0, 107, 213, 316, 416, 512, 602, 685, 761, 828, 887, 935, 974, 1002, 1018, 1024 };
static int sin60(int k)                          /* k in 60ths of a turn */
{
    k = ((k % 60) + 60) % 60;
    if (k <= 15) return sin6[k];
    if (k <= 30) return sin6[30 - k];
    if (k <= 45) return -sin6[k - 30];
    return -sin6[60 - k];
}
static int cos60(int k) { return sin60(k + 15); }

static void button(int x, int y, int w, const char *label, int lit)
{
    vgradient(x, y, w, 30, lit ? 0x5A4218 : 0x2E2416, lit ? 0x3A2A12 : 0x1E170E);
    round_frame(x, y, w, 30, 4, lit ? AMBER : EDGE);
    text(F_SMALL, x + (w - text_width(F_SMALL, label)) / 2, y + (30 - text_height(F_SMALL)) / 2,
         label, lit ? AMBER_HOT : TEXT);
}

/* ------------------------------------------------------------ the faces */
static void face_digital(int x, int y, int w, const struct rtc_time *t, int with_date)
{
    char b[40];
    snprintf(b, sizeof b, "%02d:%02d:%02d", t->hour, t->min, t->sec);
    text(F_CLOCK, x + (w - text_width(F_CLOCK, b)) / 2, y, b, ringing ? WARM : AMBER_HOT);
    if (with_date) {
        snprintf(b, sizeof b, "%s, %d %s %d", day_names[t->wday], t->day, month_names[t->month - 1], t->year);
        text(F_NORMAL, x + (w - text_width(F_NORMAL, b)) / 2, y + text_height(F_CLOCK) + 6, b, TEXT_DIM);
    }
}

static void face_analog(int cx, int cy, int r, const struct rtc_time *t)
{
    int k, hk;
    soft_ellipse(cx, cy, r + 6, r + 6, 0x000000, 90);            /* the shadow it sits in */
    soft_ellipse(cx, cy, r, r, FACE, 255);
    for (k = 0; k < 60; k++) {                                   /* the ticks */
        int len = k % 5 ? 4 : 10, th = k % 5 ? 1 : 3;
        int x0 = cx + (r - 4) * sin60(k) / 1024, y0 = cy - (r - 4) * cos60(k) / 1024;
        int x1 = cx + (r - 4 - len) * sin60(k) / 1024, y1 = cy - (r - 4 - len) * cos60(k) / 1024;
        line(x0, y0, x1, y1, th, k % 5 ? TEXT_DIM : AMBER);
    }
    for (k = 0; k < 12; k++) {                                   /* the numbers */
        char b[4];
        int nx = cx + (r - 26) * sin60(k * 5) / 1024, ny = cy - (r - 26) * cos60(k * 5) / 1024;
        snprintf(b, sizeof b, "%d", k == 0 ? 12 : k);
        text(F_BOLD, nx - text_width(F_BOLD, b) / 2, ny - text_height(F_BOLD) / 2, b, TEXT);
    }
    hk = (t->hour % 12) * 5 + t->min / 12;                       /* the hour hand creeps */
    line(cx, cy, cx + (r * 52 / 100) * sin60(hk) / 1024, cy - (r * 52 / 100) * cos60(hk) / 1024, 7, TEXT);
    line(cx, cy, cx + (r * 78 / 100) * sin60(t->min) / 1024, cy - (r * 78 / 100) * cos60(t->min) / 1024, 5, TEXT);
    line(cx, cy, cx + (r * 84 / 100) * sin60(t->sec) / 1024, cy - (r * 84 / 100) * cos60(t->sec) / 1024, 2,
         ringing ? WARM : AMBER_HOT);
    soft_ellipse(cx, cy, 6, 6, AMBER_HOT, 255);
    soft_ellipse(cx, cy, 3, 3, FACE, 255);
}

/* one seven-segment digit, h tall, at x,y; on/off by bit */
static void seg_digit(int x, int y, int h, int d, uint32_t on)
{
    static const unsigned char segs[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    int th = h / 9, w = h * 55 / 100, half = h / 2, bits = d >= 0 && d <= 9 ? segs[d] : 0;
    uint32_t c;
    c = bits & 0x01 ? on : SEG_OFF; round_fill(x + th, y, w - 2 * th, th, th / 2, c);                        /* a: top */
    c = bits & 0x02 ? on : SEG_OFF; round_fill(x + w - th, y + th, th, half - th - th / 2, th / 2, c);      /* b */
    c = bits & 0x04 ? on : SEG_OFF; round_fill(x + w - th, y + half + th / 2, th, half - th - th / 2, th / 2, c); /* c */
    c = bits & 0x08 ? on : SEG_OFF; round_fill(x + th, y + h - th, w - 2 * th, th, th / 2, c);              /* d */
    c = bits & 0x10 ? on : SEG_OFF; round_fill(x, y + half + th / 2, th, half - th - th / 2, th / 2, c);    /* e */
    c = bits & 0x20 ? on : SEG_OFF; round_fill(x, y + th, th, half - th - th / 2, th / 2, c);              /* f */
    c = bits & 0x40 ? on : SEG_OFF; round_fill(x + th, y + half - th / 2, w - 2 * th, th, th / 2, c);       /* g */
}

static void face_segments(int x, int y, int w, int h, const struct rtc_time *t)
{
    int dw = h * 55 / 100, gap = h / 6, colon = h / 5;
    int total = 6 * dw + 4 * gap + 2 * colon, sx = x + (w - total) / 2, th = h / 9;
    uint32_t on = ringing ? WARM : AMBER_HOT;
    int digits[6] = { t->hour / 10, t->hour % 10, t->min / 10, t->min % 10, t->sec / 10, t->sec % 10 }, i;
    round_fill(x, y - 12, w, h + 24, 6, WELL);
    for (i = 0; i < 6; i++) {
        seg_digit(sx, y, h, digits[i], on);
        sx += dw + gap;
        if (i == 1 || i == 3) {
            if (t->sec & 1 || i == 3) {
                round_fill(sx - gap / 2 + colon / 2 - th / 2, y + h / 3 - th / 2, th, th, th / 2, on);
                round_fill(sx - gap / 2 + colon / 2 - th / 2, y + 2 * h / 3 - th / 2, th, th, th / 2, on);
            }
            sx += colon;
        }
    }
}

/* ------------------------------------------------------------ the window */
static void clock_draw(struct window *w)
{
    struct rtc_time t;
    char b[64];
    int x = w->x, y = w->y;
    unsigned el = sw_elapsed();

    rtc_read(&t);
    if (ringing) {
        round_fill(x + 8, y + 8, w->w - 16, slim ? w->h - 16 : 190, 8, 0x3A2010);
        glow(x + 8, y + 8, w->w - 16, slim ? w->h - 16 : 190, 8, WARM, 3);
    }

    if (slim) {
        if (mode == M_SEGMENTS) face_segments(x + 16, y + 26, w->w - 32, 48, &t);
        else face_digital(x, y + 18, w->w, &t, 0);
        button(x + w->w - 74, y + w->h - 40, 60, "Full", 0);
        if (ringing) button(x + 14, y + w->h - 40, 70, "Quiet", 1);
        return;
    }

    /* the face */
    switch (mode) {
    case M_ANALOG:   face_analog(x + w->w / 2, y + 108, 92, &t); break;
    case M_SEGMENTS: face_segments(x + 24, y + 70, w->w - 48, 80, &t); break;
    default:         face_digital(x, y + 40, w->w, &t, 1); break;
    }
    if (mode != M_DIGITAL) {
        snprintf(b, sizeof b, "%s, %d %s %d", day_names[t.wday], t.day, month_names[t.month - 1], t.year);
        text(F_SMALL, x + (w->w - text_width(F_SMALL, b)) / 2, y + 208, b, TEXT_DIM);
    }

    /* the stopwatch */
    y += 236;
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
    button(x + 120, y + 36, 30, "-", 0);  button(x + 154, y + 36, 30, "+", 0);
    button(x + 200, y + 36, 30, "-", 0);  button(x + 234, y + 36, 30, "+", 0);
    text(F_SMALL, x + 122, y + 70, "hour", TEXT_DIM);
    text(F_SMALL, x + 200, y + 70, "minute", TEXT_DIM);
    button(x + w->w - 100, y + 36, 80, ringing ? "Quiet" : alarm_on ? "On" : "Off", alarm_on);
    if (ringing) text(F_BOLD, x + w->w - 190, y + 74, "Alarm!", WARM);

    /* the modes */
    y += 96;
    fill(x + 16, y, w->w - 32, 1, EDGE);
    {
        int i, bw = (w->w - 32 - 3 * 8) / 4;
        for (i = 0; i < M_COUNT; i++)
            button(x + 16 + i * (bw + 8), y + 10, bw, mode_names[i], i == mode);
        button(x + 16 + 3 * (bw + 8), y + 10, bw, "Slim", 0);
    }
}

static void set_slim(int on)
{
    struct window *w = win_at(win_id);
    slim = on;
    w->w = on ? SLIM_W : FULL_W;
    w->h = on ? SLIM_H : FULL_H;
    if (w->x + w->w > scr_w) w->x = scr_w - w->w - 8;
    if (w->y + w->h > scr_h) w->y = scr_h - w->h - 8;
    damage_all();
}

static int clock_event(struct window *w, struct event *e)
{
    int x, y;
    if (e->type != EV_MOUSE_DOWN) return 0;
    x = e->a;
    y = e->b;
    if (slim) {
        if (y >= w->h - 40 && y < w->h - 10) {
            if (x >= w->w - 74) set_slim(0);
            else if (ringing && x >= 14 && x < 84) { ringing = 0; alarm_on = 0; }
        }
        return 1;
    }
    y -= 236;
    if (y >= 36 && y < 66) {                                     /* the stopwatch row */
        if (x >= w->w - 190 && x < w->w - 110) {
            if (sw_running) { sw_accum += now_ms() - sw_start; sw_running = 0; }
            else { sw_start = now_ms(); sw_running = 1; }
            return 1;
        }
        if (x >= w->w - 100 && x < w->w - 20) { sw_accum = 0; sw_running = 0; return 1; }
    }
    y -= 84;
    if (y >= 36 && y < 66) {                                     /* the alarm row */
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
    y -= 96;
    if (y >= 10 && y < 40) {                                     /* the modes */
        int bw = (w->w - 32 - 3 * 8) / 4, i = (x - 16) / (bw + 8);
        if (x >= 16 && i >= 0 && i < M_COUNT) mode = i;
        else if (i == 3) set_slim(1);
        return 1;
    }
    return 1;
}

void clock_closed(int id) { if (id == win_id) win_id = -1; }

void app_clock(void)
{
    if (win_id >= 0) return;
    win_id = win_open("Clock", slim ? SLIM_W : FULL_W, slim ? SLIM_H : FULL_H, clock_draw, clock_event);
}

/* from the main loop: 1 when the window should be repainted */
int clock_tick(void)
{
    struct rtc_time t;
    int changed = 0;
    rtc_read(&t);
    if (alarm_on && !ringing && t.hour == alarm_h && t.min == alarm_m && t.sec < 2) {
        ringing = 1;
        last_ring_ms = 0;
        if (win_id < 0) app_clock();
        changed = 1;
    }
    if (ringing && now_ms() - last_ring_ms > 3000) {           /* the chime, until quieted */
        music_chime();
        last_ring_ms = now_ms();
    }
    if (win_id < 0) return 0;
    if (t.sec != last_sec || sw_running) { last_sec = t.sec; changed = 1; }
    return changed;
}

int clock_window(void) { return win_id; }
