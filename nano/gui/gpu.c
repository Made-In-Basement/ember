/* gpu.c - the display engine of the Intel graphics chip, put to two uses.
 *
 * The firmware sets the screen mode and scans out of memory it stole for
 * itself, which the processor can only reach through a slow window.  The
 * display engine, though, will scan out of any memory named in the GPU's
 * page table.  So the desktop's own back buffer is entered into that table
 * and the display is pointed at it: drawing happens with ordinary cached
 * writes, and "presenting" a frame is flushing the changed cache lines to
 * memory, where the display reads.  No pixel is copied.
 *
 * The second use is the cursor plane: a small sprite the display engine
 * composites itself, so moving the pointer is a register write and no
 * repaint at all.
 *
 * Learned on a Broadwell (HD Graphics 5300) with GPU.N32, one probe at a
 * time; the offsets are Gen8's, as the Linux i915 driver names them.
 * Anything unexpected makes gpu_open() decline, and the desktop falls back
 * to copying frames through the firmware's window.
 */
#include <nanolibc.h>
#include "nano.h"
#include "gpu.h"

static volatile uint32_t *mmio;         /* the register block, BAR 0 */
static volatile uint64_t *ggtt;         /* the global graphics page table */
static uint32_t ggtt_entries;
static int plane = -1;                  /* the display plane the firmware drives */
static uint32_t firmware_surf, firmware_stride;
static uint32_t *bufs[3];               /* the desktop's buffers: one shown, one asked for, one drawn */
static int scr_pitch;                   /* bytes a row */
static int pending = -1;                /* the buffer a flip was asked for, until it shows */
static uint32_t *cursor_px;             /* 64x64 ARGB, four pages */
static int cur_hot_x, cur_hot_y, cur_on, active, has_clflushopt;
static char note_buf[120] = "no display engine driver";

static const uint32_t buf_gpu[3] = { 0x10000000u, 0x12000000u, 0x14000000u };   /* the buffers' GPU addresses */
#define CURSOR_GPU      0x0F000000u
#define PTE_FLAGS       0x03u           /* present, writable (the cache index is not honoured) */
#define WBINVD_ABOVE    (2u << 20)      /* a bigger flush than this: the whole cache, at once */

#define RD(o)        (mmio[(o) / 4])
#define WR(o, v)     (mmio[(o) / 4] = (v))
#define GFX_FLSH_CNTL       0x101008
#define PLANE_CNTR(p)       (0x70180 + (p) * 0x1000)
#define PLANE_STRIDE(p)     (0x70188 + (p) * 0x1000)
#define PLANE_SURF(p)       (0x7019C + (p) * 0x1000)
#define PLANE_SURFLIVE(p)   (0x701AC + (p) * 0x1000)   /* the surface being scanned right now */
#define CUR_CNTR(p)         (0x70080 + (p) * 0x1000)
#define CUR_BASE(p)         (0x70084 + (p) * 0x1000)
#define CUR_POS(p)          (0x70088 + (p) * 0x1000)
#define CURSOR_64_ARGB      0x27

