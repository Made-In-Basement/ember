/* gpu3d.c - the chip's 3D engine as a service for the desktop's programs.
 *
 * What RENDER.N32 proved, made permanent: the render engine's command
 * ring, a batch buffer carrying the whole pipeline state that a textured,
 * depth-buffered triangle needs (the Intel GPU tools' Broadwell sequence,
 * with their pixel shader), and a draw.  The vertex shader is off and the
 * viewport transform is off: corners arrive in screen space as x*w, y*w,
 * z*w, w, the engine divides, and its interpolation is perspective-correct.
 *
 * It sits on the display driver: the same page table, and the target is
 * one of the desktop's own scanout buffers.  Cache attribute entry 0 is
 * made uncached while the engine is in use, so its writes land in memory;
 * the desktop's own writes are flushed line by line anyway.  The clock is
 * raised to the chip's top while a program uses the engine, and lowered
 * again after.
 *
 * A job that does not finish is written to the log with the fault
 * registers, the engine is reset, and after three of those the service
 * withdraws and programs fall back to drawing on the processor.
 */
#include <nanolibc.h>
#include "nano.h"
#include "draw.h"
#include "gpu.h"
#include "gpu3d.h"

static volatile uint32_t *mmio;
static volatile uint64_t *ggtt;
#define RD(o)     (mmio[(o) / 4])
#define WR(o, v)  (mmio[(o) / 4] = (v))

#define FORCEWAKE_MT        0xA188
#define FORCEWAKE_ACK       0x130044
#define RC_CONTROL          0xA090
#define GFX_FLSH_CNTL       0x101008
#define PPAT_LO             0x40E0
#define GDRST               0x941C
#define RP_STATE_CAP        0x145998    /* rp0 [7:0], rpn [23:16], in 50 MHz */
#define RPNSWREQ            0xA008
#define RP_INTERRUPT_LIMITS 0xA014
#define RPSTAT1             0xA01C
#define RP_CONTROL          0xA024
#define RCS                 0x2000
#define RING_TAIL           (RCS + 0x30)
#define RING_HEAD           (RCS + 0x34)
#define RING_START          (RCS + 0x38)
#define RING_CTL            (RCS + 0x3C)
#define RING_IPEIR          (RCS + 0x64)
#define RING_IPEHR          (RCS + 0x68)
#define RING_INSTDONE       (RCS + 0x6C)
#define RING_ACTHD          (RCS + 0x74)
#define RING_HWS            (RCS + 0x80)
#define RING_MI_MODE        (RCS + 0x9C)
#define RING_IMR            (RCS + 0xA8)
#define RING_EIR            (RCS + 0xB0)
#define RING_RESET_CTL      (RCS + 0xD0)

/* the engine's memory: GPU addresses below the desktop's screens */
#define RING_GPU     0x07000000u
#define HWS_GPU      0x07001000u
#define SCRATCH_GPU  0x07002000u
#define BATCH_GPU    0x07010000u        /* four pages */
#define TEX_GPU      0x07020000u        /* four pages */
#define DEPTH_GPU    0x16000000u
#define PTE_FLAGS    0x03u
#define TEX_W 64
#define TEX_H 64

static uint32_t *ring, *hws, *scratch, *batch, *tex, *depth;
static int active, failures, depth_rows;
static uint32_t ring_tail, stamp = 1, ppat_was, rp0, rpn;
static unsigned last_us;
static char note_buf[120] = "3D engine not started";

static void wbinvd(void) { __asm__ volatile("wbinvd" ::: "memory"); }
static void mfence(void) { __asm__ volatile("mfence" ::: "memory"); }

static int poll_until(uint32_t reg, uint32_t mask, uint32_t want, int loops)
{
    int i;
    for (i = 0; i < loops; i++)
        if ((RD(reg) & mask) == want) return i;
    return -1;
}

static uint32_t page_aligned(int pages)
{
    uint32_t p = (uint32_t)malloc((pages + 1) * 4096);
    return p ? (p + 4095) & ~4095u : 0;
}

int gpu3d_active(void) { return active; }
const char *gpu3d_note(void) { return note_buf; }
unsigned gpu3d_last_us(void) { return last_us; }
unsigned gpu3d_mhz(void) { return active ? ((RD(RPSTAT1) >> 7) & 0x7F) * 50 : 0; }

