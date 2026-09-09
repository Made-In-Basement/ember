/* =============================================================================
   render_gpu.c - the voxel world, drawn by the graphics engine
   -----------------------------------------------------------------------------
   The game hands over triangles in world coordinates and a camera.  The
   processor turns them into the camera's view, clips whatever crosses the
   near plane, projects them onto the screen and writes the corners into a
   buffer the engine reads; the engine rasterises them, samples the texture
   and sorts them with a depth buffer.  That division is deliberate: the
   engine's pixel program can only sample a texture, so anything cleverer
   than that has to happen before the corners are handed over.

   Lighting is the visible consequence.  The game lights every corner
   separately - that is where its ambient shading comes from - but a texture
   sample cannot be multiplied by a colour without a program that says so.
   Instead the texture carries sixteen copies of itself at descending
   brightness, and each triangle picks the copy nearest its own light.  Flat
   shading per face, then, rather than smooth shading per corner: the look of
   a blockier era, which is not the worst fate for a game made of cubes.

   The hardware setup below - the device, forcewake, the page tables, the
   ring and the batch's state - is the same sequence FLY.N32 uses, and was
   copied from it rather than shared, so that a proven program was not
   disturbed while this one was being written.  The two want merging into one
   engine module once this is known to work.
   ========================================================================== */

#include <nanolibc.h>
#include "nano.h"
#include "render.h"

