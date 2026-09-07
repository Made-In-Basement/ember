/* fly.c - FLY.N32: a flight through a tunnel, drawn by the 3D engine.
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
    int h = sys_create("\\FLY.TXT");
    if (h < 0) { sys_puts("(could not write FLY.TXT)\r\n"); return; }
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

/* ------------------------------------------------------------ the demo */
/* A tunnel of rings receding into the dark, its centre wandering, and a
   craft in front of it.  The processor turns and projects the corners; the
   engine rasterises, samples the texture and sorts everything with its
   depth buffer, which is what makes the near wall hide the far one and the
   craft sit inside the tunnel rather than on top of it. */
#define VB_GPU   0x18000000u            /* the corners: their own pages */
#define VB_PAGES 32                     /* 128 KB, room for 5000 corners */
static float *vbuf;
static int vcount;

#define SIDES    16                     /* around the tunnel */
#define RINGS    24                     /* along it */
#define SPACING  1.15f
#define RADIUS   1.45f
#define NEAR     0.40f
#define FARZ     (RINGS * SPACING)

static float cam_x, cam_y, steer_x, steer_y, phase, bank;
static float proj_cx, proj_cy, proj_f;

/* the tunnel's centre wanders, so the flight is never straight */
static float curve_x(float z) { return fsin(z * 0.21f + phase * 0.03f) * 1.05f; }
static float curve_y(float z) { return fcos(z * 0.17f + phase * 0.024f) * 0.70f; }

/* The one texture, in four quarters of rising brightness: a plated wall
   with lit seams.  A face picks the quarter that suits how it is lit and
   how far away it is, which is what gives the tunnel its depth. */
static void demo_texture(void)
{
    static const int level[4] = { 44, 96, 168, 255 };
    int q, x, y;
    for (q = 0; q < 4; q++)
        for (y = 0; y < 32; y++)
            for (x = 0; x < 32; x++) {
                int px = (q & 1) * 32 + x, py = (q >> 1) * 32 + y;
                int lit = level[q], r, g, b;
                int seam = (x < 2 || y < 2);            /* the plate's edges */
                int rivet = ((x - 16) * (x - 16) + (y - 16) * (y - 16)) < 6;
                if (seam) { r = 255; g = 190; b = 90; }         /* a lit seam */
                else if (rivet) { r = 200; g = 140; b = 70; }
                else { r = 96 + ((x ^ y) & 7) * 3; g = 74; b = 52; }
                r = r * lit / 255; g = g * lit / 255; b = b * lit / 255;
                tex[py * TEX_W + px] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            }
    tex[63 * TEX_W + 63] = 0xFF07050Au;             /* the void the frame is cleared to */
    cache_flush(tex, TEX_W * TEX_H * 4);
    mfence();
}

/* one corner, projected and handed over as x*w, y*w, z*w, w */
static int emit_point(float x, float y, float z, float u, float v)
{
    float sx, sy, d;
    if (z < NEAR) return 0;
    sx = proj_cx + proj_f * x / z;
    sy = proj_cy - proj_f * y / z;
    d = (1.0f / NEAR - 1.0f / z) / (1.0f / NEAR - 1.0f / FARZ);
    put_vertex(vbuf + vcount * 6, sx * z, sy * z, d * z, z, u, v);
    vcount++;
    return 1;
}

/* a quad as two triangles, all four corners in tunnel space */
static void quad(const float *a, const float *b, const float *c, const float *dd, int q)
{
    float qx = (q & 1) ? 0.515f : 0.015f, qy = (q >> 1) ? 0.515f : 0.015f, s = 0.47f;
    int before = vcount;
    if (vcount + 6 > 5000) return;
    if (!emit_point(a[0], a[1], a[2], qx, qy)) { vcount = before; return; }
    if (!emit_point(b[0], b[1], b[2], qx + s, qy)) { vcount = before; return; }
    if (!emit_point(c[0], c[1], c[2], qx + s, qy + s)) { vcount = before; return; }
    if (!emit_point(a[0], a[1], a[2], qx, qy)) { vcount = before; return; }
    if (!emit_point(c[0], c[1], c[2], qx + s, qy + s)) { vcount = before; return; }
    if (!emit_point(dd[0], dd[1], dd[2], qx, qy + s)) { vcount = before; return; }
}