/* ---------------------------------------------------------------- the state */
/* offsets within the batch: commands from 0, state in the second page,
   vertices from the sixth kilobyte */
#define OFF_BT       0x1000
#define OFF_SS_RT    0x1040
#define OFF_SS_TEX   0x1080
#define OFF_SAMPLER  0x10C0
#define OFF_CC       0x1100
#define OFF_BLEND    0x1140
#define OFF_CCVP     0x11C0
#define OFF_SFVP     0x1200
#define OFF_SCISSOR  0x1240
#define OFF_KERNEL   0x1280
#define OFF_VERTS    0x1400
#define VSIZE        24

#define GEN(pipe, op, sub) ((3u << 29) | ((pipe) << 27) | ((op) << 24) | ((sub) << 16))

static const uint32_t ps_kernel[4][4] = {  /* igt's blit.g7a, assembled for gen8: sample, write */
    { 0x0080005a, 0x2f403ae8, 0x3a0000c0, 0x008d0040 },
    { 0x0080005a, 0x2f803ae8, 0x3a0000d0, 0x008d0040 },
    { 0x02800031, 0x2e203a48, 0x0e8d0f40, 0x08840001 },
    { 0x05800031, 0x20003a40, 0x0e8d0e20, 0x90031000 },
};

static void surface_state(uint32_t *ss, uint32_t gpu, int w, int h, int pitch)
{
    memset(ss, 0, 64);
    ss[0] = (1u << 29) | (0x0C0u << 18) | (1u << 16) | (1u << 14) | (1u << 8);
    ss[1] = 0x18u << 24;
    ss[2] = ((uint32_t)(h - 1) << 16) | (uint32_t)(w - 1);
    ss[3] = (uint32_t)(pitch - 1);
    ss[7] = (4u << 25) | (5u << 22) | (6u << 19) | (7u << 16);
    ss[8] = gpu;
    ss[9] = 0;
}

static void build_state(void)
{
    uint8_t *b = (uint8_t *)batch;
    uint32_t *p;
    int i;
    union { float f; uint32_t u; } fl;
    p = (uint32_t *)(b + OFF_BT);
    p[0] = OFF_SS_RT;
    p[1] = OFF_SS_TEX;
    surface_state((uint32_t *)(b + OFF_SS_TEX), TEX_GPU, TEX_W, TEX_H, TEX_W * 4);
    p = (uint32_t *)(b + OFF_SAMPLER);              /* nearest, clamp */
    p[0] = 0; p[1] = 0; p[2] = 0;
    p[3] = (2u << 6) | (2u << 3) | 2u;
    memset(b + OFF_CC, 0, 24);
    p = (uint32_t *)(b + OFF_BLEND);
    p[0] = 0;
    for (i = 0; i < 16; i++) {
        p[1 + i * 2] = (1u << 26) | (0x11u << 21);
        p[2 + i * 2] = 2u;
    }
    p = (uint32_t *)(b + OFF_CCVP);
    fl.f = -1.0e35f; p[0] = fl.u;
    fl.f = 1.0e35f; p[1] = fl.u;
    p = (uint32_t *)(b + OFF_SFVP);
    memset(p, 0, 64);
    fl.f = 1.0f;
    p[9] = fl.u;
    p[11] = fl.u;
    memset(b + OFF_SCISSOR, 0, 8);
    memcpy(b + OFF_KERNEL, ps_kernel, sizeof ps_kernel);
}

static uint32_t *cmd;
static void emit(uint32_t v) { *cmd++ = v; }
static void emit_zeros(int n) { while (n--) emit(0); }