/* ------------------------------------------------------------ ports, time */
static inline void outl(uint16_t p, uint32_t v) { __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(p)); }
static inline uint32_t inl(uint16_t p) { uint32_t v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static void wbinvd(void) { __asm__ volatile("wbinvd" ::: "memory"); }
static void mfence(void) { __asm__ volatile("mfence" ::: "memory"); }

static uint32_t pci_read(int reg)
{
    outl(0xCF8, 0x80000000u | (2u << 11) | (uint32_t)reg);
    return inl(0xCFC);
}

/* ------------------------------------------------------------ the chip */
static volatile uint32_t *mmio;
static volatile uint64_t *ggtt;
static uint32_t bar0, ggtt_entries;

#define RD(r)    (mmio[(r) / 4])
#define WR(r, v) do { mmio[(r) / 4] = (v); } while (0)

#define FORCEWAKE_MT        0xA188
#define FORCEWAKE_ACK       0x130044
#define RC_CONTROL          0xA090
#define GFX_FLSH_CNTL       0x101008
#define PPAT_LO             0x40E0
#define PPAT_HI             0x40E4
#define RCS                 0x2000
#define RING_TAIL(b)        ((b) + 0x30)
#define RING_HEAD(b)        ((b) + 0x34)
#define RING_START(b)       ((b) + 0x38)
#define RING_CTL(b)         ((b) + 0x3C)
#define RING_MI_MODE(b)     ((b) + 0x9C)
#define RING_IMR(b)         ((b) + 0xA8)
#define RING_HWS(b)         ((b) + 0x80)
#define PLANE_SURF(p)       (0x7019C + (p) * 0x1000)
#define PLANE_SURFLIVE(p)   (0x701AC + (p) * 0x1000)
#define PLANE_CTL(p)        (0x70180 + (p) * 0x1000)

static int poll_until(uint32_t reg, uint32_t mask, uint32_t want, int loops)
{
    while (loops--) if ((RD(reg) & mask) == want) return 0;
    return -1;
}

/* ------------------------------------------------------------ memory */
static uint32_t *ring, *hws, *scratch, *batch, *atlas;
static uint32_t *screens[2], *depth;
static float *vbuf;
static int scr_w, scr_h, scr_p, npages, depth_rows, active_plane = -1;

#define PTE_FLAGS 0x03u

#define RING_GPU    0x07000000u
#define HWS_GPU     0x07001000u
#define SCRATCH_GPU 0x07002000u
#define BATCH_GPU   0x07010000u
#define SCREEN0_GPU 0x10000000u
#define SCREEN1_GPU 0x12000000u
#define DEPTH_GPU   0x14000000u
#define VB_GPU      0x18000000u
#define ATLAS_GPU   0x1C000000u

#define VB_PAGES    2048                /* 8 MB: about 116,000 triangles */
#define VB_MAX_VERT ((VB_PAGES * 4096) / 24)

/* The texture: sixteen copies of the game's atlas, each dimmer than the
   last, then one band of flat colour the sky is painted from. */
#define LIGHT_LEVELS 16
#define ATLAS_W      VG_ATLAS_WIDTH
#define ATLAS_TILE   VG_ATLAS_HEIGHT
#define ATLAS_SKY_Y  (ATLAS_TILE * LIGHT_LEVELS)
#define ATLAS_H      (ATLAS_SKY_Y + ATLAS_TILE)

static uint32_t page_aligned(int pages)
{
    uint32_t p = (uint32_t)malloc((pages + 1) * 4096);
    return p ? (p + 4095) & ~4095u : 0;
}

static void map_pages(uint32_t gpu, uint32_t phys, int pages)
{
    int i;
    for (i = 0; i < pages; i++)
        ggtt[(gpu >> 12) + i] = (uint64_t)((phys + i * 4096) | PTE_FLAGS);
}

static int find_device(void)
{
    uint32_t id = pci_read(0), cls = pci_read(8);
    if ((id & 0xFFFF) != 0x8086 || (cls >> 24) != 0x03) return -1;
    bar0 = pci_read(0x10) & ~0xFu;
    if (!bar0) return -1;
    mmio = (volatile uint32_t *)bar0;
    ggtt = (volatile uint64_t *)(bar0 + (8u << 20));
    ggtt_entries = (2u << 20) / 8;
    return 0;
}

static void find_plane(void)
{
    int p;
    for (p = 0; p < 3; p++)
        if (RD(PLANE_CTL(p)) & 0x80000000u) { active_plane = p; return; }
}

static int wake(void)
{
    WR(FORCEWAKE_MT, (1u << 16) | 1u);
    if (poll_until(FORCEWAKE_ACK, 1u, 1u, 200000) < 0) return -1;
    WR(RC_CONTROL, 0);
    /* Entry 0 of the cache table governs every access through the page
       table; anything but uncached and the engine reads stale pixels. */
    WR(PPAT_LO, 0x00000000u);
    WR(PPAT_HI, 0x00000000u);
    return 0;
}

/* ------------------------------------------------------------ the texture */
/* The game's own atlas generator, from the reference platform, with the
   brightness copies stacked underneath. */
static uint32_t atlas_hash(uint32_t a)
{
    a ^= a >> 16; a *= 0x7feb352du;
    a ^= a >> 15; a *= 0x846ca68bu;
    return a ^ (a >> 16);
}

static float grain(int x, int y, int salt)
{
    return (float)(atlas_hash((uint32_t)(x + y * 731 + salt * 9013)) & 255u) / 255.0f;
}

static float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

static void build_atlas(void)
{
    /* The reference platform's own palette, tile by tile.  The order is the
       atlas's, not the block enum's: water is tile 8 and the torch tile 9,
       which is why a guessed table put an orange sea in a green world. */
    static const float colors[16][3] = {
        {96,151,56},{130,94,61},{137,144,149},{117,79,43},{64,124,59},
        {214,195,144},{130,94,61},{157,120,75},{92,169,190},{126,83,41},
        {0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0}
    };
    int x, y, k, level;
    for (y = 0; y < ATLAS_TILE; y++)
        for (x = 0; x < ATLAS_W; x++) {
            int tile = x / VG_ATLAS_TILE_SIZE, tx = x % VG_ATLAS_TILE_SIZE;
            float n = (grain(tx, y, tile) - 0.5f) * 13 + (grain(tx / 4, y / 4, tile + 17) - 0.5f) * 18;
            float base[3];
            for (k = 0; k < 3; k++) base[k] = colors[tile & 15][k];
            if (tile == 0 || (tile == 6 && y > 25 - (int)(grain(tx / 2, 0, 1) * 4))) {
                for (k = 0; k < 3; k++) base[k] = colors[0][k];
                if (grain(tx, y, 7) > 0.91f) n += 24;
            }
            if (tile == 2) {
                n += (grain(tx / 7, y / 6, 19) - 0.5f) * 14;
                if (grain(tx / 3, y / 3, 12) > 0.85f && y % 6 == 0) n -= 13;
            }
            if (tile == 3 || tile == 9) {
                n += sinf((float)tx * 1.7f + sinf((float)y * 0.2f)) * 14;
                if (tx % 7 == 0) n -= 16;
                if (tile == 9 && y > 25) { base[0] = 245; base[1] = 140; base[2] = 39; }
            }
            if (tile == 4) n += (grain(tx / 3, y / 3, 18) - 0.5f) * 35;
            if (tile == 5) n *= 0.50f;
            if (tile == 7) { float dx = (float)tx - 15.5f, dy = (float)y - 15.5f; n += sinf(sqrtf(dx * dx + dy * dy) * 2.6f) * 17; }
            if (tile == 8) n = sinf((float)y * 0.5f + sinf((float)tx * 0.4f)) * 13 + (grain(tx / 3, y / 2, 3) - 0.5f) * 7;
            for (level = 0; level < LIGHT_LEVELS; level++) {
                float s = (float)(level + 1) / (float)LIGHT_LEVELS;
                uint32_t r = (uint32_t)(clamp01((base[0] + n) / 255.0f) * s * 255);
                uint32_t g = (uint32_t)(clamp01((base[1] + n) / 255.0f) * s * 255);
                uint32_t b = (uint32_t)(clamp01((base[2] + n) / 255.0f) * s * 255);
                atlas[(level * ATLAS_TILE + y) * ATLAS_W + x] =
                    0xFF000000u | (r << 16) | (g << 8) | b;
            }
        }
}

/* The sky's colour changes through the day, so it is painted into a band of
   the texture each frame rather than baked in. */
static void set_sky(uint32_t colour)
{
    int x, y;
    for (y = ATLAS_SKY_Y; y < ATLAS_SKY_Y + 32; y++)
        for (x = 0; x < 32; x++)
            atlas[y * ATLAS_W + x] = colour;
    cache_flush(atlas + (size_t)ATLAS_SKY_Y * ATLAS_W, 32 * ATLAS_W * 4);
}

/* ------------------------------------------------------------ the batch */
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

#define VSIZE 24                        /* x y z w u v, floats */
#define GEN(pipe, op, sub) ((3u << 29) | ((pipe) << 27) | ((op) << 24) | ((sub) << 16))

static const uint32_t ps_kernel[4][4] = {  /* igt's blit.g7a, assembled for gen8 */
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

static uint32_t *cmd;
static void emit(uint32_t v) { *cmd++ = v; }
static void emit_zeros(int n) { while (n--) emit(0); }

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

/* ------------------------------------------------------------ geometry */
static int vcount, truncated;
static float proj_cx, proj_cy, proj_f, near_z, far_z;

static void put_vertex(float x, float y, float z, float w, float u, float v)
{
    float *p = vbuf + (size_t)vcount * 6;
    p[0] = x; p[1] = y; p[2] = z; p[3] = w; p[4] = u; p[5] = v;
    vcount++;
}

/* a corner in the camera's own space, with its place in the texture */
typedef struct { float x, y, z, u, v; } corner;

static void project(const corner *c)
{
    float sx = proj_cx + proj_f * c->x / c->z;
    float sy = proj_cy - proj_f * c->y / c->z;
    float d = (1.0f / near_z - 1.0f / c->z) / (1.0f / near_z - 1.0f / far_z);
    put_vertex(sx * c->z, sy * c->z, d * c->z, c->z, c->u, c->v);
}

static corner mix_corner(const corner *a, const corner *b, float t)
{
    corner o;
    o.x = a->x + (b->x - a->x) * t;
    o.y = a->y + (b->y - a->y) * t;
    o.z = a->z + (b->z - a->z) * t;
    o.u = a->u + (b->u - a->u) * t;
    o.v = a->v + (b->v - a->v) * t;
    return o;
}

/* One plane, kept where a*x + b*y + c*z + d is not negative. */
#define CLIP_MAX 12
static int clip_plane(const corner *in, int n, corner *out,
                      float a, float b, float c, float d)
{
    int i, m = 0;
    for (i = 0; i < n; i++) {
        const corner *p = &in[i], *q = &in[(i + 1) % n];
        float dp = a * p->x + b * p->y + c * p->z + d;
        float dq = a * q->x + b * q->y + c * q->z + d;
        if (dp >= 0 && m < CLIP_MAX) out[m++] = *p;
        if ((dp >= 0) != (dq >= 0) && m < CLIP_MAX)
            out[m++] = mix_corner(p, q, dp / (dp - dq));
    }
    return m;
}

/* Clip against the near plane and the four sides, then emit what is left.
   The near plane alone would be a bug in two directions: dropping a triangle
   that crosses it tears a hole in the wall you are standing against, and
   keeping one whole sends the engine a corner projected half a million
   pixels off the screen, well outside the guardband it is willing to
   rasterise.  Cutting against the sides as well keeps every corner on the
   screen, where the numbers stay small and the engine stays happy. */
static void emit_clipped(corner *in, int n)
{
    corner a[CLIP_MAX], b[CLIP_MAX];
    int m, i;

    m = clip_plane(in, n, a, 0, 0, 1, -near_z);
    if (m < 3) return;
    m = clip_plane(a, m, b, proj_f, 0, proj_cx, 0);                     /* left */
    if (m < 3) return;
    m = clip_plane(b, m, a, -proj_f, 0, (float)scr_w - proj_cx, 0);     /* right */
    if (m < 3) return;
    m = clip_plane(a, m, b, 0, -proj_f, proj_cy, 0);                    /* top */
    if (m < 3) return;
    m = clip_plane(b, m, a, 0, proj_f, (float)scr_h - proj_cy, 0);      /* bottom */
    if (m < 3) return;

    if (vcount + (m - 2) * 3 > VB_MAX_VERT) { truncated = 1; return; }
    for (i = 2; i < m; i++) {            /* a fan across whatever survived */
        project(&a[0]);
        project(&a[i - 1]);
        project(&a[i]);
    }
}

/* ------------------------------------------------------------ the frame */
static uint32_t ring_tail;

static int run_frame(uint32_t stamp)
{
    uint32_t pos = ring_tail & 0xFFFu;
    int spins = 0;
    ring[pos / 4] = 0x18800001u;
    ring[pos / 4 + 1] = BATCH_GPU;
    ring[pos / 4 + 2] = 0;
    ring[pos / 4 + 3] = 0;
    cache_flush(ring + pos / 4, 16);
    scratch[0] = 0;
    cache_flush(scratch, 64);
    mfence();
    ring_tail = (ring_tail + 16) & 0xFFFu;
    WR(RING_TAIL(RCS), ring_tail);
    for (;;) {
        cache_flush(scratch, 64);
        mfence();
        if (scratch[0] == stamp) return 0;
        if (++spins > 40000000) return -1;
    }
}

static uint32_t build_frame_batch(uint32_t target_gpu, uint32_t stamp, int bg)
{
    uint8_t *b = (uint8_t *)batch;

    surface_state((uint32_t *)(b + OFF_SS_RT), target_gpu, scr_w, scr_h, scr_p * 4);
    surface_state((uint32_t *)(b + OFF_SS_TEX), ATLAS_GPU, ATLAS_W, ATLAS_H, ATLAS_W * 4);

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
    emit(DEPTH_GPU); emit(0);
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
    /* the sky first, laying depth 1 over the whole screen */
    emit(GEN(3, 0, 0x4E) | 1); emit(3u); emit(0);
    emit(GEN(3, 3, 0) | 5); emit(0); emit((uint32_t)bg); emit(0); emit(1); emit(0); emit(0);
    /* then the world, sorted by depth */
    if (vcount > bg) {
        emit(GEN(3, 0, 0x4E) | 1); emit(3u | (2u << 5)); emit(0);
        emit(GEN(3, 3, 0) | 5); emit(0); emit((uint32_t)(vcount - bg)); emit((uint32_t)bg); emit(1); emit(0); emit(0);
    }
    emit(GEN(3, 2, 0) | 4);
    emit((1u << 12) | (1u << 0) | (1u << 20) | (1u << 14) | (1u << 24));
    emit(SCRATCH_GPU); emit(0);
    emit(stamp); emit(0);
    emit(0x05000000u);
    cache_flush(batch, 4 * 4096);
    mfence();
    return (uint32_t)((uint8_t *)cmd - (uint8_t *)batch);
}

/* ------------------------------------------------------------ the interface */
static int cur_screen;
static uint32_t stamp = 1;

int vox_render_open(void)
{
    struct vbe_info info;
    struct vbe_mode m;
    int mode = -1, i, best = 0;

    if (find_device() != 0) return -1;
    if (wake() != 0) return -1;

    if (sys_vbe_info(&info) != 0) return -1;
    /* The widest mode that is not the panel's own.  This screen is 3200 by
       1800, which is 5.8 million pixels to fill and transform for every
       frame; something nearer 1280 leaves the game playable, and the engine
       stretches it to the panel anyway. */
    for (i = 0; i < info.mode_count; i++) {
        struct vbe_mode t;
        if (sys_vbe_mode(info.modes[i], &t) != 0 || !t.framebuffer || t.bpp != 32) continue;
        if (t.width < 800 || t.width > 1400) continue;
        if (t.width > best) { best = t.width; mode = info.modes[i]; m = t; }
    }
    if (mode < 0) return -1;
    scr_w = m.width;
    scr_h = m.height;
    scr_p = m.pitch / 4;
    npages = (m.pitch * m.height + 4095) / 4096;
    depth_rows = (scr_h + 31) & ~31;

    ring = (uint32_t *)page_aligned(1);
    hws = (uint32_t *)page_aligned(1);
    scratch = (uint32_t *)page_aligned(1);
    batch = (uint32_t *)page_aligned(4);
    atlas = (uint32_t *)page_aligned((ATLAS_W * ATLAS_H * 4 + 4095) / 4096);
    screens[0] = (uint32_t *)page_aligned(npages);
    screens[1] = (uint32_t *)page_aligned(npages);
    depth = (uint32_t *)page_aligned((scr_p * 4 * depth_rows + 4095) / 4096);
    vbuf = (float *)page_aligned(VB_PAGES);
    if (!ring || !hws || !scratch || !batch || !atlas || !screens[0] || !screens[1] || !depth || !vbuf)
        return -1;

    memset(ring, 0, 4096);
    memset(hws, 0, 4096);
    memset(scratch, 0, 4096);
    memset(batch, 0, 4 * 4096);
    memset(screens[0], 0, (size_t)m.pitch * m.height);
    memset(screens[1], 0, (size_t)m.pitch * m.height);
    memset(depth, 0, (size_t)scr_p * 4 * depth_rows);
    build_atlas();
    wbinvd();

    map_pages(RING_GPU, (uint32_t)ring, 1);
    map_pages(HWS_GPU, (uint32_t)hws, 1);
    map_pages(SCRATCH_GPU, (uint32_t)scratch, 1);
    map_pages(BATCH_GPU, (uint32_t)batch, 4);
    map_pages(ATLAS_GPU, (uint32_t)atlas, (ATLAS_W * ATLAS_H * 4 + 4095) / 4096);
    map_pages(SCREEN0_GPU, (uint32_t)screens[0], npages);
    map_pages(SCREEN1_GPU, (uint32_t)screens[1], npages);
    map_pages(DEPTH_GPU, (uint32_t)depth, (scr_p * 4 * depth_rows + 4095) / 4096);
    map_pages(VB_GPU, (uint32_t)vbuf, VB_PAGES);
    WR(GFX_FLSH_CNTL, 1);

    /* the ring */
    WR(RING_IMR(RCS), 0xFFFFFFFFu);
    WR(RING_MI_MODE(RCS), (1u << 24) | (1u << 8));
    poll_until(RING_MI_MODE(RCS), 1u << 9, 1u << 9, 200000);
    WR(RING_HWS(RCS), HWS_GPU);
    WR(RING_HEAD(RCS), 0);
    WR(RING_TAIL(RCS), 0);
    WR(RING_START(RCS), RING_GPU);
    WR(RING_CTL(RCS), 1u);
    if (!(RD(RING_CTL(RCS)) & 1)) return -1;
    WR(RING_MI_MODE(RCS), 1u << 24);

    if (sys_set_vbe_mode(mode, 1) != 0) return -1;
    find_plane();
    if (active_plane < 0) { sys_set_video_mode(3); return -1; }
    WR(PLANE_SURF(active_plane), SCREEN0_GPU);
    ring_tail = RD(RING_TAIL(RCS));
    build_state();
    sys_logf("VOXEL: %ux%u, atlas %ux%u, %d corners of room", scr_w, scr_h, ATLAS_W, ATLAS_H, VB_MAX_VERT);
    return 0;
}

void vox_render(const vg_frame *f, const char *notice)
{
    const vg_camera *c = &f->camera;
    float angle = (f->time_of_day - 0.25f) * 6.2831853f;
    float sun_x = cosf(angle) * 0.65f, sun_y = sinf(angle), sun_z = -cosf(angle) * 0.7599342f;
    float day = clamp01(sun_y * 2.3f + 0.25f);
    float dusk = clamp01(1 - fabsf(sun_y) * 3) * (1 - day * 0.45f);
    float sky_r = (0.03f + (0.69f - 0.03f) * day) + dusk * 0.22f;
    float sky_g = (0.047f + (0.80f - 0.047f) * day) - dusk * 0.16f;
    float sky_b = (0.10f + (0.85f - 0.10f) * day) - dusk * 0.23f;
    float bgu = 16.0f / (float)ATLAS_W, bgv = ((float)ATLAS_SKY_Y + 16.0f) / (float)ATLAS_H;
    size_t i;
    int target = cur_screen ^ 1, bg;
    (void)notice;

    if (f->underwater) { sky_r = 0.035f; sky_g = 0.20f; sky_b = 0.29f; }
    set_sky(0xFF000000u | ((uint32_t)(clamp01(sky_r) * 255) << 16) |
            ((uint32_t)(clamp01(sky_g) * 255) << 8) | (uint32_t)(clamp01(sky_b) * 255));

    near_z = c->near_plane;
    far_z = c->far_plane;
    proj_cx = scr_w / 2.0f;
    proj_cy = scr_h / 2.0f;
    proj_f = (scr_h / 2.0f) / (sinf(c->vertical_fov * 0.5f) / cosf(c->vertical_fov * 0.5f));

    vcount = 0;
    truncated = 0;
    /* the sky: two triangles at the far plane, which also lay the depth down */
    put_vertex(0, 0, 1, 1, bgu, bgv);
    put_vertex((float)scr_w, 0, 1, 1, bgu, bgv);
    put_vertex((float)scr_w, (float)scr_h, 1, 1, bgu, bgv);
    put_vertex(0, 0, 1, 1, bgu, bgv);
    put_vertex((float)scr_w, (float)scr_h, 1, 1, bgu, bgv);
    put_vertex(0, (float)scr_h, 1, 1, bgu, bgv);
    bg = vcount;

    for (i = 0; i < f->triangle_count; i++) {
        const vg_triangle *t = &f->triangles[i];
        corner in[3];
        float ax = t->v[1].position.x - t->v[0].position.x;
        float ay = t->v[1].position.y - t->v[0].position.y;
        float az = t->v[1].position.z - t->v[0].position.z;
        float bx = t->v[2].position.x - t->v[0].position.x;
        float by = t->v[2].position.y - t->v[0].position.y;
        float bz = t->v[2].position.z - t->v[0].position.z;
        float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
        float len = sqrtf(nx * nx + ny * ny + nz * nz);
        float diffuse, lum = 0, vrow;
        int j, level;

        if (len > 0) { nx /= len; ny /= len; nz /= len; }
        diffuse = clamp01(nx * sun_x + ny * sun_y + nz * sun_z);
        for (j = 0; j < 3; j++) {
            float base = (0.13f + day * (0.46f + 0.40f * diffuse)) * t->v[j].ambient;
            float torch = t->v[j].block_light * t->v[j].block_light * 1.35f;
            lum += base + torch * 0.6f;
        }
        lum /= 3.0f;
        level = (int)(clamp01(lum) * (LIGHT_LEVELS - 1) + 0.5f);
        vrow = (float)(level * ATLAS_TILE) / (float)ATLAS_H;

        for (j = 0; j < 3; j++) {
            float dx = t->v[j].position.x - c->position.x;
            float dy = t->v[j].position.y - c->position.y;
            float dz = t->v[j].position.z - c->position.z;
            in[j].x = dx * c->right.x + dy * c->right.y + dz * c->right.z;
            in[j].y = dx * c->up.x + dy * c->up.y + dz * c->up.z;
            in[j].z = dx * c->forward.x + dy * c->forward.y + dz * c->forward.z;
            in[j].u = t->v[j].u;
            in[j].v = vrow + t->v[j].v * (float)ATLAS_TILE / (float)ATLAS_H;
        }
        emit_clipped(in, 3);
    }

    cache_flush(vbuf, (uint32_t)vcount * VSIZE);
    mfence();
    build_frame_batch(target ? SCREEN1_GPU : SCREEN0_GPU, stamp, bg);
    if (run_frame(stamp) == 0) {
        cur_screen = target;
        WR(PLANE_SURF(active_plane), target ? SCREEN1_GPU : SCREEN0_GPU);
    }
    stamp++;
    if (truncated) sys_log("VOXEL: more corners than there is room for");
}

void vox_render_close(void)
{
    if (active_plane >= 0) WR(PLANE_SURF(active_plane), 0);
    sys_set_video_mode(3);
}
