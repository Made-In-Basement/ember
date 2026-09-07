/* gpu.c - GPU.N32: a probe of the Intel graphics engine.
 *
 * The firmware sets a screen mode and hands us a framebuffer; the rest of
 * the chip sits idle.  This program asks, one careful step at a time, what
 * it would take to use it: where its registers are, how the display is set
 * up, whether the engines can be woken, whether we can map memory for them
 * to see, whether the blitter's command ring runs, and finally whether the
 * blitter will paint a rectangle onto the screen.
 *
 * Every section is written to \GPU.TXT before the next begins, so a lockup
 * leaves the evidence of how far it got.  Nothing here touches the display
 * configuration; the only writes are to the engine's own registers, to
 * unused page-table slots, and to pixels.
 *
 * Register offsets are those of the Broadwell (Gen8) programming manuals,
 * as also used by the Linux i915 driver.
 */
#include <nanolibc.h>
#include "nano.h"

/* ------------------------------------------------------------ the record */
static char out[32000];
static int out_n;
static int failed;                      /* a step said no: later ones stay away */

static void say(const char *fmt, ...)
{
    char line[200];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof line - 3, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (n > (int)sizeof line - 3) n = sizeof line - 3;
    line[n++] = '\r';
    line[n++] = '\n';
    line[n] = 0;
    sys_puts(line);
    if (out_n + n < (int)sizeof out) { memcpy(out + out_n, line, n); out_n += n; }
}

static void flush(void)
{
    int h = sys_create("\\GPU.TXT");
    if (h < 0) { sys_puts("(could not write GPU.TXT)\r\n"); return; }
    sys_write(h, out, out_n);
    sys_close(h);
}

/* ------------------------------------------------------------ the ports */
static inline void outl(uint16_t p, uint32_t v) { __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(p)); }
static inline uint32_t inl(uint16_t p) { uint32_t v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }

static uint32_t pci_read(int dev, int fn, int reg)
{
    outl(0xCF8, 0x80000000u | (dev << 11) | (fn << 8) | (reg & 0xFC));
    return inl(0xCFC);
}

/* ------------------------------------------------------------ the chip */
static volatile uint32_t *mmio;         /* the register block, BAR 0 */
static uint32_t bar0, bar2;             /* registers; the aperture */
static volatile uint64_t *ggtt;         /* the global graphics page table */
static uint32_t ggtt_bytes;             /* its size, from the GGC register */
static uint32_t stolen_base, stolen_bytes;

#define RD(o)     (mmio[(o) / 4])
#define WR(o, v)  (mmio[(o) / 4] = (v))

/* registers */
#define FORCEWAKE_MT        0xA188
#define FORCEWAKE_ACK       0x130044
#define GT_THREAD_STATUS    0x13805C
#define RC_CONTROL          0xA090
#define RC_STATE            0x20074
#define GFX_FLSH_CNTL       0x101008
#define RING_TAIL(b)        ((b) + 0x30)
#define RING_HEAD(b)        ((b) + 0x34)
#define RING_START(b)       ((b) + 0x38)
#define RING_CTL(b)         ((b) + 0x3C)
#define RING_ACTHD(b)       ((b) + 0x74)
#define RING_HWS(b)         ((b) + 0x80)
#define RING_MI_MODE(b)     ((b) + 0x9C)
#define RING_IMR(b)         ((b) + 0xA8)
#define RING_GFX_MODE(b)    ((b) + 0x29C)
#define RING_INSTDONE(b)    ((b) + 0x6C)
#define RCS                 0x2000
#define BCS                 0x22000
#define PIPE_CONF(p)        (0x70008 + (p) * 0x1000)
#define PIPE_SRC(p)         (0x6001C + (p) * 0x1000)
#define PLANE_CNTR(p)       (0x70180 + (p) * 0x1000)
#define PLANE_STRIDE(p)     (0x70188 + (p) * 0x1000)
#define PLANE_SURF(p)       (0x7019C + (p) * 0x1000)
#define PLANE_OFFSET(p)     (0x701A4 + (p) * 0x1000)
#define CUR_CNTR(p)         (0x70080 + (p) * 0x1000)
#define CUR_BASE(p)         (0x70084 + (p) * 0x1000)
#define CUR_POS(p)          (0x70088 + (p) * 0x1000)
#define PPAT_LO             0x40E0
#define PPAT_HI             0x40E4