static void build_tunnel(void)
{
    static const float TWO_PI = 6.2831853f;
    int i, s;
    for (i = 0; i < RINGS - 1; i++) {
        float z0 = i * SPACING - phase, z1 = z0 + SPACING;
        float c0x, c0y, c1x, c1y;
        int fade;
        if (z1 < NEAR) continue;
        c0x = curve_x(z0 + phase) - cam_x; c0y = curve_y(z0 + phase) - cam_y;
        c1x = curve_x(z1 + phase) - cam_x; c1y = curve_y(z1 + phase) - cam_y;
        fade = i < 5 ? 2 : i < 11 ? 1 : 0;           /* the far end falls into the dark */
        for (s = 0; s < SIDES; s++) {
            float a0 = TWO_PI * s / SIDES, a1 = TWO_PI * (s + 1) / SIDES;
            float ca0 = fcos(a0), sa0 = fsin(a0), ca1 = fcos(a1), sa1 = fsin(a1);
            float p0[3], p1[3], p2[3], p3[3];
            float ny = -(sa0 + sa1) * 0.5f;           /* the wall faces inward */
            int q = (int)((ny * 0.5f + 0.5f) * 1.9f) + fade;
            if (q > 3) q = 3;
            if (q < 0) q = 0;
            p0[0] = c0x + RADIUS * ca0; p0[1] = c0y + RADIUS * sa0; p0[2] = z0;
            p1[0] = c0x + RADIUS * ca1; p1[1] = c0y + RADIUS * sa1; p1[2] = z0;
            p2[0] = c1x + RADIUS * ca1; p2[1] = c1y + RADIUS * sa1; p2[2] = z1;
            p3[0] = c1x + RADIUS * ca0; p3[1] = c1y + RADIUS * sa0; p3[2] = z1;
            quad(p0, p1, p2, p3, q);
        }
    }
}

/* the craft: a delta with a raised spine, banking as it is steered */
static void build_ship(void)
{
    static const float body[5][3] = {
        { 0.00f, -0.02f,  0.62f },      /* nose */
        {-0.34f, -0.06f, -0.22f },      /* left wing */
        { 0.34f, -0.06f, -0.22f },      /* right wing */
        { 0.00f,  0.13f, -0.14f },      /* spine */
        { 0.00f, -0.10f, -0.10f },      /* keel */
    };
    static const int tri[6][3] = {
        { 0, 1, 3 }, { 0, 3, 2 }, { 0, 4, 1 }, { 0, 2, 4 }, { 1, 4, 3 }, { 2, 3, 4 },
    };
    static const int shade_of[6] = { 3, 3, 1, 1, 2, 2 };
    float cb = fcos(bank), sb = fsin(bank), p[5][3];
    int i, k;
    for (i = 0; i < 5; i++) {           /* rolled, then set in front and below */
        float x = body[i][0], y = body[i][1];
        p[i][0] = x * cb - y * sb + steer_x * 0.30f;
        p[i][1] = x * sb + y * cb - 0.34f;
        p[i][2] = body[i][2] + 1.75f;
    }
    for (i = 0; i < 6; i++) {
        int q = shade_of[i];
        float qx = (q & 1) ? 0.515f : 0.015f, qy = (q >> 1) ? 0.515f : 0.015f, s = 0.47f;
        int before = vcount;
        if (vcount + 3 > 5000) return;
        if (!emit_point(p[tri[i][0]][0], p[tri[i][0]][1], p[tri[i][0]][2], qx + s * 0.5f, qy)) { vcount = before; continue; }
        if (!emit_point(p[tri[i][1]][0], p[tri[i][1]][1], p[tri[i][1]][2], qx, qy + s)) { vcount = before; continue; }
        if (!emit_point(p[tri[i][2]][0], p[tri[i][2]][1], p[tri[i][2]][2], qx + s, qy + s)) { vcount = before; continue; }
        (void)k;
    }
}

