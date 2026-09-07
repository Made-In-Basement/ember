/* render.c - RENDER.N32: the first triangle from the 3D engine.
 *
 * Everything the earlier probes proved is reused: the page table at
 * BAR0 + 8 MB, forcewake, cache attribute entry 0 made uncached so what
 * the engines write lands in memory, a screen of our own scanned out by
 * the display.  New here is the render engine: its command ring is
 * started, and a batch buffer carries the whole 3D pipeline set-up that a
 * textured triangle needs - the same sequence, packet for packet, that the
 * Intel GPU tools use to copy a texture on Broadwell, with their
 * pre-assembled pixel shader - followed by one 3DPRIMITIVE.  The vertex
 * shader is off and the viewport transform is off, so the corners are
 * given in screen pixels and the engine does the rasterising, the
 * perspective interpolation and the texture sampling.
 *
 * If the engine never finishes, the fault registers are written down and
 * the engine is reset, so the machine still comes back to the prompt.
 * RENDER.TXT is rewritten after every section.
 */
#include <nanolibc.h>
#include "nano.h"

/* ------------------------------------------------------------ the record */
static char out[32000];
static int out_n;

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

static char later[24][200];             /* said once the screen is text again */
static int nlater;
static void note(const char *fmt, ...)
{
    va_list ap;
    int n;
    if (nlater >= 24) return;
    va_start(ap, fmt);
    n = vsnprintf(later[nlater], 200 - 3, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (n > 200 - 3) n = 200 - 3;
    if (out_n + n + 2 < (int)sizeof out) {
        memcpy(out + out_n, later[nlater], n);
        out_n += n;
        out[out_n++] = 13;
        out[out_n++] = 10;
    }
    nlater++;
}

static void flush(void)
{
    int h = sys_create("\\RENDER.TXT");
    if (h < 0) { sys_puts("(could not write RENDER.TXT)\r\n"); return; }
    sys_write(h, out, out_n);
    sys_close(h);
}

/* ------------------------------------------------------------ ports, time */
static inline void outl(uint16_t p, uint32_t v) { __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(p)); }
static inline uint32_t inl(uint16_t p) { uint32_t v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static void wbinvd(void) { __asm__ volatile("wbinvd" ::: "memory"); }
static void mfence(void) { __asm__ volatile("mfence" ::: "memory"); }

static uint32_t pci_read(int reg)
{
    outl(0xCF8, 0x80000000u | (2 << 11) | (reg & 0xFC));
    return inl(0xCFC);
}

static uint64_t tsc_hz;
static uint64_t rdtsc(void) { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo; }

static void clock_start(void)
{
    uint8_t p61 = inb(0x61);
    uint64_t t0, t1;
    unsigned guard = 0;
    outb(0x61, (uint8_t)((p61 & ~0x02) | 0x01));
    outb(0x43, 0xB0);
    outb(0x42, 0xFF);
    outb(0x42, 0xFF);
    t0 = rdtsc();
    while (!(inb(0x61) & 0x20))
        if (++guard > 200000000u) break;
    t1 = rdtsc();
    outb(0x61, (uint8_t)(p61 & ~0x03));
    if (guard <= 200000000u && t1 - t0 >= 100000)
        tsc_hz = (t1 - t0) * 1193182u / 65535u;
    else
        tsc_hz = 1400000000u;               /* a guess; only the timeouts use it */
}

static unsigned us_since(uint64_t t0) { return (unsigned)((rdtsc() - t0) / (tsc_hz / 1000000u)); }

/* ------------------------------------------------------------ the chip */
static volatile uint32_t *mmio;
static volatile uint64_t *ggtt;
static uint32_t bar0, bar2, ggtt_entries, stolen_base;

#define RD(o)     (mmio[(o) / 4])
#define WR(o, v)  (mmio[(o) / 4] = (v))

#define FORCEWAKE_MT        0xA188
#define FORCEWAKE_ACK       0x130044
#define RC_CONTROL          0xA090
#define GFX_FLSH_CNTL       0x101008
#define PPAT_LO             0x40E0
#define PPAT_HI             0x40E4
#define GDRST               0x941C
#define RP_STATE_CAP        0x140000    /* rp0 [7:0], rp1 [15:8], rpn [23:16], in 50 MHz */
#define RPNSWREQ            0xA008      /* the frequency asked for: ratio << 24 on Broadwell */
#define RP_INTERRUPT_LIMITS 0xA014
#define RPSTAT1             0xA01C      /* the frequency running: [13:7] */
#define RP_CONTROL          0xA024
#define RCS                 0x2000
#define RING_TAIL(b)        ((b) + 0x30)
#define RING_HEAD(b)        ((b) + 0x34)
#define RING_START(b)       ((b) + 0x38)
#define RING_CTL(b)         ((b) + 0x3C)
#define RING_IPEIR(b)       ((b) + 0x64)
#define RING_IPEHR(b)       ((b) + 0x68)
#define RING_INSTDONE(b)    ((b) + 0x6C)
#define RING_ACTHD(b)       ((b) + 0x74)
#define RING_HWS(b)         ((b) + 0x80)
#define RING_MI_MODE(b)     ((b) + 0x9C)
#define RING_IMR(b)         ((b) + 0xA8)
#define RING_EIR(b)         ((b) + 0xB0)
#define RING_ESR(b)         ((b) + 0xB8)
#define RING_RESET_CTL(b)   ((b) + 0xD0)
#define RING_GFX_MODE(b)    ((b) + 0x29C)
#define PLANE_CNTR(p)       (0x70180 + (p) * 0x1000)
#define PLANE_STRIDE(p)     (0x70188 + (p) * 0x1000)
#define PLANE_SURF(p)       (0x7019C + (p) * 0x1000)
#define PLANE_SURFLIVE(p)   (0x701AC + (p) * 0x1000)

static int poll_until(uint32_t reg, uint32_t mask, uint32_t want, int loops)
{
    int i;
    for (i = 0; i < loops; i++)
        if ((RD(reg) & mask) == want) return i;
    return -1;
}

/* ------------------------------------------------------------ sections 1-3 */
static int find_device(void)
{
    uint32_t id = pci_read(0), cls = pci_read(8), cmd = pci_read(4);
    uint32_t b0lo = pci_read(0x10), b0hi = pci_read(0x14), b2lo = pci_read(0x18);
    uint32_t ggc = pci_read(0x50), bdsm = pci_read(0x5C), ggms = (ggc >> 6) & 3;
    uint32_t halves[3] = { 2u << 20, 4u << 20, 8u << 20 };
    int i;
    say("== 1. the device");
    say("id %04X:%04X class %06X command %04X", id & 0xFFFF, id >> 16, cls >> 8, cmd & 0xFFFF);
    if ((id & 0xFFFF) != 0x8086 || (cls >> 24) != 0x03) { say("not Intel graphics"); return -1; }
    if (b0hi || !(cmd & 2) || !ggms) { say("unreachable or no page table"); return -1; }
    bar0 = b0lo & ~0xFu;
    bar2 = b2lo & ~0xFu;
    stolen_base = bdsm & 0xFFF00000u;
    ggtt_entries = (1u << (20 + ggms)) / 8;
    mmio = (volatile uint32_t *)bar0;
    for (i = 0; i < 3 && !ggtt; i++) {
        volatile uint64_t *t = (volatile uint64_t *)(bar0 + halves[i]);
        if ((t[0] & 1) && ((uint32_t)t[0] & 0xFFFFF000u) == stolen_base &&
            ((uint32_t)t[1] & 0xFFFFF000u) == stolen_base + 4096) ggtt = t;
    }
    if (!ggtt) { say("page table not where expected"); return -1; }
    say("registers %08X, aperture %08X, table at BAR0 + %u MB with %u entries", bar0, bar2, halves[i - 1] >> 20, ggtt_entries);
    return 0;
}

static int active_plane = -1;
static void find_plane(void)
{
    int p;
    active_plane = -1;
    for (p = 0; p < 3 && active_plane < 0; p++)
        if (RD(PLANE_CNTR(p)) >> 31) active_plane = p;
}

/* ------------------------------------------------------------ section 4: awake, uncached */
static int wake(void)
{
    int n;
    say("== 2. the engines awake, and their writes uncached");
    WR(FORCEWAKE_MT, (1u << 16) | 1u);
    n = poll_until(FORCEWAKE_ACK, 1, 1, 2000000);
    if (n < 0) { say("no forcewake acknowledgement"); return -1; }
    WR(RC_CONTROL, 0);
    say("attribute table was %08X %08X", RD(PPAT_LO), RD(PPAT_HI));
    WR(PPAT_LO, RD(PPAT_LO) & 0xFFFFFF00u);         /* entry 0: uncached */
    say("attribute table now %08X %08X (entry 0 uncached)", RD(PPAT_LO), RD(PPAT_HI));
    say("clock: capabilities %08X (max %u MHz, min %u MHz), running %u MHz", RD(RP_STATE_CAP),
        (RD(RP_STATE_CAP) & 0xFF) * 50, ((RD(RP_STATE_CAP) >> 16) & 0xFF) * 50, ((RD(RPSTAT1) >> 7) & 0x7F) * 50);
    say("render ring: head %08X tail %08X ctl %08X mi_mode %08X gfx_mode %08X eir %08X",
        RD(RING_HEAD(RCS)), RD(RING_TAIL(RCS)), RD(RING_CTL(RCS)), RD(RING_MI_MODE(RCS)), RD(RING_GFX_MODE(RCS)), RD(RING_EIR(RCS)));
    return 0;
}

/* ------------------------------------------------------------ section 5: memory */
static uint32_t *ring, *hws, *scratch, *batch, *tex, *screen;
static uint32_t ring_gpu, hws_gpu, scratch_gpu, batch_gpu, tex_gpu, screen_gpu = 0x10000000u;
static int scr_w, scr_h, scr_p, npages;

#define PTE_FLAGS 0x03u                 /* present, writable; the cache index is not honoured */
#define TEX_W 64
#define TEX_H 64

static uint32_t page_aligned(int pages)
{
    uint32_t p = (uint32_t)malloc((pages + 1) * 4096);
    return p ? (p + 4095) & ~4095u : 0;
}

static void map_pages(uint32_t gpu, uint32_t phys, int pages)
{
    int i;
    for (i = 0; i < pages; i++) ggtt[(gpu >> 12) + i] = (uint64_t)((phys + i * 4096) | PTE_FLAGS);
}

static void texture(void)               /* an amber and slate checker with a warm gradient */
{
    int x, y;
    for (y = 0; y < TEX_H; y++)
        for (x = 0; x < TEX_W; x++) {
            int check = ((x >> 3) + (y >> 3)) & 1;
            uint32_t r = check ? 0xF0 - y : 0x30 + y / 2, g = check ? 0xA0 - y / 2 : 0x28 + y / 3, b = check ? 0x20 : 0x40 + x / 2;
            tex[y * TEX_W + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
            if (y >= 60) {                          /* the bottom rows: backgrounds, one a phase */
                static const uint32_t dark[4] = { 0xFF1A1410u, 0xFF0E1A12u, 0xFF101828u, 0xFF000000u };
                tex[y * TEX_W + x] = dark[y - 60];
            }
        }
    cache_flush(tex, TEX_W * TEX_H * 4);
    mfence();
}

static void scene(void)                 /* the screen: dark, with a frame and a floor */
{
    int x, y;
    for (y = 0; y < scr_h; y++) {
        int t = y * 255 / scr_h;
        uint32_t c = ((0x30 - 0x20 * t / 255) << 16) | ((0x24 - 0x18 * t / 255) << 8) | (0x14 - 0x0A * t / 255);
        uint32_t *row = screen + y * scr_p;
        for (x = 0; x < scr_w; x++) row[x] = c;
    }
    for (x = 0; x < scr_w; x++) { screen[8 * scr_p + x] = 0xF0A020; screen[(scr_h - 9) * scr_p + x] = 0xF0A020; }
    for (y = 0; y < scr_h; y++) { screen[y * scr_p + 8] = 0xF0A020; screen[y * scr_p + scr_w - 9] = 0xF0A020; }
    cache_flush(screen, (uint32_t)npages * 4096);
    mfence();
}

static int get_memory(void)
{
    uint32_t base = 0x07000000u;
    say("== 3. memory for the ring, the batch, the texture and a screen of our own");
    ring = (uint32_t *)page_aligned(1);
    hws = (uint32_t *)page_aligned(1);
    scratch = (uint32_t *)page_aligned(1);
    batch = (uint32_t *)page_aligned(4);
    tex = (uint32_t *)page_aligned(4);
    if (!ring || !hws || !scratch || !batch || !tex) { say("no memory"); return -1; }
    memset(ring, 0, 4096);
    memset(hws, 0, 4096);
    memset(scratch, 0, 4096);
    memset(batch, 0, 4 * 4096);
    texture();
    cache_flush(ring, 4096); cache_flush(hws, 4096); cache_flush(scratch, 4096); cache_flush(batch, 4 * 4096);
    ring_gpu = base;
    hws_gpu = base + 0x1000;
    scratch_gpu = base + 0x2000;
    batch_gpu = base + 0x10000;
    tex_gpu = base + 0x20000;
    map_pages(ring_gpu, (uint32_t)ring, 1);
    map_pages(hws_gpu, (uint32_t)hws, 1);
    map_pages(scratch_gpu, (uint32_t)scratch, 1);
    map_pages(batch_gpu, (uint32_t)batch, 4);
    map_pages(tex_gpu, (uint32_t)tex, 4);
    WR(GFX_FLSH_CNTL, 1);
    say("ring %08X hws %08X scratch %08X batch %08X texture %08X (GPU addresses)", ring_gpu, hws_gpu, scratch_gpu, batch_gpu, tex_gpu);
    return 0;
}

/* ------------------------------------------------------------ section 6: the render ring */
static int start_ring(void)
{
    int n;
    uint32_t tail;
    say("== 4. the render engine's command ring");
    WR(RING_IMR(RCS), 0xFFFFFFFFu);
    WR(RING_MI_MODE(RCS), (1u << 24) | (1u << 8));  /* stop */
    n = poll_until(RING_MI_MODE(RCS), 1u << 9, 1u << 9, 200000);
    WR(RING_HWS(RCS), hws_gpu);
    WR(RING_HEAD(RCS), 0);
    WR(RING_TAIL(RCS), 0);
    WR(RING_START(RCS), ring_gpu);
    WR(RING_CTL(RCS), 1u);                           /* one page, valid */
    if (!(RD(RING_CTL(RCS)) & 1)) { say("the ring would not become valid (ctl %08X)", RD(RING_CTL(RCS))); return -1; }
    WR(RING_MI_MODE(RCS), 1u << 24);                 /* run */
    memset(ring, 0, 64);                             /* eight no-ops */
    cache_flush(ring, 64);
    mfence();
    tail = 32;
    WR(RING_TAIL(RCS), tail);
    n = poll_until(RING_HEAD(RCS), 0x1FFFFC, tail, 2000000);
    say("eight no-ops: head %08X tail %08X (%s)", RD(RING_HEAD(RCS)), RD(RING_TAIL(RCS)), n < 0 ? "did NOT follow" : "followed");
    return n < 0 ? -1 : 0;
}

/* ------------------------------------------------------------ section 7: the batch */
/* offsets within the batch's four pages; the state lives in the second */
#define OFF_BT       0x1000              /* binding table: two pointers */
#define OFF_SS_RT    0x1040              /* surface state: the render target */
#define OFF_SS_TEX   0x1080              /* surface state: the texture */
#define OFF_SAMPLER  0x10C0
#define OFF_CC       0x1100
#define OFF_BLEND    0x1140              /* 33 dwords */
#define OFF_CCVP     0x11C0
#define OFF_SFVP     0x1200
#define OFF_SCISSOR  0x1240
#define OFF_KERNEL   0x1280              /* 64 bytes */
#define OFF_VERTS    0x1300              /* 3 x 12 bytes */

#define GEN(pipe, op, sub) ((3u << 29) | ((pipe) << 27) | ((op) << 24) | ((sub) << 16))

static const uint32_t ps_kernel[4][4] = {  /* igt's lib/i915/shaders/ps/blit.g7a, assembled for gen8 */
    { 0x0080005a, 0x2f403ae8, 0x3a0000c0, 0x008d0040 },
    { 0x0080005a, 0x2f803ae8, 0x3a0000d0, 0x008d0040 },
    { 0x02800031, 0x2e203a48, 0x0e8d0f40, 0x08840001 },
    { 0x05800031, 0x20003a40, 0x0e8d0e20, 0x90031000 },
};

static void surface_state(uint32_t *ss, uint32_t gpu, int w, int h, int pitch)
{
    memset(ss, 0, 64);
    ss[0] = (1u << 29) | (0x0C0u << 18) | (1u << 16) | (1u << 14) | (1u << 8);   /* 2D, B8G8R8A8_UNORM, align 4, linear, read-write cache */
    ss[1] = 0x18u << 24;                                                          /* MOCS: cache as the page table says */
    ss[2] = ((uint32_t)(h - 1) << 16) | (uint32_t)(w - 1);
    ss[3] = (uint32_t)(pitch - 1);
    ss[7] = (4u << 25) | (5u << 22) | (6u << 19) | (7u << 16);                    /* channels straight through */
    ss[8] = gpu;
    ss[9] = 0;
}

static uint32_t *cmd;                   /* where the next command goes */
static void emit(uint32_t v) { *cmd++ = v; }
static void emit_zeros(int n) { while (n--) emit(0); }

/* the state that never changes: the binding table, the sampler, colour
   calc and blend, the viewports, the scissor, and the pixel shader itself.
   Written once; the cube's frames rewrite only surfaces and vertices. */
static void build_state(void)
{
    uint8_t *b = (uint8_t *)batch;
    uint32_t *p;
    int i;
    union { float f; uint32_t u; } fl;
    p = (uint32_t *)(b + OFF_BT);
    p[0] = OFF_SS_RT;
    p[1] = OFF_SS_TEX;
    p = (uint32_t *)(b + OFF_SAMPLER);              /* nearest, clamp */
    p[0] = 0; p[1] = 0; p[2] = 0;
    p[3] = (2u << 6) | (2u << 3) | 2u;
    memset(b + OFF_CC, 0, 24);
    p = (uint32_t *)(b + OFF_BLEND);
    p[0] = 0;
    for (i = 0; i < 16; i++) {                      /* source ONE, destination ZERO, add; pre-blend clamp */
        p[1 + i * 2] = (1u << 26) | (0x11u << 21);
        p[2 + i * 2] = 2u;
    }
    p = (uint32_t *)(b + OFF_CCVP);
    fl.f = -1.0e35f; p[0] = fl.u;
    fl.f = 1.0e35f; p[1] = fl.u;
    p = (uint32_t *)(b + OFF_SFVP);
    memset(p, 0, 64);
    fl.f = 1.0f;
    p[9] = fl.u;                                    /* guardband x max */
    p[11] = fl.u;                                   /* guardband y max */
    memset(b + OFF_SCISSOR, 0, 8);
    memcpy(b + OFF_KERNEL, ps_kernel, sizeof ps_kernel);
}

static uint32_t build_batch(int x0, int y0, int x1, int y1, int x2, int y2)
{
    uint8_t *b = (uint8_t *)batch;
    uint32_t *p;
    int i;
    union { float f; uint32_t u; } fl;

    /* ---- the state ---- */
    p = (uint32_t *)(b + OFF_BT);
    p[0] = OFF_SS_RT;
    p[1] = OFF_SS_TEX;
    surface_state((uint32_t *)(b + OFF_SS_RT), screen_gpu, scr_w, scr_h, scr_p * 4);
    surface_state((uint32_t *)(b + OFF_SS_TEX), tex_gpu, TEX_W, TEX_H, TEX_W * 4);
    p = (uint32_t *)(b + OFF_SAMPLER);              /* nearest, clamp */
    p[0] = 0; p[1] = 0; p[2] = 0;
    p[3] = (2u << 6) | (2u << 3) | 2u;
    memset(b + OFF_CC, 0, 24);
    p = (uint32_t *)(b + OFF_BLEND);
    p[0] = 0;
    for (i = 0; i < 16; i++) {                      /* source ONE, destination ZERO, add; pre-blend clamp */
        p[1 + i * 2] = (1u << 26) | (0x11u << 21);
        p[2 + i * 2] = 2u;
    }
    p = (uint32_t *)(b + OFF_CCVP);
    fl.f = -1.0e35f; p[0] = fl.u;
    fl.f = 1.0e35f; p[1] = fl.u;
    p = (uint32_t *)(b + OFF_SFVP);
    memset(p, 0, 64);
    fl.f = 1.0f;
    p[9] = fl.u;                                    /* guardband x max */
    p[11] = fl.u;                                   /* guardband y max */
    memset(b + OFF_SCISSOR, 0, 8);
    memcpy(b + OFF_KERNEL, ps_kernel, sizeof ps_kernel);
    p = (uint32_t *)(b + OFF_VERTS);                /* x,y as two shorts; u,v as floats */
    p[0] = ((uint32_t)(uint16_t)y0 << 16) | (uint16_t)x0; fl.f = 0.0f; p[1] = fl.u; fl.f = 0.0f; p[2] = fl.u;
    p[3] = ((uint32_t)(uint16_t)y1 << 16) | (uint16_t)x1; fl.f = 1.0f; p[4] = fl.u; fl.f = 0.0f; p[5] = fl.u;
    p[6] = ((uint32_t)(uint16_t)y2 << 16) | (uint16_t)x2; fl.f = 0.5f; p[7] = fl.u; fl.f = 1.0f; p[8] = fl.u;

    /* ---- the commands, from the start of the batch ---- */
    cmd = batch;
    emit(GEN(1, 1, 4));                             /* PIPELINE_SELECT: 3D */
    emit(GEN(0, 1, 2) | 1); emit_zeros(2);          /* STATE_SIP */
    emit(GEN(3, 1, 0x12)); emit(0);                 /* push constant allocations, all empty */
    emit(GEN(3, 1, 0x13)); emit(0);
    emit(GEN(3, 1, 0x14)); emit(0);
    emit(GEN(3, 1, 0x15)); emit(0);
    emit(GEN(3, 1, 0x16)); emit(0);
    emit(GEN(0, 1, 1) | 14);                        /* STATE_BASE_ADDRESS */
    emit(1); emit(0);                               /* general: 0, modify */
    emit(1);                                        /* stateless data port */
    emit(batch_gpu | 1); emit(0);                   /* surface state base: the batch */
    emit(batch_gpu | 1); emit(0);                   /* dynamic state base */
    emit(0); emit(0);                               /* indirect: untouched */
    emit(batch_gpu | 1); emit(0);                   /* instruction base */
    emit(0xFFFFF000u | 1);                          /* general size: all */
    emit((4u << 12) | 1);                           /* dynamic: four pages */
    emit(0xFFFFF000u | 1);
    emit((4u << 12) | 1);                           /* instruction: four pages */
    emit(GEN(3, 0, 0x23)); emit(OFF_CCVP);          /* viewport pointers */
    emit(GEN(3, 0, 0x21)); emit(OFF_SFVP);
    emit(GEN(3, 0, 0x30)); emit(64u | (1u << 16) | (2u << 25));   /* URB: VS 64 entries of 2, from 2 */
    emit(GEN(3, 0, 0x33)); emit(2u << 25);          /* GS, HS, DS: none */
    emit(GEN(3, 0, 0x31)); emit(2u << 25);
    emit(GEN(3, 0, 0x32)); emit(2u << 25);
    emit(GEN(3, 0, 0x24)); emit(OFF_BLEND | 1);     /* blend state, colour calc state */
    emit(GEN(3, 0, 0x0E)); emit(OFF_CC | 1);
    emit(GEN(3, 0, 0x0D)); emit(0);                 /* multisample: one */
    emit(GEN(3, 0, 0x18)); emit(1);                 /* sample mask */
    /* the stages we do not use, told so */
    emit(GEN(3, 0, 0x52) | 3); emit_zeros(4);       /* WM_HZ_OP */
    emit(GEN(3, 0, 0x19) | 9); emit_zeros(10);      /* CONSTANT_HS */
    emit(GEN(3, 0, 0x1B) | 7); emit_zeros(8);       /* HS */
    emit(GEN(3, 0, 0x27)); emit(0);                 /* binding table HS */
    emit(GEN(3, 0, 0x2C)); emit(0);                 /* sampler HS */
    emit(GEN(3, 0, 0x1C) | 2); emit_zeros(3);       /* TE */
    emit(GEN(3, 0, 0x16) | 9); emit_zeros(10);      /* CONSTANT_GS */
    emit(GEN(3, 0, 0x11) | 8); emit_zeros(9);       /* GS */
    emit(GEN(3, 0, 0x29)); emit(0);
    emit(GEN(3, 0, 0x2E)); emit(0);
    emit(GEN(3, 0, 0x1A) | 9); emit_zeros(10);      /* CONSTANT_DS */
    emit(GEN(3, 0, 0x1D) | 7); emit_zeros(8);       /* DS */
    emit(GEN(3, 0, 0x28)); emit(0);
    emit(GEN(3, 0, 0x2D)); emit(0);
    emit(GEN(3, 0, 0x26)); emit(0);                 /* binding table VS */
    emit(GEN(3, 0, 0x2B)); emit(0);                 /* sampler VS */
    emit(GEN(3, 0, 0x15) | 9); emit_zeros(10);      /* CONSTANT_VS */
    emit(GEN(3, 0, 0x10) | 7); emit_zeros(8);       /* VS: off; the corners pass straight through */
    emit(GEN(3, 0, 0x1E) | 3); emit_zeros(4);       /* STREAMOUT */
    emit(GEN(3, 0, 0x12) | 2); emit_zeros(3);       /* CLIP: off */
    emit(GEN(3, 0, 0x1F) | 2);                      /* SBE: one attribute, read one from offset one */
    emit((1u << 22) | (1u << 29) | (1u << 28) | (1u << 11) | (1u << 5));
    emit(0); emit(0);
    emit(GEN(3, 0, 0x51) | 9); emit_zeros(10);      /* SBE_SWIZ */
    emit(GEN(3, 0, 0x50) | 3);                      /* RASTER: cull none, front CCW */
    emit((1u << 21) | (1u << 16)); emit_zeros(3);
    emit(GEN(3, 0, 0x13) | 2); emit_zeros(3);       /* SF: no viewport transform */
    emit(GEN(3, 0, 0x2A)); emit(OFF_BT);            /* binding table PS */
    emit(GEN(3, 0, 0x2F)); emit(OFF_SAMPLER);       /* sampler PS */
    emit(GEN(3, 0, 0x14)); emit(1u << 11);          /* WM: perspective pixel barycentric */
    emit(GEN(3, 0, 0x17) | 9); emit_zeros(10);      /* CONSTANT_PS */
    emit(GEN(3, 0, 0x20) | 10);                     /* PS */
    emit(OFF_KERNEL); emit(0);
    emit((1u << 27) | (2u << 18));                  /* one sampler, two binding table entries */
    emit(0); emit(0);                               /* no scratch */
    emit((62u << 23) | (1u << 1));                  /* 63 threads, 16-pixel dispatch */
    emit(6u << 16);                                 /* setup data from GRF 6 */
    emit_zeros(4);
    emit(GEN(3, 0, 0x4D)); emit(1u << 30);          /* PS_BLEND: a writeable target */
    emit(GEN(3, 0, 0x4F)); emit((1u << 31) | (1u << 8));   /* PS_EXTRA: valid, attributes on */
    emit(GEN(3, 0, 0x0F)); emit(OFF_SCISSOR);       /* scissor pointer */
    emit(GEN(3, 0, 0x4E) | 1); emit_zeros(2);       /* WM_DEPTH_STENCIL: off */
    emit(GEN(3, 0, 0x05) | 6); emit_zeros(7);       /* DEPTH_BUFFER: none */
    emit(GEN(3, 0, 0x07) | 3); emit_zeros(4);       /* HIER_DEPTH */
    emit(GEN(3, 0, 0x06) | 3); emit_zeros(4);       /* STENCIL */
    emit(GEN(3, 0, 0x04) | 1); emit(0); emit(1);    /* CLEAR_PARAMS */
    emit(GEN(3, 1, 0) | 2);                         /* DRAWING_RECTANGLE: the screen */
    emit(0); emit(((uint32_t)(scr_h - 1) << 16) | (uint32_t)(scr_w - 1)); emit(0);
    emit(GEN(3, 0, 8) | 3);                         /* VERTEX_BUFFERS: one, 12 bytes a vertex */
    emit((1u << 14) | 12u); emit(batch_gpu + OFF_VERTS); emit(0); emit(3 * 12);
    emit(GEN(3, 0, 9) | 5);                         /* VERTEX_ELEMENTS: pad, position, texcoord */
    emit((1u << 25) | (0x000u << 16)); emit((2u << 28) | (2u << 24) | (2u << 20) | (2u << 16));
    emit((1u << 25) | (0x0F6u << 16) | 0); emit((1u << 28) | (1u << 24) | (2u << 20) | (3u << 16));
    emit((1u << 25) | (0x085u << 16) | 4); emit((1u << 28) | (1u << 24) | (2u << 20) | (3u << 16));
    emit(GEN(3, 0, 0x4B)); emit(4);                 /* VF_TOPOLOGY: a triangle list */
    emit(GEN(3, 0, 0x49) | 1); emit(0); emit(0);    /* VF_INSTANCING */
    emit(GEN(3, 3, 0) | 5);                         /* 3DPRIMITIVE: three vertices, one instance */
    emit(0); emit(3); emit(0); emit(1); emit(0); emit(0);
    emit(GEN(3, 2, 0) | 4);                         /* PIPE_CONTROL: flush the target, then write a word */
    emit((1u << 12) | (1u << 20) | (1u << 14) | (1u << 24));
    emit(scratch_gpu); emit(0);
    emit(0xC0FFEE01u); emit(0);
    emit(0x05000000u);                              /* MI_BATCH_BUFFER_END */
    cache_flush(batch, 4 * 4096);
    mfence();
    return (uint32_t)((uint8_t *)cmd - b);
}

/* ------------------------------------------------------------ hang handling */
static void dump_engine(const char *when)
{
    note("%s: head %08X tail %08X acthd %08X ipeir %08X ipehr %08X instdone %08X eir %08X esr %08X",
         when, RD(RING_HEAD(RCS)), RD(RING_TAIL(RCS)), RD(RING_ACTHD(RCS)), RD(RING_IPEIR(RCS)),
         RD(RING_IPEHR(RCS)), RD(RING_INSTDONE(RCS)), RD(RING_EIR(RCS)), RD(RING_ESR(RCS)));
}

static void reset_engine(void)
{
    int n;
    WR(RING_RESET_CTL(RCS), (1u << 16) | 1u);       /* ask; then wait for ready */
    n = poll_until(RING_RESET_CTL(RCS), 2u, 2u, 200000);
    note("reset: ready %s (reset_ctl %08X)", n < 0 ? "NOT signalled" : "signalled", RD(RING_RESET_CTL(RCS)));
    WR(GDRST, 1u << 1);                             /* the render domain */
    n = poll_until(GDRST, 1u << 1, 0, 2000000);
    note("reset: GDRST %08X (%s)", RD(GDRST), n < 0 ? "still set" : "done");
    WR(RING_CTL(RCS), 0);
    WR(FORCEWAKE_MT, (1u << 16) | 1u);              /* awake again, in case the reset let it sleep */
    poll_until(FORCEWAKE_ACK, 1, 1, 2000000);
    WR(RC_CONTROL, 0);
}

/* ------------------------------------------------------------ section 8: the cube */
/* Two screens for the display to alternate between, a depth buffer the
   engine sorts with, and a frame at a time: the whole pipeline state, a
   background that also clears the depth (depth test ALWAYS, z = 1), then
   the cube's twelve triangles with the test set to LESS.  The corners are
   turned and projected by the processor; w carries the view depth so the
   engine's texture interpolation is perspective-correct, and z is the
   hyperbolic depth that is linear on the screen. */
static uint32_t *screens[2], *depth;
static const uint32_t screen_gpu2[2] = { 0x10000000u, 0x12000000u };
static const uint32_t depth_gpu = 0x14000000u;
static int depth_rows;

#define VSIZE 24                        /* x y z w u v, floats */
#define OFF_VERTS2 0x1400               /* 42 vertices: 1008 bytes */

static float fsin(float x)
{
    float x2;
    while (x > 3.14159265f) x -= 6.2831853f;
    while (x < -3.14159265f) x += 6.2831853f;
    x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72))));
}
static float fcos(float x) { return fsin(x + 1.5707963f); }