/* things learned while the screen is graphic, told once text is back */
static char later[12][200];
static int nlater;
static void note(const char *fmt, ...)
{
    va_list ap;
    int n;
    if (nlater >= 12) return;
    va_start(ap, fmt);
    n = vsnprintf(later[nlater], 200 - 3, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (n > 200 - 3) n = 200 - 3;
    if (out_n + n + 2 < (int)sizeof out) {
        memcpy(out + out_n, later[nlater], n);
        out_n += n;
        out[out_n++] = '\r';
        out[out_n++] = '\n';
    }
    nlater++;
}

static int poll_until(uint32_t reg, uint32_t mask, uint32_t want, int loops)
{
    int i;
    for (i = 0; i < loops; i++)
        if ((RD(reg) & mask) == want) return i;
    return -1;
}

/* ------------------------------------------------------------ section 1: PCI */
static int find_device(void)
{
    uint32_t id = pci_read(2, 0, 0), cls = pci_read(2, 0, 8), cmd = pci_read(2, 0, 4);
    uint32_t b0lo = pci_read(2, 0, 0x10), b0hi = pci_read(2, 0, 0x14);
    uint32_t b2lo = pci_read(2, 0, 0x18), b2hi = pci_read(2, 0, 0x1C);
    uint32_t ggc = pci_read(2, 0, 0x50), bdsm = pci_read(2, 0, 0x5C);
    uint32_t gms = (ggc >> 8) & 0xFF, ggms = (ggc >> 6) & 3;

    say("== 1. the device at PCI 0:2.0");
    say("id %04X:%04X class %06X rev %u  command %04X (memory %s, bus master %s)",
        id & 0xFFFF, id >> 16, cls >> 8, cls & 0xFF, cmd & 0xFFFF,
        (cmd & 2) ? "on" : "OFF", (cmd & 4) ? "on" : "off");
    if ((id & 0xFFFF) != 0x8086 || (cls >> 24) != 0x03) {      /* base class 3: display */
        say("not Intel graphics: nothing more to try");
        return -1;
    }
    say("BAR0 (registers) %08X%08X  BAR2 (aperture) %08X%08X", b0hi, b0lo & ~0xFu, b2hi, b2lo & ~0xFu);
    if (b0hi || b2hi) { say("above 4 GB: this program cannot reach it"); return -1; }
    bar0 = b0lo & ~0xFu;
    bar2 = b2lo & ~0xFu;
    stolen_bytes = gms < 0xF0 ? gms * 32u * 1024 * 1024 : (gms - 0xF0 + 1) * 4u * 1024 * 1024;
    stolen_base = bdsm & 0xFFF00000u;
    ggtt_bytes = ggms ? 1u << (20 + ggms) : 0;
    say("GGC %08X: graphics memory %u MB stolen at %08X, GGTT %u MB (%u pages of address space)",
        ggc, stolen_bytes >> 20, stolen_base, ggtt_bytes >> 20, ggtt_bytes / 8);
    if (!(cmd & 2) || !ggtt_bytes) { say("memory decoding off or no GGTT: stopping"); return -1; }
    if ((id >> 16) == 0x1616 || (id >> 16) == 0x161E) say("that is HD Graphics 5300, Broadwell GT2: the one we expect");
    else say("device %04X is not the Broadwell GT2 this program was written against; going on carefully", id >> 16);
    mmio = (volatile uint32_t *)bar0;
    return 0;
}

/* ------------------------------------------------------------ section 2: display */
static int active_plane = -1;

static void read_display(const char *when)
{
    int p;
    say("== 2. the display, %s", when);
    active_plane = -1;
    for (p = 0; p < 3; p++) {
        uint32_t conf = RD(PIPE_CONF(p)), src = RD(PIPE_SRC(p));
        uint32_t cntr = RD(PLANE_CNTR(p)), stride = RD(PLANE_STRIDE(p));
        uint32_t surf = RD(PLANE_SURF(p)), off = RD(PLANE_OFFSET(p));
        say("pipe %c conf %08X (%s) source %ux%u", 'A' + p, conf, (conf >> 31) ? "on" : "off",
            (src >> 16) + 1, (src & 0xFFFF) + 1);
        say("plane %c cntr %08X (%s, format %u, %s) stride %u surface %08X offset %08X", 'A' + p,
            cntr, (cntr >> 31) ? "on" : "off", (cntr >> 26) & 0xF, (cntr & (1 << 10)) ? "tiled" : "linear",
            stride, surf & ~0xFFFu, off);
        if ((cntr >> 31) && active_plane < 0) active_plane = p;
    }
    if (active_plane < 0) say("no plane is on: the VGA path is drawing the text screen");
}

/* the GGTT sits in the second half of BAR0; try the plausible halves and
   keep the one whose first entry maps the stolen memory */
static int find_ggtt(void)
{
    uint32_t halves[3] = { 2u << 20, 4u << 20, 8u << 20 };
    int i, found = -1;
    say("== 3. the global graphics page table");
    for (i = 0; i < 3; i++) {
        volatile uint64_t *t = (volatile uint64_t *)(bar0 + halves[i]);
        uint64_t e0 = t[0], e1 = t[1];
        say("at BAR0 + %u MB: entry 0 = %08X%08X, entry 1 = %08X%08X", halves[i] >> 20,
            (uint32_t)(e0 >> 32), (uint32_t)e0, (uint32_t)(e1 >> 32), (uint32_t)e1);
        if (found < 0 && (e0 & 1) && ((uint32_t)e0 & 0xFFFFF000u) == stolen_base &&
            ((uint32_t)e1 & 0xFFFFF000u) == stolen_base + 4096)
            found = i;
    }
    if (found < 0) { say("no half maps the stolen memory at its entry 0: leaving the table alone"); return -1; }
    ggtt = (volatile uint64_t *)(bar0 + halves[found]);
    say("the table is at BAR0 + %u MB and maps the stolen memory from address 0, as expected", halves[found] >> 20);
    return 0;
}

/* ------------------------------------------------------------ section 4: forcewake */
static int wake(void)
{
    int n;
    uint32_t ack;
    say("== 4. waking the engines");
    say("before: forcewake ack %08X, thread status %08X, RC control %08X, RC state %08X",
        RD(FORCEWAKE_ACK), RD(GT_THREAD_STATUS), RD(RC_CONTROL), RD(RC_STATE));
    WR(FORCEWAKE_MT, (1u << 16) | 1u);              /* masked: set bit 0 */
    n = poll_until(FORCEWAKE_ACK, 1, 1, 2000000);
    ack = RD(FORCEWAKE_ACK);
    if (n < 0) { say("no acknowledgement (ack reads %08X): the render well would not wake", ack); return -1; }
    say("acknowledged after %d reads (ack %08X)", n, ack);
    n = poll_until(GT_THREAD_STATUS, 7, 0, 2000000);
    say("thread status %08X%s", RD(GT_THREAD_STATUS), n < 0 ? " (threads still busy)" : "");
    WR(RC_CONTROL, 0);                              /* no RC6 while we work */
    say("RC control now %08X", RD(RC_CONTROL));
    say("render  ring: head %08X tail %08X start %08X ctl %08X acthd %08X mi_mode %08X gfx_mode %08X",
        RD(RING_HEAD(RCS)), RD(RING_TAIL(RCS)), RD(RING_START(RCS)), RD(RING_CTL(RCS)),
        RD(RING_ACTHD(RCS)), RD(RING_MI_MODE(RCS)), RD(RING_GFX_MODE(RCS)));
    say("blitter ring: head %08X tail %08X start %08X ctl %08X acthd %08X mi_mode %08X gfx_mode %08X",
        RD(RING_HEAD(BCS)), RD(RING_TAIL(BCS)), RD(RING_START(BCS)), RD(RING_CTL(BCS)),
        RD(RING_ACTHD(BCS)), RD(RING_MI_MODE(BCS)), RD(RING_GFX_MODE(BCS)));
    return 0;
}

/* ------------------------------------------------------------ section 5: our pages */
static uint32_t *ring, *hws, *scratch;  /* one page each, CPU addresses */
static uint32_t ring_gpu, hws_gpu, scratch_gpu;
static uint32_t *tile, *cursor;         /* 64x64 ARGB each: four pages, contiguous */
static uint32_t tile_gpu, cursor_gpu;

static uint32_t page_aligned(int pages)
{
    uint32_t p = (uint32_t)malloc((pages + 1) * 4096);
    return (p + 4095) & ~4095u;
}

/* a page-table entry's low bits: present, writable, and a cache index from
   bits 3, 4 and 7.  The index picks an entry of the attribute table (PPAT),
   which we program: 3 = uncached, 4 = write-back and coherent with the LLC. */
#define PTE_UNCACHED  0x1Bu             /* index 3 */
#define PTE_CACHED    0x03u             /* index 0: what the firmware uses */
#define PTE_LLC       0x83u             /* index 4 */

static void map_page(uint32_t gpu, uint32_t phys, uint32_t flags)
{
    uint32_t idx = gpu >> 12;
    ggtt[idx] = (uint64_t)(phys | flags);
}

static int map_pages(void)
{
    uint32_t base, i, e;
    volatile uint32_t *window;
    say("== 5. mapping three pages of ours for the engines");
    ring = (uint32_t *)page_aligned(1);
    hws = (uint32_t *)page_aligned(1);
    scratch = (uint32_t *)page_aligned(1);
    tile = (uint32_t *)page_aligned(4);
    cursor = (uint32_t *)page_aligned(4);
    if (!ring || !hws || !scratch || !tile || !cursor) { say("no memory"); return -1; }
    memset(ring, 0, 4096);
    memset(hws, 0, 4096);
    for (i = 0; i < 1024; i++) scratch[i] = 0xC0DE0000u + i;
    cache_flush(scratch, 4096);

    /* a slot above the stolen memory and below the aperture's reach */
    base = stolen_bytes > (96u << 20) ? stolen_bytes : (96u << 20);
    base = (base + (16u << 20)) & ~0xFFFFFu;
    if ((base >> 12) + 3 > ggtt_bytes / 8) { say("the table is too small for a slot at %08X", base); return -1; }
    say("slot %08X: entries there read %08X%08X %08X%08X %08X%08X", base,
        (uint32_t)(ggtt[base >> 12] >> 32), (uint32_t)ggtt[base >> 12],
        (uint32_t)(ggtt[(base >> 12) + 1] >> 32), (uint32_t)ggtt[(base >> 12) + 1],
        (uint32_t)(ggtt[(base >> 12) + 2] >> 32), (uint32_t)ggtt[(base >> 12) + 2]);
    ring_gpu = base;
    hws_gpu = base + 4096;
    scratch_gpu = base + 8192;
    map_page(ring_gpu, (uint32_t)ring, PTE_UNCACHED);
    map_page(hws_gpu, (uint32_t)hws, PTE_UNCACHED);
    map_page(scratch_gpu, (uint32_t)scratch, PTE_UNCACHED);
    tile_gpu = base + 4 * 4096;                     /* cached: the coherency question */
    cursor_gpu = base + 8 * 4096;                   /* uncached: the display reads it */
    for (i = 0; i < 4; i++) {
        map_page(tile_gpu + i * 4096, (uint32_t)tile + i * 4096, PTE_LLC);
        map_page(cursor_gpu + i * 4096, (uint32_t)cursor + i * 4096, PTE_UNCACHED);
    }
    WR(GFX_FLSH_CNTL, 1);
    /* the attribute table: entry 3 becomes uncached (0), entry 4 write-back
       in the LLC (7); entries 0-2, which the firmware's mappings use, stay */
    say("cache attribute table: %08X %08X", RD(PPAT_LO), RD(PPAT_HI));
    WR(PPAT_LO, RD(PPAT_LO) & 0x00FFFFFFu);
    WR(PPAT_HI, (RD(PPAT_HI) & 0xFFFFFF00u) | 0x07u);
    say("programmed:            %08X %08X (entry 3 uncached, entry 4 write-back LLC)", RD(PPAT_LO), RD(PPAT_HI));
    e = (uint32_t)ggtt[scratch_gpu >> 12];
    say("wrote them: scratch entry reads back %08X (page %08X)", e, (uint32_t)scratch);

    /* read our scratch page back through the aperture: if it matches, the
       engines see memory through the table exactly as we mapped it */
    window = (volatile uint32_t *)(bar2 + scratch_gpu);
    say("through the aperture at %08X: %08X %08X %08X %08X", bar2 + scratch_gpu, window[0], window[1], window[2], window[1023]);
    if (window[0] != 0xC0DE0000u || window[1023] != 0xC0DE03FFu) { say("the aperture does not show our page: the mapping is wrong"); return -1; }
    say("the mapping works");
    return 0;
}

/* ------------------------------------------------------------ section 6: the ring */
static int start_ring(void)
{
    int n;
    uint32_t tail;
    say("== 6. the blitter's command ring");
    WR(RING_IMR(BCS), 0xFFFFFFFFu);                 /* no interrupts from it */
    WR(RING_MI_MODE(BCS), (1u << 24) | (1u << 8)); /* stop */
    n = poll_until(RING_MI_MODE(BCS), 1u << 9, 1u << 9, 200000);
    say("stopped: mi_mode %08X%s", RD(RING_MI_MODE(BCS)), n < 0 ? " (idle bit never rose)" : "");
    WR(RING_HWS(BCS), hws_gpu);
    WR(RING_HEAD(BCS), 0);
    WR(RING_TAIL(BCS), 0);
    WR(RING_START(BCS), ring_gpu);
    WR(RING_CTL(BCS), (0u << 12) | 1u);             /* one page, valid */
    say("programmed: start %08X ctl %08X head %08X tail %08X hws %08X",
        RD(RING_START(BCS)), RD(RING_CTL(BCS)), RD(RING_HEAD(BCS)), RD(RING_TAIL(BCS)), RD(RING_HWS(BCS)));
    if (!(RD(RING_CTL(BCS)) & 1)) { say("the ring would not become valid"); return -1; }
    WR(RING_MI_MODE(BCS), 1u << 24);                /* run */

    /* eight no-ops: does the head follow the tail? */
    memset(ring, 0, 64);
    cache_flush(ring, 64);
    tail = 32;
    WR(RING_TAIL(BCS), tail);
    n = poll_until(RING_HEAD(BCS), 0x1FFFFC, tail, 2000000);
    say("after 8 no-ops: head %08X tail %08X acthd %08X instdone %08X (%s, %d reads)",
        RD(RING_HEAD(BCS)), RD(RING_TAIL(BCS)), RD(RING_ACTHD(BCS)), RD(RING_INSTDONE(BCS)),
        n < 0 ? "did NOT follow" : "followed", n);
    return n < 0 ? -1 : 0;
}

/* ------------------------------------------------------------ section 7: a rectangle */
static int pick_mode(struct vbe_mode *m)
{
    struct vbe_info info;
    int i, best = -1, best_w = 0;
    if (sys_vbe_info(&info) != 0) return -1;
    for (i = 0; i < info.mode_count; i++) {
        struct vbe_mode t;
        if (sys_vbe_mode(info.modes[i], &t) != 0 || !t.framebuffer || t.bpp != 32) continue;
        if (t.width < 800) continue;
        if (t.width > best_w) { best_w = t.width; best = info.modes[i]; *m = t; }   /* the widest: what Ember runs */
    }
    return best;
}

/* ------------------------------------------------------------ section 8: a copy from our memory */
/* The desktop would draw into ordinary cached memory and have the blitter
   copy from there.  Does the blitter see what the processor just wrote,
   without a cache flush?  Two copies of a checkerboard, the second after
   changing it, read back through the framebuffer. */
static void fill_tile(int phase)
{
    int x, y;
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++)
            tile[y * 64 + x] = (((x >> 3) + (y >> 3) + phase) & 1) ? 0xFFF0A020u : 0xFF102040u;
}

