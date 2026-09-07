/* monitor.c - the machine at a glance.
 *
 * Gauges for the processor (how busy, how fast, how warm), bars for
 * memory and the battery, a strip of the display's numbers, and a
 * history behind each gauge so a spike is still there to be seen a
 * minute later.  Everything is read from the processor and the firmware
 * directly: the busy figure from the main loop's own accounting, the
 * clock from the processor's cycle counters, the temperature from its
 * thermal sensor, the battery from the firmware's power services if it
 * has them, memory from the firmware's map.
 *
 * The one line that is more than decoration is the framebuffer's memory
 * type: "uncacheable" against "write-combining" was a tenfold difference
 * in how fast anything reached the screen.
 */
#include <nanolibc.h>
#include "nano.h"
#include "draw.h"
#include "gpu.h"
#include "input.h"
#include "shell.h"
#include "paudio.h"
#include "power.h"

#define TEXT      0xD8C8B0
#define TEXT_DIM  0x8A7C68
#define AMBER     0xF0A020
#define AMBER_HOT 0xFFC65A
#define AMBER_DIM 0x6A4E20
#define EDGE      0x4A3618
#define WELL      0x100C08
#define CARD      0x1A140E
#define GOOD      0x9BD27A
#define PANEL     0x140F0A
#define WARM      0xF0602A

#define HIST      96                    /* samples kept: 48 seconds at two a second */
#define WIN_W     660
#define WIN_H     426
#define SMALL_W   300                   /* the compact view: a strip for the side of the screen */
#define SMALL_H   188

static int win_id = -1;
static unsigned last_ms;
static struct shell_stats last;

/* what the last half second came to */
static unsigned fps, draw_ms10, present_ms10, mb_s, busy_pct, loops;
static const char *fb_type = "unknown";

/* the machine */
static int have_dts, tjmax = 100, temp_c = -1;
static int have_aperf, mhz;
static uint64_t last_aperf, last_mperf;
static int ram_mb = -1, apm_ok = -1, bat_pct = -1, bat_ac = -1, bat_state = -1;
static unsigned last_bat_ms;
static int busy_hist[HIST], temp_hist[HIST], hist_n;