/* the commands for one job; the vertices are already in place */
static uint32_t build_batch(uint32_t target_gpu, int x, int y, int w, int h, int nverts)
{
    uint8_t *b = (uint8_t *)batch;
    surface_state((uint32_t *)(b + OFF_SS_RT), target_gpu, scr_w, scr_h, scr_w * 4);
    cmd = batch;
    emit(GEN(1, 1, 4));                             /* PIPELINE_SELECT: 3D */
    emit(GEN(0, 1, 2) | 1); emit_zeros(2);          /* STATE_SIP */
    emit(GEN(3, 1, 0x12)); emit(0);                 /* push constants: none */
    emit(GEN(3, 1, 0x13)); emit(0);
    emit(GEN(3, 1, 0x14)); emit(0);
    emit(GEN(3, 1, 0x15)); emit(0);
    emit(GEN(3, 1, 0x16)); emit(0);
    emit(GEN(0, 1, 1) | 14);                        /* STATE_BASE_ADDRESS: the batch */
    emit(1); emit(0);
    emit(1);
    emit(BATCH_GPU | 1); emit(0);
    emit(BATCH_GPU | 1); emit(0);
    emit(0); emit(0);
    emit(BATCH_GPU | 1); emit(0);
    emit(0xFFFFF000u | 1);
    emit((4u << 12) | 1);
    emit(0xFFFFF000u | 1);
    emit((4u << 12) | 1);
    emit(GEN(3, 0, 0x23)); emit(OFF_CCVP);
    emit(GEN(3, 0, 0x21)); emit(OFF_SFVP);
    emit(GEN(3, 0, 0x30)); emit(64u | (1u << 16) | (2u << 25));   /* URB */
    emit(GEN(3, 0, 0x33)); emit(2u << 25);
    emit(GEN(3, 0, 0x31)); emit(2u << 25);
    emit(GEN(3, 0, 0x32)); emit(2u << 25);
    emit(GEN(3, 0, 0x24)); emit(OFF_BLEND | 1);
    emit(GEN(3, 0, 0x0E)); emit(OFF_CC | 1);
    emit(GEN(3, 0, 0x0D)); emit(0);
    emit(GEN(3, 0, 0x18)); emit(1);
    emit(GEN(3, 0, 0x52) | 3); emit_zeros(4);       /* the stages not used, told so */
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
    emit(GEN(3, 0, 0x10) | 7); emit_zeros(8);       /* VS: off */
    emit(GEN(3, 0, 0x1E) | 3); emit_zeros(4);
    emit(GEN(3, 0, 0x12) | 2); emit_zeros(3);       /* CLIP: off */
    emit(GEN(3, 0, 0x1F) | 2);                      /* SBE: one attribute */
    emit((1u << 22) | (1u << 29) | (1u << 28) | (1u << 11) | (1u << 5));
    emit(0); emit(0);
    emit(GEN(3, 0, 0x51) | 9); emit_zeros(10);
    emit(GEN(3, 0, 0x50) | 3);                      /* RASTER: cull none */
    emit((1u << 21) | (1u << 16)); emit_zeros(3);
    emit(GEN(3, 0, 0x13) | 2); emit_zeros(3);       /* SF: no viewport transform */
    emit(GEN(3, 0, 0x2A)); emit(OFF_BT);
    emit(GEN(3, 0, 0x2F)); emit(OFF_SAMPLER);
    emit(GEN(3, 0, 0x14)); emit(1u << 11);          /* WM: perspective pixel barycentric */
    emit(GEN(3, 0, 0x17) | 9); emit_zeros(10);
    emit(GEN(3, 0, 0x20) | 10);                     /* PS */
    emit(OFF_KERNEL); emit(0);
    emit((1u << 27) | (2u << 18));
    emit(0); emit(0);
    emit((62u << 23) | (1u << 1));
    emit(6u << 16);
    emit_zeros(4);
    emit(GEN(3, 0, 0x4D)); emit(1u << 30);
    emit(GEN(3, 0, 0x4F)); emit((1u << 31) | (1u << 8));
    emit(GEN(3, 0, 0x0F)); emit(OFF_SCISSOR);
    emit(GEN(3, 2, 0) | 4); emit(1u << 13); emit_zeros(4);   /* depth stalls, as the manuals ask */
    emit(GEN(3, 2, 0) | 4); emit(1u << 0); emit_zeros(4);
    emit(GEN(3, 2, 0) | 4); emit(1u << 13); emit_zeros(4);
    emit(GEN(3, 0, 0x05) | 6);                      /* DEPTH_BUFFER: 2D, D32_FLOAT, written */
    emit((1u << 29) | (1u << 28) | (1u << 18) | (uint32_t)(scr_w * 4 - 1));
    emit(DEPTH_GPU); emit(0);
    emit(((uint32_t)(scr_h - 1) << 18) | ((uint32_t)(scr_w - 1) << 4));
    emit(0x18u); emit(0); emit(0);
    emit(GEN(3, 0, 0x07) | 3); emit_zeros(4);
    emit(GEN(3, 0, 0x06) | 3); emit_zeros(4);
    emit(GEN(3, 0, 0x04) | 1); emit(0); emit(1);
    emit(GEN(3, 1, 0) | 2);                         /* DRAWING_RECTANGLE: the window's part */
    emit(((uint32_t)y << 16) | (uint32_t)x);
    emit(((uint32_t)(y + h - 1) << 16) | (uint32_t)(x + w - 1));
    emit(0);
    emit(GEN(3, 0, 8) | 3);                         /* VERTEX_BUFFERS */
    emit((1u << 14) | VSIZE); emit(BATCH_GPU + OFF_VERTS); emit(0); emit((uint32_t)(6 + nverts) * VSIZE);
    emit(GEN(3, 0, 9) | 5);                         /* VERTEX_ELEMENTS: pad, xyzw, uv */
    emit((1u << 25) | (0x000u << 16)); emit((2u << 28) | (2u << 24) | (2u << 20) | (2u << 16));
    emit((1u << 25) | (0x000u << 16) | 0); emit((1u << 28) | (1u << 24) | (1u << 20) | (1u << 16));
    emit((1u << 25) | (0x085u << 16) | 16); emit((1u << 28) | (1u << 24) | (2u << 20) | (3u << 16));
    emit(GEN(3, 0, 0x4B)); emit(4);                 /* triangles */
    emit(GEN(3, 0, 0x49) | 1); emit(0); emit(0);
    emit(GEN(3, 0, 0x4E) | 1); emit(3u); emit(0);   /* the clear: depth ALWAYS, written */
    emit(GEN(3, 3, 0) | 5); emit(0); emit(6); emit(0); emit(1); emit(0); emit(0);
    emit(GEN(3, 0, 0x4E) | 1); emit(3u | (2u << 5)); emit(0);   /* the scene: depth LESS */
    emit(GEN(3, 3, 0) | 5); emit(0); emit((uint32_t)nverts); emit(6); emit(1); emit(0); emit(0);
    emit(GEN(3, 2, 0) | 4);                         /* PIPE_CONTROL: flush, then the word */
    emit((1u << 12) | (1u << 0) | (1u << 20) | (1u << 14) | (1u << 24));
    emit(SCRATCH_GPU); emit(0);
    emit(stamp); emit(0);
    emit(0x05000000u);                              /* MI_BATCH_BUFFER_END */
    cache_flush(batch, 4 * 4096);
    mfence();
    return (uint32_t)((uint8_t *)cmd - b);
}