static uint32_t copy_tail;

static int copy_once(uint32_t surf, uint32_t stride, int dx, int dy)   /* surf: the destination mapping */
{
    uint32_t *cmd = ring + copy_tail / 4;
    int n;
    cmd[0] = (2u << 29) | (0x53u << 22) | (3u << 20) | 8u;    /* XY_SRC_COPY_BLT */
    cmd[1] = (3u << 24) | (0xCCu << 16) | (stride & 0xFFFF);  /* 32bpp, ROP SRCCOPY */
    cmd[2] = ((uint32_t)dy << 16) | (uint32_t)dx;
    cmd[3] = ((uint32_t)(dy + 64) << 16) | (uint32_t)(dx + 64);
    cmd[4] = surf;
    cmd[5] = 0;
    cmd[6] = 0;                                     /* source from (0,0) */
    cmd[7] = 256;                                   /* source pitch */
    cmd[8] = tile_gpu;
    cmd[9] = 0;
    cmd[10] = (0x26u << 23) | 2u;                   /* MI_FLUSH_DW */
    cmd[11] = 0;
    cmd[12] = 0;
    cmd[13] = 0;
    cache_flush(cmd, 14 * 4);
    copy_tail += 14 * 4;
    WR(RING_TAIL(BCS), copy_tail);
    n = poll_until(RING_HEAD(BCS), 0x1FFFFC, copy_tail, 4000000);
    return n;
}