/* ------------------------------------------------------------ the processor */
static uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static void cpuid(uint32_t leaf, uint32_t *a, uint32_t *c, uint32_t *d)
{
    uint32_t b;
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

/* The memory type the processor applies to an address: the variable
   MTRRs first, then the default. */
static const char *memory_type(uint64_t addr)
{
    static const char *names[] = { "uncacheable", "write-combining", "?", "?",
                                   "write-through", "write-protect", "write-back" };
    uint32_t a, c, d;
    uint64_t cap, def;
    int n, i, type = -1;
    cpuid(1, &a, &c, &d);
    if (!(d & (1 << 12))) return "no MTRRs";
    cap = rdmsr(0xFE);
    def = rdmsr(0x2FF);
    n = (int)(cap & 0xFF);
    if (!(def & (1 << 11))) return "MTRRs off";
    for (i = 0; i < n; i++) {
        uint64_t base = rdmsr(0x200 + 2 * i), mask = rdmsr(0x201 + 2 * i);
        if (!(mask & (1 << 11))) continue;
        if ((addr & mask & ~0xFFFull) == (base & mask & ~0xFFFull)) {
            int t = (int)(base & 0xFF);
            if (type < 0 || t == 0) type = t;           /* uncacheable wins */
        }
    }
    if (type < 0) type = (int)(def & 0xFF);
    return type >= 0 && type <= 6 ? names[type] : "?";
}

static void probe_processor(void)
{
    uint32_t a, c, d, max;
    cpuid(0, &max, &c, &d);
    have_dts = have_aperf = 0;
    if (max >= 6) {
        cpuid(6, &a, &c, &d);
        have_dts = a & 1;                       /* the digital thermal sensor */
        have_aperf = c & 1;                     /* APERF and MPERF */
    }
    if (have_dts) {
        int t = (int)((rdmsr(0x1A2) >> 16) & 0xFF);     /* MSR_TEMPERATURE_TARGET */
        if (t >= 50 && t <= 120) tjmax = t;
    }
    if (have_aperf) {
        last_aperf = rdmsr(0xE8);
        last_mperf = rdmsr(0xE7);
    }
}

static void sample_processor(void)
{
    if (have_dts) {
        uint64_t s = rdmsr(0x19C);              /* IA32_THERM_STATUS */
        if (s & (1u << 31)) temp_c = tjmax - (int)((s >> 16) & 0x7F);
    }
    if (have_aperf) {
        uint64_t ap = rdmsr(0xE8), mp = rdmsr(0xE7);
        uint64_t da = ap - last_aperf, dm = mp - last_mperf;
        last_aperf = ap;
        last_mperf = mp;
        if (dm) mhz = (int)((uint64_t)tsc_mhz() * da / dm);
    }
}

/* ------------------------------------------------------------ the firmware */
static void probe_memory(void)
{
    struct rmcall r;
    memset(&r, 0, sizeof r);
    r.ax = 0xE801;                              /* memory size, two ranges */
    r.intno = 0x15;
    sys_bios(&r);
    if (!(r.flags & 1)) {
        unsigned kb_low = r.ax ? r.ax : r.cx;   /* 1 MB to 16 MB, in KB */
        unsigned blk = r.bx ? r.bx : r.dx;      /* above 16 MB, in 64 KB blocks */
        ram_mb = 1 + kb_low / 1024 + blk / 16;
    }
}

/* the Advanced Power Management services, where a laptop's firmware
   still offers them */
static void probe_battery(void)
{
    struct rmcall r;
    memset(&r, 0, sizeof r);
    r.ax = 0x5300;                              /* installation check */
    r.intno = 0x15;
    sys_bios(&r);
    if ((r.flags & 1) || r.bx != 0x504D) { apm_ok = 0; return; }
    memset(&r, 0, sizeof r);
    r.ax = 0x5301;                              /* connect, real mode */
    r.intno = 0x15;
    sys_bios(&r);                               /* "already connected" is fine too */
    apm_ok = 1;
}

static void sample_battery(void)
{
    struct rmcall r;
    if (apm_ok <= 0) return;
    memset(&r, 0, sizeof r);
    r.ax = 0x530A;                              /* power status */
    r.bx = 0x0001;                              /* all devices */
    r.intno = 0x15;
    sys_bios(&r);
    if (r.flags & 1) { apm_ok = 0; return; }
    bat_ac = r.bx >> 8;                         /* 0 off, 1 on, 2 backup, FF unknown */
    bat_state = r.bx & 0xFF;                    /* 0 high, 1 low, 2 critical, 3 charging */
    bat_pct = (r.cx & 0xFF) == 0xFF ? -1 : (int)(r.cx & 0xFF);
}

/* ------------------------------------------------------------ drawing */
/* degrees, 0 to the right and counter-clockwise, from a small polynomial
   good to about a degree: all a gauge needs */
static int iatan2(int y, int x)
{
    int ax = x < 0 ? -x : x, ay = y < 0 ? -y : y, a;
    if (ax == 0 && ay == 0) return 0;
    if (ay <= ax) {
        int t = ay * 256 / ax;
        a = 45 * t / 256 + 146 * t * (256 - t) / 655360;
    } else {
        int t = ax * 256 / ay;
        a = 90 - (45 * t / 256 + 146 * t * (256 - t) / 655360);
    }
    if (x < 0) a = 180 - a;
    if (y < 0) a = 360 - a;
    return a % 360;
}

/* A three-quarter ring from lower left round to lower right, lit as far
   as value/max.  Drawn pixel by pixel, which for a gauge this size is a
   few thousand and no trouble. */
static void gauge(int cx, int cy, int r, int th, int value, int max, uint32_t lit)
{
    int dx, dy;
    int frac = (max > 0 && value > 0) ? (value > max ? max : value) * 270 / max : -1;
    for (dy = -r; dy <= r; dy++)
        for (dx = -r; dx <= r; dx++) {
            int d2 = dx * dx + dy * dy, ang, g;
            if (d2 > r * r || d2 < (r - th) * (r - th)) continue;
            ang = iatan2(-dy, dx);
            g = (225 - ang + 720) % 360;        /* 0 at the start of the sweep */
            if (g > 270) continue;              /* the gap at the bottom */
            if (g <= frac) {
                int edge = (d2 > (r - 1) * (r - 1) || d2 < (r - th + 1) * (r - th + 1));
                pixel_blend(cx + dx, cy + dy, g > frac - 6 ? AMBER_HOT : lit, edge ? 150 : 255);
            } else {
                pixel_blend(cx + dx, cy + dy, 0x000000, 120);
            }
        }
    damage(cx - r, cy - r, 2 * r + 1, 2 * r + 1);
}

/* the last HIST samples as an area, newest at the right */
static void history(int x, int y, int w, int h, const int *v, int max, uint32_t c)
{
    int i;
    round_fill(x, y, w, h, 4, WELL);
    for (i = 0; i < hist_n && i < HIST; i++) {
        int s = v[(hist_n - 1 - i) % HIST];
        int col = x + w - 2 - i * 2, bh;
        if (col < x + 1) break;
        if (s < 0) continue;
        bh = (s > max ? max : s) * (h - 6) / max;
        if (bh < 1) bh = 1;
        fill_alpha(col, y + h - 3 - bh, 2, bh, c, 110);
        fill(col, y + h - 3 - bh, 2, 1, AMBER_HOT);
    }
    round_frame(x, y, w, h, 4, EDGE);
}

static void card(int x, int y, int w, int h, const char *title)
{
    round_fill(x, y, w, h, 6, CARD);
    round_frame(x, y, w, h, 6, EDGE);
    text(F_BOLD, x + 14, y + 8, title, TEXT_DIM);
}

static void bar(int x, int y, int w, int pct, uint32_t c)
{
    int lit = pct < 0 ? 0 : pct > 100 ? w : w * pct / 100;
    round_fill(x, y, w, 14, 4, WELL);
    if (lit > 0) {
        round_fill(x, y, lit, 14, 4, c);
        glow(x, y, lit, 14, 4, c, 2);
    }
    round_frame(x, y, w, 14, 4, EDGE);
}

/* F, or a click, moves between the full dashboard and the compact strip */
static int monitor_event(struct window *w, struct event *e)
{
    int ch = e->type == EV_KEY ? e->b : 0;
    if (ch == 'f' || ch == 'F' || ch == 'c' || ch == 'C' ||
        (e->type == EV_MOUSE_DOWN && e->dbl)) {
        int to_small = w->w > SMALL_W + 60;
        w->w = to_small ? SMALL_W : WIN_W;
        w->h = to_small ? SMALL_H : WIN_H;
        if (to_small) {                             /* out of the way, at the right */
            w->x = scr_w - SMALL_W - 24;
            w->y = 70;
        }
        return 1;
    }
    return 0;
}

/* the compact view: the two readings that matter, and their history */
static void monitor_small(struct window *w)
{
    int x = w->x, y = w->y, iw = w->w - 20, i;
    char b[64];
    fill(x, y, w->w, w->h, PANEL);
    /* the processor */
    text(F_SMALL, x + 10, y + 8, "Processor", TEXT_DIM);
    snprintf(b, sizeof b, "%u%%", busy_pct);
    text(F_BOLD, x + w->w - 12 - text_width(F_BOLD, b), y + 6, b, busy_pct > 80 ? WARM : AMBER);
    history(x + 10, y + 28, iw, 44, busy_hist, 100, AMBER);
    /* the memory */
    text(F_SMALL, x + 10, y + 80, "Memory", TEXT_DIM);
    if (ram_mb > 0) {
        int used = ((scr_w * scr_h * 4) * 3 + 0x80000 + 0x200000) >> 20;
        snprintf(b, sizeof b, "%d of %d MB", used, ram_mb);
        text(F_SMALL, x + w->w - 12 - text_width(F_SMALL, b), y + 80, b, TEXT);
        bar(x + 10, y + 100, iw, ram_mb > 0 ? used * 100 / ram_mb : 0, GOOD);
    }
    /* the temperature, when the processor will say */
    if (temp_c > 0) {
        snprintf(b, sizeof b, "%d C", temp_c);
        text(F_SMALL, x + 10, y + 120, "Temperature", TEXT_DIM);
        text(F_SMALL, x + w->w - 12 - text_width(F_SMALL, b), y + 120, b, temp_c > 80 ? WARM : TEXT);
        history(x + 10, y + 138, iw, 30, temp_hist, 100, temp_c > 80 ? WARM : GOOD);
    } else {
        snprintf(b, sizeof b, "%u fps   %u.%u ms", fps, present_ms10 / 10, present_ms10 % 10);
        text(F_SMALL, x + 10, y + 124, b, TEXT_DIM);
    }
    for (i = 0; i < 1; i++) fill(x, y + w->h - 22, w->w, 1, EDGE);
    text(F_SMALL, x + 10, y + w->h - 18, "Full view: F", TEXT_DIM);
}

static void monitor_draw(struct window *w)
{
    if (w->w <= SMALL_W + 60) { monitor_small(w); return; }   /* small window, small view */
    char b[96];
    int x = w->x, y = w->y;
    int cw = (WIN_W - 42) / 2, ch = 190;
    int cx, cy, gx, gw;

    /* ---- the processor ---- */
    card(x + 14, y + 14, cw, ch, "Processor");
    cx = x + 14 + 70;
    cy = y + 14 + 100;
    gauge(cx, cy, 52, 12, busy_pct, 100, AMBER);
    snprintf(b, sizeof b, "%u%%", busy_pct);
    text(F_TITLE, cx - text_width(F_TITLE, b) / 2, cy - text_height(F_TITLE) / 2 - 4, b, AMBER_HOT);
    text(F_SMALL, cx - text_width(F_SMALL, "busy") / 2, cy + 14, "busy", TEXT_DIM);
    if (have_aperf && mhz > 0) snprintf(b, sizeof b, "%d MHz now, %u nominal", mhz, tsc_mhz());
    else snprintf(b, sizeof b, "%u MHz", tsc_mhz());
    text(F_SMALL, x + 14 + 14, y + 14 + ch - 26, b, TEXT);
    gx = cx + 66;
    gw = x + 14 + cw - 14 - gx;
    history(gx, y + 14 + 36, gw, 100, busy_hist, 100, AMBER);
    text(F_SMALL, gx, y + 14 + 140, "the last 48 seconds", TEXT_DIM);

    /* ---- the temperature ---- */
    card(x + 28 + cw, y + 14, cw, ch, "Temperature");
    cx = x + 28 + cw + 70;
    gx = cx + 66;
    if (have_dts && temp_c > 0) {
        uint32_t c = temp_c >= 85 ? WARM : temp_c >= 70 ? AMBER : GOOD;
        gauge(cx, cy, 52, 12, temp_c, tjmax, c);
        snprintf(b, sizeof b, "%d C", temp_c);
        text(F_TITLE, cx - text_width(F_TITLE, b) / 2, cy - text_height(F_TITLE) / 2 - 4, b, AMBER_HOT);
        text(F_SMALL, cx - text_width(F_SMALL, "celsius") / 2, cy + 14, "celsius", TEXT_DIM);
        snprintf(b, sizeof b, "throttles at %d C", tjmax);
        text(F_SMALL, x + 28 + cw + 14, y + 14 + ch - 26, b, TEXT);
        history(gx, y + 14 + 36, gw, 100, temp_hist, tjmax, c);
        text(F_SMALL, gx, y + 14 + 140, "the last 48 seconds", TEXT_DIM);
    } else {
        gauge(cx, cy, 52, 12, 0, 100, AMBER);
        text(F_SMALL, cx - text_width(F_SMALL, "no sensor") / 2, cy - 6, "no sensor", TEXT_DIM);
    }

    /* ---- memory ---- */
    y += 14 + ch + 14;
    card(x + 14, y, cw, 92, "Memory");
    {
        int used_mb = ((scr_w * scr_h * 4) * 3 + 0x80000 + 0x200000) >> 20;   /* our buffers and program */
        int pct = ram_mb > 0 ? used_mb * 100 / ram_mb : 0;
        bar(x + 28, y + 40, cw - 28, pct < 2 ? 2 : pct, AMBER);
        if (ram_mb > 0)
            snprintf(b, sizeof b, "%d MB usable by a 32-bit system; Ember holds %d MB", ram_mb, used_mb);
        else
            snprintf(b, sizeof b, "the firmware did not say; Ember holds %d MB", used_mb);
        text(F_SMALL, x + 28, y + 62, b, TEXT);
    }

    /* ---- the battery ---- */
    card(x + 28 + cw, y, cw, 92, "Battery");
    if (power_known() && power_percent() >= 0) {
        int pct = power_percent();
        bar(x + 42 + cw, y + 40, cw - 28, pct, pct <= 15 ? WARM : GOOD);
        snprintf(b, sizeof b, "%d%%, %s%s; %d cycles", pct,
                 power_on_mains() == 1 ? "on mains" : power_on_mains() == 0 ? "on battery" : "",
                 power_charging() == 1 ? ", charging" : power_on_mains() == 0 ? ", discharging" : "",
                 power_cycles());
        text(F_SMALL, x + 42 + cw, y + 62, b, TEXT);
    } else if (apm_ok > 0 && bat_pct >= 0) {
        bar(x + 42 + cw, y + 40, cw - 28, bat_pct, bat_pct <= 15 ? WARM : GOOD);
            snprintf(b, sizeof b, "%d%%, %s%s", bat_pct,
                     bat_ac == 1 ? "on mains" : bat_ac == 0 ? "on battery" : "power unknown",
                     bat_state == 3 ? ", charging" : bat_state == 2 ? ", critical" : "");
        text(F_SMALL, x + 42 + cw, y + 62, b, TEXT);
    } else {
        bar(x + 42 + cw, y + 40, cw - 28, 0, GOOD);
        text(F_SMALL, x + 42 + cw, y + 62,
             "no battery, or none the controller will show", TEXT_DIM);
    }

    /* ---- the display ---- */
    y += 92 + 14;
    card(x + 14, y, WIN_W - 28, 88, "Display");
    snprintf(b, sizeof b, "%u frames a second   %u.%u ms drawing   %u.%u ms to the screen   %u MB/s   %dx%d",
             fps, draw_ms10 / 10, draw_ms10 % 10, present_ms10 / 10, present_ms10 % 10,
             mb_s, scr_w, scr_h);
    text(F_SMALL, x + 28, y + 36, b, TEXT);
    if (draw_direct())
        snprintf(b, sizeof b, "%s; %d%% of the loop idle, %u passes a second, sound ring %d%% full",
                 gpu_note(), 100 - (int)busy_pct, loops, audio_ring_fill());
    else
        snprintf(b, sizeof b, "framebuffer %s; %d%% of the loop idle, %u passes a second, sound ring %d%% full",
                 fb_type, 100 - (int)busy_pct, loops, audio_ring_fill());
    text(F_SMALL, x + 28, y + 58, b, draw_direct() || !strcmp(fb_type, "write-combining") ? TEXT_DIM : AMBER);
}

void monitor_closed(int id)
{
    if (id == win_id) win_id = -1;
}

void app_monitor(void)
{
    if (win_id >= 0) return;
    fb_type = memory_type(draw_fb_phys());
    probe_processor();
    if (ram_mb < 0) probe_memory();
    if (apm_ok < 0) probe_battery();
    sample_battery();
    sample_processor();
    last = shell_stats;
    last_ms = now_ms();
    win_id = win_open("Monitor", WIN_W, WIN_H, monitor_draw, monitor_event);
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
    sample_processor();
    if (now - last_bat_ms > 5000) {
        sample_battery();
        last_bat_ms = now;
    }
    busy_hist[hist_n % HIST] = (int)busy_pct;
    temp_hist[hist_n % HIST] = temp_c;
    hist_n++;
    last = s;
    last_ms = now;
    return 1;
}

int monitor_window(void) { return win_id; }