/* ---------------------------------------------------------------- the ring */
static int start_ring(void)
{
    uint32_t tail;
    int n;
    WR(RING_IMR, 0xFFFFFFFFu);
    WR(RING_MI_MODE, (1u << 24) | (1u << 8));
    poll_until(RING_MI_MODE, 1u << 9, 1u << 9, 200000);
    WR(RING_HWS, HWS_GPU);
    WR(RING_HEAD, 0);
    WR(RING_TAIL, 0);
    WR(RING_START, RING_GPU);
    WR(RING_CTL, 1u);
    if (!(RD(RING_CTL) & 1)) return -1;
    WR(RING_MI_MODE, 1u << 24);
    memset(ring, 0, 64);
    cache_flush(ring, 64);
    mfence();
    tail = 32;
    WR(RING_TAIL, tail);
    n = poll_until(RING_HEAD, 0x1FFFFC, tail, 2000000);
    ring_tail = tail;
    return n < 0 ? -1 : 0;
}

static void reset_engine(void)
{
    sys_logf("3d: stalled: head %08X tail %08X acthd %08X ipehr %08X instdone %08X eir %08X",
             RD(RING_HEAD), RD(RING_TAIL), RD(RING_ACTHD), RD(RING_IPEHR), RD(RING_INSTDONE), RD(RING_EIR));
    WR(RING_RESET_CTL, (1u << 16) | 1u);
    poll_until(RING_RESET_CTL, 2u, 2u, 200000);
    WR(GDRST, 1u << 1);
    poll_until(GDRST, 1u << 1, 0, 2000000);
    WR(RING_CTL, 0);
    WR(FORCEWAKE_MT, (1u << 16) | 1u);
    poll_until(FORCEWAKE_ACK, 1, 1, 2000000);
    WR(RC_CONTROL, 0);
    if (start_ring() != 0) { active = 0; snprintf(note_buf, sizeof note_buf, "3D engine withdrawn: it would not restart"); }
    sys_logf("3d: reset; %s", active ? "ring restarted" : "ring would not restart");
}