static void copy_tests(uint32_t surf, uint32_t stride, uint32_t tail, volatile uint32_t *fb, uint32_t pitch)
{
    int n, good, x, y;
    copy_tail = tail;

    /* first: the tile written, not flushed, copied to (600,400) */
    fill_tile(0);
    n = copy_once(surf, stride, 600, 400);
    for (good = 1, y = 0; y < 64; y += 9)
        for (x = 0; x < 64; x += 7)
            if ((fb[(400 + y) * (pitch / 4) + 600 + x] & 0xFFFFFF) != (tile[y * 64 + x] & 0xFFFFFF)) good = 0;
    note("copy 1 from LLC-cached memory, unflushed: %s (%s)", good ? "every sample matched" : "samples DIFFERED",
         n < 0 ? "head did not reach the tail" : "ring ran");

    /* second: the tile changed in place, still not flushed, copied to (700,400) */
    fill_tile(1);
    n = copy_once(surf, stride, 700, 400);
    for (good = 1, y = 0; y < 64; y += 9)
        for (x = 0; x < 64; x += 7)
            if ((fb[(400 + y) * (pitch / 4) + 700 + x] & 0xFFFFFF) != (tile[y * 64 + x] & 0xFFFFFF)) good = 0;
    note("copy 2 after changing it, unflushed: %s (%s)", good ? "every sample matched: coherent" : "samples DIFFERED: a flush is needed",
         n < 0 ? "head did not reach the tail" : "ring ran");

    /* third: flushed, for the record */
    fill_tile(0);
    cache_flush(tile, 64 * 64 * 4);
    n = copy_once(surf, stride, 800, 400);
    for (good = 1, y = 0; y < 64; y += 9)
        for (x = 0; x < 64; x += 7)
            if ((fb[(400 + y) * (pitch / 4) + 800 + x] & 0xFFFFFF) != (tile[y * 64 + x] & 0xFFFFFF)) good = 0;
    note("copy 3 flushed: %s (%s)", good ? "every sample matched" : "samples DIFFERED",
         n < 0 ? "head did not reach the tail" : "ring ran");
}