/* the whole frame: the void, then everything the flight is made of */
static uint32_t build_demo(uint32_t target_gpu, uint32_t stamp)
{
    uint8_t *b = (uint8_t *)batch;
    float bgv = 63.5f / 64.0f, bgu = 63.5f / 64.0f;
    int bg;

    proj_cx = scr_w / 2.0f;
    proj_cy = scr_h / 2.0f;
    proj_f = scr_h * 0.62f;
    vcount = 0;
    /* the void: two triangles at the far plane */
    put_vertex(vbuf + 0, 0, 0, 1.0f, 1, bgu, bgv);
    put_vertex(vbuf + 6, (float)scr_w, 0, 1.0f, 1, bgu, bgv);
    put_vertex(vbuf + 12, (float)scr_w, (float)scr_h, 1.0f, 1, bgu, bgv);
    put_vertex(vbuf + 18, 0, 0, 1.0f, 1, bgu, bgv);
    put_vertex(vbuf + 24, (float)scr_w, (float)scr_h, 1.0f, 1, bgu, bgv);
    put_vertex(vbuf + 30, 0, (float)scr_h, 1.0f, 1, bgu, bgv);
    vcount = 6;
    bg = 6;
    build_tunnel();
    build_ship();
    cache_flush(vbuf, (uint32_t)vcount * VSIZE);
    mfence();

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
    emit(GEN(3, 2, 0) | 4); emit(1u << 13); emit_zeros(4);
    emit(GEN(3, 2, 0) | 4); emit(1u << 0); emit_zeros(4);
    emit(GEN(3, 2, 0) | 4); emit(1u << 13); emit_zeros(4);
    emit(GEN(3, 0, 0x05) | 6);
    emit((1u << 29) | (1u << 28) | (1u << 18) | (uint32_t)(scr_p * 4 - 1));
    emit(depth_gpu); emit(0);
    emit(((uint32_t)(scr_h - 1) << 18) | ((uint32_t)(scr_w - 1) << 4));
    emit(0x18u); emit(0); emit(0);
    emit(GEN(3, 0, 0x07) | 3); emit_zeros(4);
    emit(GEN(3, 0, 0x06) | 3); emit_zeros(4);
    emit(GEN(3, 0, 0x04) | 1); emit(0); emit(1);
    emit(GEN(3, 1, 0) | 2);
    emit(0); emit(((uint32_t)(scr_h - 1) << 16) | (uint32_t)(scr_w - 1)); emit(0);
    emit(GEN(3, 0, 8) | 3);
    emit((1u << 14) | VSIZE); emit(VB_GPU); emit(0); emit((uint32_t)vcount * VSIZE);
    emit(GEN(3, 0, 9) | 5);
    emit((1u << 25) | (0x000u << 16)); emit((2u << 28) | (2u << 24) | (2u << 20) | (2u << 16));
    emit((1u << 25) | (0x000u << 16) | 0); emit((1u << 28) | (1u << 24) | (1u << 20) | (1u << 16));
    emit((1u << 25) | (0x085u << 16) | 16); emit((1u << 28) | (1u << 24) | (2u << 20) | (3u << 16));
    emit(GEN(3, 0, 0x4B)); emit(4);
    emit(GEN(3, 0, 0x49) | 1); emit(0); emit(0);
    /* the void first, writing depth 1 everywhere */
    emit(GEN(3, 0, 0x4E) | 1); emit(3u); emit(0);
    emit(GEN(3, 3, 0) | 5); emit(0); emit((uint32_t)bg); emit(0); emit(1); emit(0); emit(0);
    /* then the flight, sorted by depth */
    if (vcount > bg) {
        emit(GEN(3, 0, 0x4E) | 1); emit(3u | (2u << 5)); emit(0);
        emit(GEN(3, 3, 0) | 5); emit(0); emit((uint32_t)(vcount - bg)); emit((uint32_t)bg); emit(1); emit(0); emit(0);
    }
    emit(GEN(3, 2, 0) | 4);
    emit((1u << 12) | (1u << 0) | (1u << 20) | (1u << 14) | (1u << 24));
    emit(scratch_gpu); emit(0);
    emit(stamp); emit(0);
    emit(0x05000000u);
    cache_flush(batch, 4 * 4096);
    mfence();
    return (uint32_t)((uint8_t *)cmd - b);
}

/* a batch into the ring; 0 when the stamp came back */
static uint32_t ring_tail;
static int run_frame(uint32_t stamp, unsigned *gpu_us)
{
    uint32_t pos = ring_tail & 0xFFFu;
    uint64_t t0;
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
        *gpu_us = us_since(t0);
        if (word == stamp) return 0;
        if (*gpu_us > 500000) return -1;
    }
}