static inline void outl(uint16_t p, uint32_t v) { __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(p)); }
static inline uint32_t inl(uint16_t p) { uint32_t v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static void wbinvd(void) { __asm__ volatile("wbinvd" ::: "memory"); }
static void mfence(void) { __asm__ volatile("mfence" ::: "memory"); }

static uint32_t pci_read(int reg)       /* device 2, function 0: the graphics */
{
    outl(0xCF8, 0x80000000u | (2 << 11) | (reg & 0xFC));
    return inl(0xCFC);
}

static void decline(const char *why)
{
    snprintf(note_buf, sizeof note_buf, "display engine not used: %s", why);
    sys_logf("gpu: %s", note_buf);
}

int gpu_active(void) { return active; }
const char *gpu_note(void) { return note_buf; }

/* for the 3D service: the registers, the table, a mapping, a buffer's address */
volatile uint32_t *gpu_regs(void) { return active ? mmio : 0; }
volatile uint64_t *gpu_table(void) { return active ? ggtt : 0; }
void gpu_map(uint32_t gpu, uint32_t phys, int pages)
{
    int i;
    if (!active) return;
    for (i = 0; i < pages; i++) ggtt[(gpu >> 12) + i] = (uint64_t)((phys + i * 4096) | PTE_FLAGS);
}
uint32_t gpu_buffer_address(const uint32_t *buffer)
{
    int i;
    for (i = 0; i < 3; i++) if (bufs[i] == buffer) return buf_gpu[i];
    return 0;
}

/* ---------------------------------------------------------------- open */
int gpu_open(uint32_t *a, uint32_t *b, uint32_t *c, int w, int h, int pitch)
{
    uint32_t id, cls, cmd, b0lo, b0hi, ggc, bdsm, ggms, bar0, npages, i, cntr;
    uint32_t halves[3] = { 2u << 20, 4u << 20, 8u << 20 };
    int p, k;

    active = 0;
    if (((uint32_t)a & 0xFFF) || ((uint32_t)b & 0xFFF) || ((uint32_t)c & 0xFFF) || (pitch & 63)) { decline("buffers not page aligned or pitch not a multiple of 64"); return -1; }

    id = pci_read(0);
    cls = pci_read(8);
    cmd = pci_read(4);
    if ((id & 0xFFFF) != 0x8086 || (cls >> 24) != 0x03) { decline("no Intel display device at PCI 0:2.0"); return -1; }
    /* Every register below was read off a Broadwell, and the checks further
       down would not catch a later generation that keeps the names and moves
       the furniture.  Anything else gets its frames copied instead, which is
       slower and certain - and on a machine that is not ours, certain wins. */
    if ((id >> 16) != 0x1616 && (id >> 16) != 0x161E) { decline("not the Broadwell GT2 this was written for"); return -1; }
    if (!(cmd & 2)) { decline("its memory decoding is off"); return -1; }
    b0lo = pci_read(0x10);
    b0hi = pci_read(0x14);
    if (b0hi) { decline("registers above 4 GB"); return -1; }
    bar0 = b0lo & ~0xFu;
    ggc = pci_read(0x50);
    bdsm = pci_read(0x5C) & 0xFFF00000u;
    ggms = (ggc >> 6) & 3;
    if (!ggms) { decline("no graphics page table"); return -1; }
    ggtt_entries = (1u << (20 + ggms)) / 8;
    mmio = (volatile uint32_t *)bar0;

    /* the table sits in the second half of BAR0; the half is the one whose
       first entries map the stolen memory, as the firmware leaves it */
    ggtt = 0;
    for (i = 0; i < 3 && !ggtt; i++) {
        volatile uint64_t *t = (volatile uint64_t *)(bar0 + halves[i]);
        uint64_t e0 = t[0], e1 = t[1];
        if ((e0 & 1) && ((uint32_t)e0 & 0xFFFFF000u) == bdsm && ((uint32_t)e1 & 0xFFFFF000u) == bdsm + 4096)
            ggtt = t;
    }
    if (!ggtt) { decline("page table not where expected"); return -1; }

    /* the plane the firmware is driving, in the format we draw */
    for (p = 0; p < 3 && plane < 0; p++)
        if (RD(PLANE_CNTR(p)) >> 31) plane = p;
    if (plane < 0) { decline("no display plane is on"); return -1; }
    cntr = RD(PLANE_CNTR(plane));
    if (((cntr >> 26) & 0xF) != 6) { plane = -1; decline("plane is not 32-bit XRGB"); return -1; }
    if (cntr & (1 << 10)) { plane = -1; decline("plane surface is tiled"); return -1; }

    npages = ((uint32_t)pitch * h + 4095) / 4096;
    if ((buf_gpu[2] >> 12) + npages > ggtt_entries || npages > (buf_gpu[1] - buf_gpu[0]) / 4096) {
        plane = -1; decline("page table too small"); return -1;
    }

    /* the cursor's four pages */
    cursor_px = (uint32_t *)(((uint32_t)malloc(5 * 4096) + 4095) & ~4095u);
    if (!cursor_px) { plane = -1; decline("no memory"); return -1; }
    memset(cursor_px, 0, 4 * 4096);

    /* both buffers black, and in memory, before the display sees them */
    bufs[0] = a;
    bufs[1] = b;
    bufs[2] = c;
    memset(a, 0, (size_t)pitch * h);
    memset(b, 0, (size_t)pitch * h);
    memset(c, 0, (size_t)pitch * h);
    wbinvd();
    for (k = 0; k < 3; k++)
        for (i = 0; i < npages; i++)
            ggtt[(buf_gpu[k] >> 12) + i] = (uint64_t)(((uint32_t)bufs[k] + i * 4096) | PTE_FLAGS);
    for (i = 0; i < 4; i++)
        ggtt[(CURSOR_GPU >> 12) + i] = (uint64_t)(((uint32_t)cursor_px + i * 4096) | PTE_FLAGS);
    WR(GFX_FLSH_CNTL, 1);

    {
        uint32_t ra, rb, rc, rd;
        __asm__ volatile("cpuid" : "=a"(ra), "=b"(rb), "=c"(rc), "=d"(rd) : "a"(7), "c"(0));
        has_clflushopt = (rb >> 23) & 1;
    }

    /* the flip: stride first if ours differs, then the surface, which arms it */
    firmware_surf = RD(PLANE_SURF(plane));
    firmware_stride = RD(PLANE_STRIDE(plane));
    if (firmware_stride != (uint32_t)pitch) WR(PLANE_STRIDE(plane), (uint32_t)pitch);
    WR(PLANE_SURF(plane), buf_gpu[0]);
    pending = 0;
    scr_pitch = pitch;
    active = 1;
    snprintf(note_buf, sizeof note_buf, "Intel %04X: the display reads the desktop's memory; the pointer is a sprite", id >> 16);
    sys_logf("gpu: device %04X, table at BAR0+%u MB, %u entries; plane %c scans %u pages at %08X/%08X/%08X (was %08X), stride %u",
             id >> 16, halves[i - 1] >> 20, ggtt_entries, 'A' + plane, npages, buf_gpu[0], buf_gpu[1], buf_gpu[2], firmware_surf, pitch);
    return 0;
}

void gpu_close(void)
{
    if (!active) return;
    WR(CUR_CNTR(plane), 0);
    WR(CUR_BASE(plane), 0);
    if (firmware_stride != (uint32_t)scr_pitch) WR(PLANE_STRIDE(plane), firmware_stride);
    WR(PLANE_SURF(plane), firmware_surf);
    active = 0;
    sys_log("gpu: the display back on the firmware's surface");
}

/* ---------------------------------------------------------------- flush */
/* Lines written by the processor sit in its caches until evicted; the
   display reads memory.  A small region is flushed line by line; a large
   one is cheaper to settle by writing back every cache at once. */
static void flush_lines(const void *p, uint32_t n)
{
    const char *c = (const char *)((uint32_t)p & ~63u), *e = (const char *)p + n;
    if (has_clflushopt) { for (; c < e; c += 64) __asm__ volatile("clflushopt (%0)" : : "r"(c) : "memory"); }
    else { for (; c < e; c += 64) __asm__ volatile("clflush (%0)" : : "r"(c) : "memory"); }
}

void gpu_flush(const uint32_t *buf, int x, int y, int w, int h)
{
    int j;
    if (!active || w <= 0 || h <= 0) return;
    if (!nx_has_clflush || (unsigned long)w * h * 4 >= WBINVD_ABOVE) { wbinvd(); return; }
    for (j = 0; j < h; j++)
        flush_lines((const uint8_t *)buf + (size_t)(y + j) * scr_pitch + (size_t)x * 4, (uint32_t)w * 4);
    mfence();
}

/* ---------------------------------------------------------------- flipping */
/* The surface register takes effect at the next vertical blank; the live
   register says which surface is being scanned, so a buffer is never drawn
   into while it is still on the screen. */
void gpu_flip(int which)
{
    if (!active || which < 0 || which > 2) return;
    WR(PLANE_SURF(plane), buf_gpu[which]);
    pending = which;
}

/* which buffer the display is scanning right now, or -1 */
int gpu_shown(void)
{
    uint32_t live;
    int i;
    if (!active) return -1;
    live = RD(PLANE_SURFLIVE(plane)) & ~0xFFFu;
    for (i = 0; i < 3; i++) if (live == buf_gpu[i]) return i;
    return -1;
}

int gpu_flip_done(void)
{
    if (!active || pending < 0) return 1;
    return (RD(PLANE_SURFLIVE(plane)) & ~0xFFFu) == buf_gpu[pending];
}

/* ---------------------------------------------------------------- cursor */
static void cursor_arm(void)
{
    WR(CUR_CNTR(plane), cur_on ? CURSOR_64_ARGB : 0);
    WR(CUR_BASE(plane), CURSOR_GPU);                /* this write applies the others */
}

void gpu_cursor_image(const uint32_t *argb, int w, int h, int hot_x, int hot_y)
{
    int x, y;
    if (!active) return;
    if (w > 64) w = 64;
    if (h > 64) h = 64;
    memset(cursor_px, 0, 64 * 64 * 4);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) cursor_px[y * 64 + x] = argb[y * w + x];
    flush_lines(cursor_px, 64 * 64 * 4);
    mfence();
    cur_hot_x = hot_x;
    cur_hot_y = hot_y;
    cursor_arm();
}

void gpu_cursor_move(int x, int y)
{
    uint32_t pos;
    if (!active) return;
    x -= cur_hot_x;
    y -= cur_hot_y;
    pos = (x < 0 ? 0x8000u | (uint32_t)(-x & 0x1FFF) : (uint32_t)(x & 0x1FFF))
        | (y < 0 ? 0x80000000u | ((uint32_t)(-y & 0x1FFF) << 16) : ((uint32_t)(y & 0x1FFF) << 16));
    WR(CUR_POS(plane), pos);
    WR(CUR_BASE(plane), CURSOR_GPU);
}

void gpu_cursor_show(int on)
{
    if (!active) return;
    cur_on = on;
    cursor_arm();
}