/* ------------------------------------------------------------ section 9: the hardware cursor */
static void show_cursor(void)
{
    int x, y;
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++) {
            int dx = x - 32, dy = y - 32, d2 = dx * dx + dy * dy;
            uint32_t c = 0;
            if (d2 < 26 * 26) c = 0xFFF0A020u;                  /* an amber disc */
            else if (d2 < 30 * 30) c = 0xFF1A140Du;             /* with a dark rim */
            if (d2 < 10 * 10) c = 0xFF1A140Du;                  /* and a hole */
            cursor[y * 64 + x] = c;
        }
    cache_flush(cursor, 64 * 64 * 4);
    note("cursor: cntr was %08X base %08X pos %08X", RD(CUR_CNTR(active_plane)), RD(CUR_BASE(active_plane)), RD(CUR_POS(active_plane)));
    WR(CUR_CNTR(active_plane), 0x27);               /* 64x64, ARGB */
    WR(CUR_POS(active_plane), (900u << 16) | 1500u);
    WR(CUR_BASE(active_plane), cursor_gpu);         /* this write shows it */
    note("cursor: set cntr %08X base %08X pos %08X: a ringed amber disc should sit right of centre",
         RD(CUR_CNTR(active_plane)), RD(CUR_BASE(active_plane)), RD(CUR_POS(active_plane)));
}