static void put_vertex(float *v, float x, float y, float z, float w, float u, float t)
{
    v[0] = x; v[1] = y; v[2] = z; v[3] = w; v[4] = u; v[5] = t;
}

/* the frame's commands and vertices; returns the byte count */
#define V_DEPTH_PACKETS 1
#define V_DEPTH_TEST    2
#define V_FINAL_FLUSH   4
static uint32_t build_frame(uint32_t target_gpu, float ax, float ay, int bg_row, int w_is_one, uint32_t stamp, int variant, int shape)
{
    static const float corner[8][3] = {
        {-1,-1,-1}, {1,-1,-1}, {1,1,-1}, {-1,1,-1}, {-1,-1,1}, {1,-1,1}, {1,1,1}, {-1,1,1} };
    static const int face[6][4] = { {0,1,2,3}, {5,4,7,6}, {4,0,3,7}, {1,5,6,2}, {4,5,1,0}, {3,2,6,7} };
    static const float fuv[4][2] = { {0,0}, {1,0}, {1,0.9375f}, {0,0.9375f} };
    uint8_t *b = (uint8_t *)batch;
    float *vb = (float *)(b + OFF_VERTS2), rx[8], ry[8], rz[8];
    float sinx = fsin(ax), cosx = fcos(ax), siny = fsin(ay), cosy = fcos(ay);
    float cx = scr_w / 2.0f, cy = scr_h / 2.0f, focal = scr_h * 0.9f, camz = 4.0f, near = 1.5f, far = 8.0f;
    float bgv = (bg_row + 0.5f) / 64.0f;
    int i, k, n = 0, count1 = 6, count2 = 36;
    uint32_t *p;
    union { float f; uint32_t u; } fl;

    /* the background: two triangles over the whole screen, at the far plane */
    put_vertex(vb + n * 6, 0, 0, 1.0f, 1, 0.5f, bgv); n++;
    put_vertex(vb + n * 6, (float)scr_w, 0, 1.0f, 1, 0.5f, bgv); n++;
    put_vertex(vb + n * 6, (float)scr_w, (float)scr_h, 1.0f, 1, 0.5f, bgv); n++;
    put_vertex(vb + n * 6, 0, 0, 1.0f, 1, 0.5f, bgv); n++;
    put_vertex(vb + n * 6, (float)scr_w, (float)scr_h, 1.0f, 1, 0.5f, bgv); n++;
    put_vertex(vb + n * 6, 0, (float)scr_h, 1.0f, 1, 0.5f, bgv); n++;

    /* the cube's corners, turned and pushed back */
    for (i = 0; i < 8; i++) {
        float x = corner[i][0], y = corner[i][1], z = corner[i][2];
        float x1 = x * cosy + z * siny, z1 = -x * siny + z * cosy;
        float y2 = y * cosx - z1 * sinx, z2 = y * sinx + z1 * cosx;
        rx[i] = x1; ry[i] = y2; rz[i] = z2 + camz;
    }
    for (i = 0; i < 6; i++)
        for (k = 0; k < 6; k++) {
            static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
            int c = face[i][tri[k]];
            float z = rz[c], sx = cx + focal * rx[c] / z, sy = cy - focal * ry[c] / z;
            float d = (1.0f / near - 1.0f / z) / (1.0f / near - 1.0f / far);    /* 0 near .. 1 far */
            if (w_is_one) put_vertex(vb + n * 6, sx, sy, d, 1.0f, fuv[tri[k]][0], fuv[tri[k]][1]);
            else put_vertex(vb + n * 6, sx * z, sy * z, d * z, z, fuv[tri[k]][0], fuv[tri[k]][1]);
            n++;
        }

    if (shape == 1) {                               /* the old triangle, in the new format */
        put_vertex(vb + 0, 1600, 300, 0, 1, 0.0f, 0.0f);
        put_vertex(vb + 6, 2666, 1500, 0, 1, 1.0f, 0.0f);
        put_vertex(vb + 12, 533, 1500, 0, 1, 0.5f, 1.0f);
        count1 = 3; count2 = 0;
    } else if (shape == 2) {                        /* the background alone */
        count2 = 0;
    }

    /* the state, as before, but the target is this frame's screen */
    surface_state((uint32_t *)(b + OFF_SS_RT), target_gpu, scr_w, scr_h, scr_p * 4);
    surface_state((uint32_t *)(b + OFF_SS_TEX), tex_gpu, TEX_W, TEX_H, TEX_W * 4);

    cmd = batch;
    emit(GEN(1, 1, 4));
    emit(GEN(0, 1, 2) | 1); emit_zeros(2);
    emit(GEN(3, 1, 0x12)); emit(0);
    emit(GEN(3, 1, 0x13)); emit(0);
    emit(GEN(3, 1, 0x14)); emit(0);
    emit(GEN(3, 1, 0x15)); emit(0);
    emit(GEN(3, 1, 0x16)); emit(0);
    emit(GEN(0, 1, 1) | 14);
    emit(1); emit(0);
    emit(1);
    emit(batch_gpu | 1); emit(0);
    emit(batch_gpu | 1); emit(0);
    emit(0); emit(0);
    emit(batch_gpu | 1); emit(0);
    emit(0xFFFFF000u | 1);
    emit((4u << 12) | 1);
    emit(0xFFFFF000u | 1);
    emit((4u << 12) | 1);
    emit(GEN(3, 0, 0x23)); emit(OFF_CCVP);
    emit(GEN(3, 0, 0x21)); emit(OFF_SFVP);
    emit(GEN(3, 0, 0x30)); emit(64u | (1u << 16) | (2u << 25));
    emit(GEN(3, 0, 0x33)); emit(2u << 25);
    emit(GEN(3, 0, 0x31)); emit(2u << 25);
    emit(GEN(3, 0, 0x32)); emit(2u << 25);
    emit(GEN(3, 0, 0x24)); emit(OFF_BLEND | 1);
    emit(GEN(3, 0, 0x0E)); emit(OFF_CC | 1);
    emit(GEN(3, 0, 0x0D)); emit(0);
    emit(GEN(3, 0, 0x18)); emit(1);
    emit(GEN(3, 0, 0x52) | 3); emit_zeros(4);
    emit(GEN(3, 0, 0x19) | 9); emit_zeros(10);
    emit(GEN(3, 0, 0x1B) | 7); emit_zeros(8);
    emit(GEN(3, 0, 0x27)); emit(0);
    emit(GEN(3, 0, 0x2C)); emit(0);
    emit(GEN(3, 0, 0x1C) | 2); emit_zeros(3);
    emit(GEN(3, 0, 0x16) | 9); emit_zeros(10);
    emit(GEN(3, 0, 0x11) | 8); emit_zeros(9);
    emit(GEN(3, 0, 0x29)); emit(0);
    emit(GEN(3, 0, 0x2E)); emit(0);
    emit(GEN(3, 0, 0x1A) | 9); emit_zeros(10);
    emit(GEN(3, 0, 0x1D) | 7); emit_zeros(8);
    emit(GEN(3, 0, 0x28)); emit(0);
    emit(GEN(3, 0, 0x2D)); emit(0);
    emit(GEN(3, 0, 0x26)); emit(0);
    emit(GEN(3, 0, 0x2B)); emit(0);
    emit(GEN(3, 0, 0x15) | 9); emit_zeros(10);
    emit(GEN(3, 0, 0x10) | 7); emit_zeros(8);
    emit(GEN(3, 0, 0x1E) | 3); emit_zeros(4);
    emit(GEN(3, 0, 0x12) | 2); emit_zeros(3);
    emit(GEN(3, 0, 0x1F) | 2);
    emit((1u << 22) | (1u << 29) | (1u << 28) | (1u << 11) | (1u << 5));
    emit(0); emit(0);
    emit(GEN(3, 0, 0x51) | 9); emit_zeros(10);
    emit(GEN(3, 0, 0x50) | 3);
    emit((1u << 21) | (1u << 16)); emit_zeros(3);
    emit(GEN(3, 0, 0x13) | 2); emit_zeros(3);
    emit(GEN(3, 0, 0x2A)); emit(OFF_BT);
    emit(GEN(3, 0, 0x2F)); emit(OFF_SAMPLER);
    emit(GEN(3, 0, 0x14)); emit(1u << 11);
    emit(GEN(3, 0, 0x17) | 9); emit_zeros(10);
    emit(GEN(3, 0, 0x20) | 10);
    emit(OFF_KERNEL); emit(0);
    emit((1u << 27) | (2u << 18));
    emit(0); emit(0);
    emit((62u << 23) | (1u << 1));
    emit(6u << 16);
    emit_zeros(4);
    emit(GEN(3, 0, 0x4D)); emit(1u << 30);
    emit(GEN(3, 0, 0x4F)); emit((1u << 31) | (1u << 8));
    emit(GEN(3, 0, 0x0F)); emit(OFF_SCISSOR);
    if (variant & V_DEPTH_PACKETS) {
        /* the depth buffer: three stalls first, as the manuals ask */
        emit(GEN(3, 2, 0) | 4); emit(1u << 13); emit_zeros(4);
        emit(GEN(3, 2, 0) | 4); emit(1u << 0); emit_zeros(4);
        emit(GEN(3, 2, 0) | 4); emit(1u << 13); emit_zeros(4);
        emit(GEN(3, 0, 0x05) | 6);                  /* DEPTH_BUFFER: 2D, D32_FLOAT, written */
        emit((1u << 29) | (1u << 28) | (1u << 18) | (uint32_t)(scr_p * 4 - 1));
        emit(depth_gpu); emit(0);
        emit(((uint32_t)(scr_h - 1) << 18) | ((uint32_t)(scr_w - 1) << 4));
        emit(0x18u); emit(0); emit(0);
    } else {
        emit(GEN(3, 0, 0x05) | 6); emit_zeros(7);   /* no depth buffer, as the triangle had */
    }
    emit(GEN(3, 0, 0x07) | 3); emit_zeros(4);
    emit(GEN(3, 0, 0x06) | 3); emit_zeros(4);
    emit(GEN(3, 0, 0x04) | 1); emit(0); emit(1);
    emit(GEN(3, 1, 0) | 2);
    emit(0); emit(((uint32_t)(scr_h - 1) << 16) | (uint32_t)(scr_w - 1)); emit(0);
    emit(GEN(3, 0, 8) | 3);                         /* VERTEX_BUFFERS: 24 bytes a vertex */
    emit((1u << 14) | VSIZE); emit(batch_gpu + OFF_VERTS2); emit(0); emit(42 * VSIZE);
    emit(GEN(3, 0, 9) | 5);                         /* VERTEX_ELEMENTS: pad, xyzw, uv */
    emit((1u << 25) | (0x000u << 16)); emit((2u << 28) | (2u << 24) | (2u << 20) | (2u << 16));
    emit((1u << 25) | (0x000u << 16) | 0); emit((1u << 28) | (1u << 24) | (1u << 20) | (1u << 16));
    emit((1u << 25) | (0x085u << 16) | 16); emit((1u << 28) | (1u << 24) | (2u << 20) | (3u << 16));
    emit(GEN(3, 0, 0x4B)); emit(4);
    emit(GEN(3, 0, 0x49) | 1); emit(0); emit(0);
    /* the background: depth ALWAYS, writing 1.0 everywhere */
    emit(GEN(3, 0, 0x4E) | 1); emit((variant & V_DEPTH_TEST) ? 3u : 0u); emit(0);
    emit(GEN(3, 3, 0) | 5); emit(0); emit((uint32_t)count1); emit(0); emit(1); emit(0); emit(0);
    if (count2) {                                   /* the cube: depth LESS */
        emit(GEN(3, 0, 0x4E) | 1); emit((variant & V_DEPTH_TEST) ? (3u | (2u << 5)) : 0u); emit(0);
        emit(GEN(3, 3, 0) | 5); emit(0); emit((uint32_t)count2); emit(6); emit(1); emit(0); emit(0);
    }
    /* done: flush the target (and the depth, when asked), then say so */
    emit(GEN(3, 2, 0) | 4);
    emit((1u << 12) | ((variant & V_FINAL_FLUSH) ? 1u : 0u) | (1u << 20) | (1u << 14) | (1u << 24));
    emit(scratch_gpu); emit(0);
    emit(stamp); emit(0);
    emit(0x05000000u);
    cache_flush(batch, 4 * 4096);
    mfence();
    (void)p; (void)fl;
    return (uint32_t)((uint8_t *)cmd - b);
}

