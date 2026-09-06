/* monitor.c - where the time goes.
 *
 * A small window that says, twice a second, how many frames were drawn,
 * how long each took to draw and to push to the card, how much went over
 * the bus, how much of the time the processor was idle, how full the
 * sound ring is, and what kind of memory the processor thinks the card's
 * framebuffer is.  The last one matters more than it sounds: writes to
 * "uncacheable" memory go out one at a time, and to "write-combining"
 * memory in bursts, several times faster.  That is the nearest thing to
 * graphics acceleration a plain framebuffer has.
 */
#include <nanolibc.h>
#include "nano.h"
#include "draw.h"
#include "input.h"
#include "shell.h"
#include "paudio.h"

#define TEXT      0xD8C8B0
#define TEXT_DIM  0x8A7C68
#define AMBER     0xF0A020
#define AMBER_HOT 0xFFC65A
#define EDGE      0x4A3618

static int win_id = -1;
static unsigned last_ms;
static struct shell_stats last;

/* what the last half second came to, as rates */
static unsigned fps, draw_ms10, present_ms10, mb_s, busy_pct, loops;
static const char *fb_type = "unknown";

static uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/* The memory type the processor applies to an address: the variable
   MTRRs first, then the default.  (The fixed ones only cover the first
   megabyte.) */
static const char *memory_type(uint64_t addr)
{
    static const char *names[] = { "uncacheable", "write-combining", "?", "?",
                                   "write-through", "write-protect", "write-back" };
    uint64_t cap = rdmsr(0xFE), def = rdmsr(0x2FF);
    int n = (int)(cap & 0xFF), i, type = -1;
    if (!(def & (1 << 11))) return "MTRRs off";
    for (i = 0; i < n; i++) {
        uint64_t base = rdmsr(0x200 + 2 * i), mask = rdmsr(0x201 + 2 * i);
        if (!(mask & (1 << 11))) continue;              /* not in use */
        if ((addr & mask & ~0xFFFull) == (base & mask & ~0xFFFull)) {
            int t = (int)(base & 0xFF);
            if (type < 0 || t == 0) type = t;           /* uncacheable wins */
        }
    }
    if (type < 0) type = (int)(def & 0xFF);
    return type >= 0 && type <= 6 ? names[type] : "?";
}

static void line(struct window *w, int row, const char *label, const char *value, uint32_t c)
{
    int y = w->y + 16 + row * 26;
    text(F_NORMAL, w->x + 20, y, label, TEXT_DIM);
    text(F_BOLD, w->x + 190, y, value, c);
}

static void monitor_draw(struct window *w)
{
    char b[48];
    snprintf(b, sizeof b, "%u a second", fps);
    line(w, 0, "Frames", b, AMBER_HOT);
    snprintf(b, sizeof b, "%u.%u ms each", draw_ms10 / 10, draw_ms10 % 10);
    line(w, 1, "Drawing", b, TEXT);
    snprintf(b, sizeof b, "%u.%u ms each, %u MB/s", present_ms10 / 10, present_ms10 % 10, mb_s);
    line(w, 2, "To the card", b, TEXT);
    snprintf(b, sizeof b, "%u%% busy, %u loops/s", busy_pct, loops);
    line(w, 3, "Processor", b, busy_pct > 80 ? AMBER : TEXT);
    snprintf(b, sizeof b, "%d%% full", audio_ring_fill());
    line(w, 4, "Sound ring", b, music_active() ? TEXT : TEXT_DIM);
    line(w, 5, "Framebuffer", fb_type, strcmp(fb_type, "write-combining") ? AMBER : TEXT);
    snprintf(b, sizeof b, "%dx%d, %d bits", scr_w, scr_h, draw_fb_bpp());
    line(w, 6, "Screen", b, TEXT_DIM);
    fill(w->x + 20, w->y + 16 + 7 * 26 + 4, w->w - 40, 1, EDGE);
    text(F_SMALL, w->x + 20, w->y + 16 + 7 * 26 + 12, "Refreshed twice a second while open.", TEXT_DIM);
}

void monitor_closed(int id)
{
    if (id == win_id) win_id = -1;
}

void app_monitor(void)
{
    if (win_id >= 0) return;
    fb_type = memory_type(draw_fb_phys());
    last = shell_stats;
    last_ms = now_ms();
    win_id = win_open("Monitor", 440, 256, monitor_draw, 0);
}

/* from the main loop: 1 when the window wants repainting */
int monitor_tick(void)
{
    unsigned now, dt;
    struct shell_stats s;
    if (win_id < 0) return 0;
    now = now_ms();
    dt = now - last_ms;
    if (dt < 500) return 0;
    s = shell_stats;
    {
        unsigned frames = s.frames - last.frames;
        unsigned long bytes = s.present_bytes - last.present_bytes;
        unsigned draw_us = s.draw_us - last.draw_us, present_us = s.present_us - last.present_us;
        unsigned idle_us = s.idle_us - last.idle_us;
        fps = frames * 1000 / dt;
        draw_ms10 = frames ? draw_us / frames / 100 : 0;
        present_ms10 = frames ? present_us / frames / 100 : 0;
        mb_s = (unsigned)(bytes / 1024 * 1000 / dt / 1024);
        busy_pct = 100 - (idle_us / 10) / dt;
        if (busy_pct > 100) busy_pct = 100;
        loops = (s.loops - last.loops) * 1000 / dt;
    }
    last = s;
    last_ms = now;
    return 1;
}

int monitor_window(void) { return win_id; }