static int fly(void)
{
    struct vbe_mode m;
    struct vbe_info info;
    int mode = -1, i, best_w = 0, k, cur = 0, hung = 0, quit = 0;
    unsigned frames = 0, gpu_total = 0, gpu_max = 0, fps = 0;
    uint32_t stamp = 1, npages;
    uint64_t t_start, t_fps;

    say("== 5. the flight");
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
    screens[0] = (uint32_t *)page_aligned((int)npages);
    screens[1] = (uint32_t *)page_aligned((int)npages);
    depth = (uint32_t *)page_aligned((scr_p * 4 * depth_rows + 4095) / 4096);
    vbuf = (float *)page_aligned(VB_PAGES);
    if (!screens[0] || !screens[1] || !depth || !vbuf) { say("not enough memory"); return -1; }
    memset(screens[0], 0, (size_t)m.pitch * m.height);
    memset(screens[1], 0, (size_t)m.pitch * m.height);
    memset(depth, 0, (size_t)scr_p * 4 * depth_rows);
    memset(vbuf, 0, VB_PAGES * 4096);
    wbinvd();
    map_pages(screen_gpu2[0], (uint32_t)screens[0], (int)npages);
    map_pages(screen_gpu2[1], (uint32_t)screens[1], (int)npages);
    map_pages(depth_gpu, (uint32_t)depth, (scr_p * 4 * depth_rows + 4095) / 4096);
    map_pages(VB_GPU, (uint32_t)vbuf, VB_PAGES);
    WR(GFX_FLSH_CNTL, 1);
    demo_texture();
    say("mode %04X %ux%u; screens %08X/%08X, depth %08X, corners %08X", mode, scr_w, scr_h,
        screen_gpu2[0], screen_gpu2[1], depth_gpu, VB_GPU);
    say("arrows steer, Esc leaves");
    flush();
    if (sys_set_vbe_mode(mode, 1) != 0) { say("the mode would not set"); return -1; }
    find_plane();
    if (active_plane < 0) { sys_set_video_mode(3); say("no plane on"); return -1; }
    WR(PLANE_SURF(active_plane), screen_gpu2[0]);
    ring_tail = RD(RING_TAIL(RCS));
    build_state();
    t_start = rdtsc();
    t_fps = t_start;

    while (!quit) {
        unsigned gpu_us;
        int target = cur ^ 1;
        while (sys_kbhit()) {                       /* steering, and the way out */
            int key = sys_getkey(), sc = (key >> 8) & 0xFF;
            if ((key & 0xFF) == 27) quit = 1;
            else if (sc == 0x4B) steer_x -= 0.16f;  /* left */
            else if (sc == 0x4D) steer_x += 0.16f;  /* right */
            else if (sc == 0x48) steer_y += 0.12f;  /* up */
            else if (sc == 0x50) steer_y -= 0.12f;  /* down */
        }
        if (steer_x > 0.9f) steer_x = 0.9f;
        if (steer_x < -0.9f) steer_x = -0.9f;
        if (steer_y > 0.7f) steer_y = 0.7f;
        if (steer_y < -0.7f) steer_y = -0.7f;
        steer_x *= 0.94f;                           /* it settles back to the middle */
        steer_y *= 0.94f;
        bank = bank * 0.85f - steer_x * 0.16f;
        phase += 0.085f;
        if (phase > SPACING) phase -= SPACING;
        cam_x = curve_x(phase) + steer_x;
        cam_y = curve_y(phase) + steer_y;

        build_demo(screen_gpu2[target], stamp);
        if (run_frame(stamp, &gpu_us) != 0) {
            hung = 1;
            note("frame %u did not finish after %u us with %d corners", frames, gpu_us, vcount);
            dump_engine("stalled");
            break;
        }
        stamp++;
        gpu_total += gpu_us;
        if (gpu_us > gpu_max) gpu_max = gpu_us;
        WR(PLANE_SURF(active_plane), screen_gpu2[target]);
        for (k = 0; k < 2000000; k++)
            if ((RD(PLANE_SURFLIVE(active_plane)) & ~0xFFFu) == screen_gpu2[target]) break;
        cur = target;
        frames++;
        if (us_since(t_fps) > 1000000u) { fps = frames; t_fps = rdtsc(); }
    }

    note("%u frames, about %u a second; the engine took %u us a frame (worst %u), %d corners",
         frames, fps, frames ? gpu_total / frames : 0, gpu_max, vcount);
    while (sys_kbhit()) sys_getkey();
    if (hung) { sys_getkey(); reset_engine(); }
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
    say("FLY: a tunnel, on the chip's own 3D engine");
    clock_start();
    if (find_device() != 0) { flush(); return 0; }
    flush();
    if (wake() != 0) { flush(); return 0; }
    flush();
    if (get_memory() != 0) { sleep_engine(); flush(); return 0; }
    flush();
    if (start_ring() != 0) { sleep_engine(); flush(); return 0; }
    flush();
    fly();
    sleep_engine();
    say("done");
    flush();
    return 0;
}