/* how many of a grid of 800 samples inside a rectangle are amber, as seen
   through the given view of the screen */
static int count_amber(volatile uint32_t *view, uint32_t pitch, int y0)
{
    int x, y, hits = 0;
    for (y = 0; y < 200; y += 10)
        for (x = 0; x < 400; x += 10)
            if ((view[(y0 + y) * (pitch / 4) + 100 + x] & 0xFFFFFF) == 0xF0A020) hits++;
    return hits;
}

static void wbinvd(void) { __asm__ volatile("wbinvd" ::: "memory"); }

/* the commands: a 400x200 amber fill at (100,y) through the given mapping;
   a flush, with extra bits if wanted.  Each returns the dwords it wrote. */
static int fill_cmd(uint32_t *c, uint32_t dst, uint32_t stride, uint32_t tiled, int y)
{
    c[0] = (2u << 29) | (0x50u << 22) | (3u << 20) | 5u;    /* XY_COLOR_BLT */
    c[1] = (3u << 24) | (0xF0u << 16) | (tiled ? 1u << 11 : 0) | (stride & 0xFFFF);
    c[2] = ((uint32_t)y << 16) | 100u;
    c[3] = ((uint32_t)(y + 200) << 16) | 500u;
    c[4] = dst;
    c[5] = 0;
    c[6] = 0xFFF0A020u;
    return 7;
}