/* ---------------------------------------------------------------- open, close */
int gpu3d_open(void)
{
    int n;
    if (active) return 0;
    if (!gpu_active() || !gpu_regs()) { snprintf(note_buf, sizeof note_buf, "3D engine needs the display driver"); return -1; }
    mmio = gpu_regs();
    ggtt = gpu_table();
    depth_rows = (scr_h + 31) & ~31;
    if (!ring) {
        ring = (uint32_t *)page_aligned(1);
        hws = (uint32_t *)page_aligned(1);
        scratch = (uint32_t *)page_aligned(1);
        batch = (uint32_t *)page_aligned(4);
        tex = (uint32_t *)page_aligned(4);
        depth = (uint32_t *)page_aligned((scr_w * 4 * depth_rows + 4095) / 4096);
        if (!ring || !hws || !scratch || !batch || !tex || !depth) {
            snprintf(note_buf, sizeof note_buf, "3D engine: no memory for a %d MB depth buffer",
                     (int)(((unsigned)scr_w * 4 * depth_rows) >> 20));
            sys_logf("3d: could not allocate; depth wanted %u bytes", (unsigned)scr_w * 4 * depth_rows);
            return -1;
        }
        memset(ring, 0, 4096); memset(hws, 0, 4096); memset(scratch, 0, 4096);
        memset(batch, 0, 4 * 4096); memset(tex, 0, 4 * 4096);
        memset(depth, 0, (size_t)scr_w * 4 * depth_rows);
        wbinvd();
        gpu_map(RING_GPU, (uint32_t)ring, 1);
        gpu_map(HWS_GPU, (uint32_t)hws, 1);
        gpu_map(SCRATCH_GPU, (uint32_t)scratch, 1);
        gpu_map(BATCH_GPU, (uint32_t)batch, 4);
        gpu_map(TEX_GPU, (uint32_t)tex, 4);
        gpu_map(DEPTH_GPU, (uint32_t)depth, (scr_w * 4 * depth_rows + 4095) / 4096);
        WR(GFX_FLSH_CNTL, 1);
    }
    WR(FORCEWAKE_MT, (1u << 16) | 1u);
    n = poll_until(FORCEWAKE_ACK, 1, 1, 2000000);
    if (n < 0) { snprintf(note_buf, sizeof note_buf, "3D engine: the render well would not wake"); return -1; }
    WR(RC_CONTROL, 0);
    ppat_was = RD(PPAT_LO);
    WR(PPAT_LO, ppat_was & 0xFFFFFF00u);            /* entry 0 uncached: engine writes land in memory */
    /* The clock is left exactly as the firmware set it.  Raising it needs
       registers this machine has not confirmed, and a request built from a
       bad reading stalls the chip and takes the display down with it. */
    if (start_ring() != 0) { snprintf(note_buf, sizeof note_buf, "3D engine: the ring would not run"); WR(PPAT_LO, ppat_was); WR(FORCEWAKE_MT, 1u << 16); return -1; }
    build_state();
    failures = 0;
    active = 1;
    snprintf(note_buf, sizeof note_buf, "3D engine ready at %u MHz", ((RD(RPSTAT1) >> 7) & 0x7F) * 50);
    sys_logf("3d: ring running at %u MHz; ring %08X batch %08X depth %08X (%d rows), screen %dx%d",
             ((RD(RPSTAT1) >> 7) & 0x7F) * 50, RING_GPU, BATCH_GPU, DEPTH_GPU, depth_rows, scr_w, scr_h);
    return 0;
}

void gpu3d_close(void)
{
    if (!active) return;
    WR(RING_MI_MODE, (1u << 24) | (1u << 8));
    WR(RING_CTL, 0);
    WR(PPAT_LO, ppat_was);
    WR(FORCEWAKE_MT, 1u << 16);
    active = 0;
    snprintf(note_buf, sizeof note_buf, "3D engine idle");
}