/* a batch into the ring; 0 when the stamp came back, else -1 */
static uint32_t ring_tail;
static int run_frame(uint32_t stamp, unsigned *gpu_us)
{
    uint64_t t0;
    uint32_t pos = ring_tail & 0xFFFu;
    ring[pos / 4] = 0x18800001u;
    ring[pos / 4 + 1] = batch_gpu;
    ring[pos / 4 + 2] = 0;
    ring[pos / 4 + 3] = 0;
    cache_flush(ring + pos / 4, 16);
    scratch[0] = 0;
    cache_flush(scratch, 64);
    mfence();
    ring_tail = (ring_tail + 16) & 0xFFFu;
    t0 = rdtsc();
    WR(RING_TAIL(RCS), ring_tail);
    for (;;) {
        uint32_t word;
        cache_flush(scratch, 64);
        mfence();
        word = scratch[0];
        if (word == stamp) break;
        if (us_since(t0) > 500000) { *gpu_us = us_since(t0); return -1; }
    }
    *gpu_us = us_since(t0);
    return 0;
}

static int draw(void)
{
    struct vbe_mode m;
    struct vbe_info info;
    int mode = -1, i, best_w = 0, k, cur = 0, hung = 0, phase, best_variant = -1, best_shape = -1;
    uint32_t stamp = 1, first;

    say("== 5. the cube, in phases: a bisection of the depth buffer, then the best with write-combining");
    if (sys_vbe_info(&info) != 0) { say("no VESA"); return -1; }
    for (i = 0; i < info.mode_count; i++) {
        struct vbe_mode t;
        if (sys_vbe_mode(info.modes[i], &t) != 0 || !t.framebuffer || t.bpp != 32 || t.width < 800) continue;
        if (t.width > best_w) { best_w = t.width; mode = info.modes[i]; m = t; }
    }
    if (mode < 0) { say("no 32-bit mode"); return -1; }
    scr_w = m.width;
    scr_h = m.height;
    scr_p = m.pitch / 4;
    npages = (m.pitch * m.height + 4095) / 4096;
    depth_rows = (scr_h + 31) & ~31;
    screens[0] = (uint32_t *)page_aligned(npages);
    screens[1] = (uint32_t *)page_aligned(npages);
    depth = (uint32_t *)page_aligned((scr_p * 4 * depth_rows + 4095) / 4096);
    if (!screens[0] || !screens[1] || !depth) { say("no memory for two screens and a depth buffer"); return -1; }
    screen = screens[0];
    scene();
    screen = screens[1];
    scene();
    memset(depth, 0, (size_t)scr_p * 4 * depth_rows);
    wbinvd();
    map_pages(screen_gpu2[0], (uint32_t)screens[0], npages);
    map_pages(screen_gpu2[1], (uint32_t)screens[1], npages);
    map_pages(depth_gpu, (uint32_t)depth, (scr_p * 4 * depth_rows + 4095) / 4096);
    WR(GFX_FLSH_CNTL, 1);
    say("mode %04X %ux%u; screens at %08X and %08X, depth at %08X (%u rows); setting the mode",
        mode, scr_w, scr_h, screen_gpu2[0], screen_gpu2[1], depth_gpu, depth_rows);
    flush();
    if (sys_set_vbe_mode(mode, 1) != 0) { say("the mode would not set"); return -1; }
    find_plane();
    if (active_plane < 0) { sys_set_video_mode(3); say("no plane on"); return -1; }
    WR(PLANE_SURF(active_plane), screen_gpu2[0]);
    ring_tail = RD(RING_TAIL(RCS));
    build_state();                                  /* the shader and the rest: once */

    for (phase = 0; phase < 8; phase++) {
        /* shape (0 = the old triangle routine), variant, w = 1?, frames, write-combining?, fixed target */
        static const int plan[8][6] = {
            { 3, 7, 0, 120, 0, -1 },    /* the cube, depth-buffered, as the firmware left the clock */
            { 3, 7, 0, 240, 0, -1 },    /* the same at the top clock */
            { 3, 7, 1, 120, 0, -1 },    /* w = 1: the same picture, the texture warped: the control */
            { 3, 7, 0, 600, 0, -1 },    /* ten seconds to look at */
            { -1, 0, 0, 0, 0, -1 }, { -1, 0, 0, 0, 0, -1 }, { -1, 0, 0, 0, 0, -1 }, { -1, 0, 0, 0, 0, -1 } };
        unsigned frames = 0, gpu_total = 0, wait_total = 0, gpu_max = 0, bytes = 0;
        uint64_t t_phase = rdtsc();
        float ax = 0.5f, ay = 0.0f;
        int shape = plan[phase][0], variant = plan[phase][1], w_one = plan[phase][2], want = plan[phase][3];
        if (!want) break;
        if (shape < 0) { if (best_shape < 0) break; shape = best_shape; variant = best_variant; }
        if (plan[phase][4]) WR(PPAT_LO, (RD(PPAT_LO) & 0xFFFFFF00u) | 0x01u);   /* entry 0: write-combining */
        if (phase == 1) {                           /* ask for the top clock and pin it there */
            uint32_t cap = RD(RP_STATE_CAP), rp0 = cap & 0xFF;
            WR(RPNSWREQ, rp0 << 24);
            WR(RP_INTERRUPT_LIMITS, (rp0 << 24) | (rp0 << 16));
            WR(RP_CONTROL, 0xF92u);                 /* turbo, hardware mode, up on busy, down on idle */
            for (k = 0; k < 3000000; k++) (void)RD(RPSTAT1);
            note("clock asked for %u MHz: running %u MHz", rp0 * 50, ((RD(RPSTAT1) >> 7) & 0x7F) * 50);
        }
        hung = 0;
        while (frames < (unsigned)want) {
            unsigned gpu_us, t_wait;
            uint64_t tw;
            int target = plan[phase][5] >= 0 ? plan[phase][5] : cur ^ 1;
            if (shape == 0) {
                screen_gpu = screen_gpu2[target];
                bytes = build_batch(1600, 300, 2666, 1500, 533, 1500);
                stamp = 0xC0FFEE01u;
            } else {
                bytes = build_frame(screen_gpu2[target], ax, ay, 60 + (phase & 3), w_one, stamp, variant, shape);
            }
            if (run_frame(stamp, &gpu_us) != 0) { hung = 1; note("phase %d (shape %d, variant %d) frame %u: no completion after %u us", phase, shape, variant, frames, gpu_us); dump_engine("stalled"); break; }
            stamp++;
            gpu_total += gpu_us;
            if (gpu_us > gpu_max) gpu_max = gpu_us;
            tw = rdtsc();
            WR(PLANE_SURF(active_plane), screen_gpu2[target]);
            for (k = 0; k < 2000000; k++) if ((RD(PLANE_SURFLIVE(active_plane)) & ~0xFFFu) == screen_gpu2[target]) break;
            t_wait = us_since(tw);
            wait_total += t_wait;
            cur = target;
            ax += 0.011f;
            ay += 0.017f;
            frames++;
        }
        if (frames) {
            unsigned total = us_since(t_phase);
            note("phase %d (shape %d, variant %d, w %s, %s): %u frames in %u ms = %u fps; engine %u us a frame (worst %u), panel wait %u us; batch %u bytes",
                 phase, shape, variant, w_one ? "= 1" : "= depth", plan[phase][4] ? "write-combining" : "uncached", frames, total / 1000,
                 frames * 1000000u / (total ? total : 1), gpu_total / frames, gpu_max, wait_total / frames, bytes);
        }
        if (!hung) { best_shape = shape; best_variant = variant; }
        note("  the clock after phase %d: %u MHz", phase, ((RD(RPSTAT1) >> 7) & 0x7F) * 50);
        flush();
        if (hung) {                                 /* the engine back, the ring again, on to the next */
            reset_engine();
            if (start_ring() != 0) { note("the ring would not restart"); break; }
            ring_tail = RD(RING_TAIL(RCS));
            stamp += 16;
        }
    }
    {
        int cx = scr_w / 2, cy = scr_h / 2;
        cache_flush(screens[cur] + cy * scr_p + cx, 64);
        cache_flush(screens[cur] + 40 * scr_p + 40, 64);
        mfence();
        note("last frame: pixel at the centre %08X, in the corner %08X", screens[cur][cy * scr_p + cx], screens[cur][40 * scr_p + 40]);
    }
    first = RD(PPAT_LO);
    note("attribute table at the end %08X", first);
    flush();
    note("best that ran: shape %d, variant %d (shapes: 0 old triangle, 1 triangle in new format, 2 background, 3 cube; variant 1 depth packets, 2 depth test, 4 depth flush)", best_shape, best_variant);
    flush();
    while (sys_kbhit()) sys_getkey();               /* keys pressed during the phases: forgotten */
    sys_getkey();                                   /* the last frame stays until a key */
    if (hung) reset_engine();
    WR(PLANE_SURF(active_plane), 0);
    for (k = 0; k < 3000000; k++) (void)RD(PLANE_CNTR(0));
    sys_set_video_mode(3);
    for (k = 0; k < nlater; k++) { sys_puts(later[k]); sys_puts("\r\n"); }
    return hung ? -1 : 0;
}

static void sleep_engine(void)
{
    WR(RING_MI_MODE(RCS), (1u << 24) | (1u << 8));
    WR(RING_CTL(RCS), 0);
    WR(FORCEWAKE_MT, 1u << 16);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    say("render probe: a spinning cube from the 3D engine, with a depth buffer");
    clock_start();
    if (find_device() != 0) { flush(); return 0; }
    flush();
    if (wake() != 0) { flush(); return 0; }
    flush();
    if (get_memory() != 0) { sleep_engine(); flush(); return 0; }
    flush();
    if (start_ring() != 0) { sleep_engine(); flush(); return 0; }
    flush();
    draw();
    sleep_engine();
    say("done");
    flush();
    return 0;
}