static int flush_cmd(uint32_t *c, uint32_t extra)
{
    c[0] = (0x26u << 23) | 2u | extra;                      /* MI_FLUSH_DW */
    c[1] = 0;
    c[2] = 0;
    c[3] = 0;
    return 4;
}

/* ndw dwords were written at the tail: hand them over, wait for the head */
static int run_ring(uint32_t *tail, int ndw)
{
    cache_flush(ring, 4096);
    *tail += (uint32_t)ndw * 4;
    WR(RING_TAIL(BCS), *tail);
    return poll_until(RING_HEAD(BCS), 0x1FFFFC, *tail, 4000000);
}

static void moment(void)
{
    int k;
    for (k = 0; k < 3000000; k++) (void)RD(RING_HEAD(BCS));
}

static int blit(void)
{
    struct vbe_mode m;
    int mode = pick_mode(&m), n, r, k;
    uint32_t surf, stride, tiled, tail = 32, *c, phys, alias = 0x08000000u, ppat0;
    volatile uint32_t *fb;
    say("== 7. three stages of rectangles; a look after each");
    if (mode < 0) { say("no 32-bit linear mode 800 or wider"); return -1; }
    say("setting mode %04X: %ux%u, %u bpp, pitch %u, framebuffer %08X", mode, m.width, m.height, m.bpp, m.pitch, m.framebuffer);
    if (sys_set_vbe_mode(mode, 1) != 0) { say("the mode would not set"); return -1; }
    read_display("in the graphics mode");
    if (active_plane < 0) { sys_set_video_mode(3); say("no plane on: cannot find the surface"); return -1; }
    surf = RD(PLANE_SURF(active_plane)) & ~0xFFFu;
    stride = RD(PLANE_STRIDE(active_plane));
    tiled = RD(PLANE_CNTR(active_plane)) & (1 << 10);
    phys = (uint32_t)ggtt[surf >> 12] & 0xFFFFF000u;
    fb = (volatile uint32_t *)m.framebuffer;
    say("the surface: GPU address %08X, physical %08X, aperture %08X", surf, phys, m.framebuffer);

    /* the screen's pages mapped again, with cache index 3 */
    {
        uint32_t npages = (m.pitch * m.height + 4095) / 4096, i;
        for (i = 0; i < npages; i++)
            ggtt[(alias >> 12) + i] = (ggtt[(surf >> 12) + i] & ~0xFFFull) | PTE_UNCACHED;
        WR(GFX_FLSH_CNTL, 1);
    }
    show_cursor();

    /* stage A: entry 0 of the attribute table, the one every firmware
       mapping names, made uncached; two fills */
    ppat0 = RD(PPAT_LO);
    WR(PPAT_LO, ppat0 & 0xFFFFFF00u);
    note("stage A: table %08X %08X (entry 0 uncached); fills at y=100 through the firmware mapping, y=400 through ours",
         RD(PPAT_LO), RD(PPAT_HI));
    c = ring + tail / 4;
    n = fill_cmd(c, surf, stride, tiled, 100);
    n += fill_cmd(c + n, alias, stride, tiled, 400);
    n += flush_cmd(c + n, 0);
    r = run_ring(&tail, n);
    moment();
    note("stage A: ring %s; through the aperture y=100: %d/800, y=400: %d/800", r < 0 ? "STALLED" : "ran",
         count_amber(fb, m.pitch, 100), count_amber(fb, m.pitch, 400));
    flush();
    sys_getkey();                                   /* look 1 */

    /* stage B: entry 0 write-back again; one fill, then the flush command
       with its LLC bit */
    WR(PPAT_LO, ppat0);
    note("stage B: table %08X (entry 0 write-back again); a fill at y=700 through the firmware mapping, then MI_FLUSH_DW with bit 9",
         RD(PPAT_LO));
    c = ring + tail / 4;
    n = fill_cmd(c, surf, stride, tiled, 700);
    n += flush_cmd(c + n, 1u << 9);
    c[n++] = 0;                                     /* MI_NOOP: an even count */
    r = run_ring(&tail, n);
    moment();
    note("stage B: ring %s; through the aperture y=700: %d/800", r < 0 ? "STALLED" : "ran", count_amber(fb, m.pitch, 700));
    flush();
    sys_getkey();                                   /* look 2 */

    /* stage C: the processor writes back every cache */
    wbinvd();
    note("stage C: wbinvd");
    flush();
    sys_getkey();                                   /* look 3 */

    WR(CUR_CNTR(active_plane), 0);
    WR(CUR_BASE(active_plane), 0);
    sys_set_video_mode(3);
    for (k = 0; k < nlater; k++) { sys_puts(later[k]); sys_puts("\r\n"); }
    say("after it all: head %08X tail %08X acthd %08X", RD(RING_HEAD(BCS)), RD(RING_TAIL(BCS)), RD(RING_ACTHD(BCS)));
    return 0;
}

static void stop_ring(void)
{
    WR(RING_MI_MODE(BCS), (1u << 24) | (1u << 8));
    WR(RING_CTL(BCS), 0);
    WR(FORCEWAKE_MT, 1u << 16);                     /* let it sleep again */
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    say("GPU probe: the Intel graphics engine, step by step");
    if (find_device() != 0) { flush(); return 0; }
    flush();
    read_display("as the firmware left it");
    flush();
    if (find_ggtt() != 0) { flush(); return 0; }
    flush();
    if (wake() != 0) { flush(); return 0; }
    flush();
    if (map_pages() != 0) { stop_ring(); flush(); return 0; }
    flush();
    if (start_ring() != 0) { stop_ring(); flush(); return 0; }
    flush();
    say("(the screen goes graphic now; press any key when you have seen it)");
    blit();
    stop_ring();
    say("done");
    flush();
    return 0;
}