/* ---------------------------------------------------------------- jobs */
void gpu3d_texture(const uint32_t *argb)
{
    if (!tex) return;
    memcpy(tex, argb, TEX_W * TEX_H * 4);
    cache_flush(tex, TEX_W * TEX_H * 4);
    mfence();
}

#define MAX_JOBS 2
#define MAX_VERTS 256
static struct job { int x, y, w, h, n; float bg_u, bg_v; float v[MAX_VERTS * 6]; } jobs[MAX_JOBS];
static int njobs;

int gpu3d_queue(int x, int y, int w, int h, const float *verts, int n, float bg_u, float bg_v)
{
    struct job *j;
    if (!active || njobs >= MAX_JOBS || n > MAX_VERTS || w <= 0 || h <= 0) return -1;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > scr_w) w = scr_w - x;
    if (y + h > scr_h) h = scr_h - y;
    if (w <= 0 || h <= 0) return -1;
    j = &jobs[njobs++];
    j->x = x; j->y = y; j->w = w; j->h = h; j->n = n; j->bg_u = bg_u; j->bg_v = bg_v;
    memcpy(j->v, verts, (size_t)n * 6 * sizeof(float));
    return 0;
}

static void put_vertex(float *v, float x, float y, float z, float w, float u, float t)
{
    v[0] = x; v[1] = y; v[2] = z; v[3] = w; v[4] = u; v[5] = t;
}

void gpu3d_run(uint32_t *buffer)
{
    uint32_t target = gpu_buffer_address(buffer);
    int k;
    if (!active || !njobs) { njobs = 0; return; }
    if (!target) { sys_log("3d: the frame is not a buffer the engine knows"); njobs = 0; return; }
    for (k = 0; k < njobs && active; k++) {
        struct job *j = &jobs[k];
        float *vb = (float *)((uint8_t *)batch + OFF_VERTS);
        float x0 = (float)j->x, y0 = (float)j->y, x1 = (float)(j->x + j->w), y1 = (float)(j->y + j->h);
        uint32_t pos;
        uint64_t t0;
        unsigned waited = 0;
        /* the clear: the rectangle at the far plane, one texel */
        put_vertex(vb + 0, x0, y0, 1, 1, j->bg_u, j->bg_v);
        put_vertex(vb + 6, x1, y0, 1, 1, j->bg_u, j->bg_v);
        put_vertex(vb + 12, x1, y1, 1, 1, j->bg_u, j->bg_v);
        put_vertex(vb + 18, x0, y0, 1, 1, j->bg_u, j->bg_v);
        put_vertex(vb + 24, x1, y1, 1, 1, j->bg_u, j->bg_v);
        put_vertex(vb + 30, x0, y1, 1, 1, j->bg_u, j->bg_v);
        memcpy(vb + 36, j->v, (size_t)j->n * 6 * sizeof(float));
        build_batch(target, j->x, j->y, j->w, j->h, j->n);
        pos = ring_tail & 0xFFFu;
        ring[pos / 4] = 0x18800001u;                /* MI_BATCH_BUFFER_START, global table */
        ring[pos / 4 + 1] = BATCH_GPU;
        ring[pos / 4 + 2] = 0;
        ring[pos / 4 + 3] = 0;
        cache_flush(ring + pos / 4, 16);
        scratch[0] = 0;
        cache_flush(scratch, 64);
        mfence();
        ring_tail = (ring_tail + 16) & 0xFFFu;
        t0 = now_us();
        WR(RING_TAIL, ring_tail);
        for (;;) {
            uint32_t word;
            cache_flush(scratch, 64);
            mfence();
            word = scratch[0];
            if (word == stamp) break;
            waited = now_us() - (unsigned)t0;
            if (waited > 100000) break;             /* a tenth of a second: it is not coming */
        }
        last_us = now_us() - (unsigned)t0;
        if (waited > 100000) {
            failures++;
            reset_engine();
            if (failures >= 3 && active) { active = 0; snprintf(note_buf, sizeof note_buf, "3D engine withdrawn after three stalls"); sys_log("3d: withdrawn"); }
        }
        stamp++;
    }
    njobs = 0;
}
