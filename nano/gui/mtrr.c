/* mtrr.c - tell the processor the framebuffer is write-combining.
 *
 * The processor decides how to treat each region of memory from its
 * memory type range registers.  The firmware marks the card's memory
 * "uncacheable", which is correct for registers but makes every pixel
 * write its own bus transaction: on one laptop that came to 20 MB/s, a
 * third of a second for one screen.  "Write-combining" lets the
 * processor gather writes into 64-byte bursts, which is what a graphics
 * driver does for a framebuffer and is many times faster.
 *
 * Firmware usually describes memory as one big write-back range for all
 * the RAM with uncacheable holes punched out for the devices, and a
 * write-combining range laid over a write-back one is an overlap Intel
 * calls undefined.  So instead of adding to that table this redraws it:
 * the RAM below 4 GB as write-back blocks that stop where the holes
 * begin, nothing for the holes (the default type, uncacheable, covers
 * them), and then the framebuffer as write-combining on ground nothing
 * else claims.  A 32-bit system never touches memory above 4 GB, so that
 * is left to the default too.  The firmware's table is saved and put
 * back exactly on the way out.
 *
 * Every change follows the sequence Intel prescribes: interrupts off,
 * caches off and flushed, the range registers disabled, the new values
 * written, everything back on.  Anything unexpected in the table means
 * the whole thing is left alone, and the monitor says why.
 */
#include <nanolibc.h>
#include "nano.h"
#include "draw.h"

#define MSR_MTRRCAP      0xFE
#define MSR_MTRRDEFTYPE  0x2FF
#define MSR_PHYSBASE(i)  (0x200 + 2 * (i))
#define MSR_PHYSMASK(i)  (0x201 + 2 * (i))
#define TYPE_UC          0
#define TYPE_WC          1
#define TYPE_WB          6
#define MAX_REGS         16
#define FOUR_GB          0x100000000ull

const char *fb_wc_note = "not attempted";   /* the outcome, for the monitor */
struct mtrr_entry mtrr_table[16];           /* the firmware's table, for the monitor */
int mtrr_count, mtrr_default = -1;

static char note_buf[96];
static int saved_n = -1;                    /* the firmware's registers, to put back */
static uint64_t saved_base[MAX_REGS], saved_mask[MAX_REGS];

static void note(const char *s) { fb_wc_note = s; sys_log(s); }
static void notef(const char *fmt, int a, int b)
{
    snprintf(note_buf, sizeof note_buf, fmt, a, b);
    note(note_buf);
}

static uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static void wrmsr(uint32_t msr, uint64_t v)
{
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

static uint32_t cpuid_eax(uint32_t leaf)
{
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(leaf), "c"(0));
    return a;
}

static uint32_t cpuid_edx(uint32_t leaf)
{
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(leaf), "c"(0));
    return d;
}

/* the physical address width, for the mask register */
static int phys_bits(void)
{
    if (cpuid_eax(0x80000000u) >= 0x80000008u)
        return (int)(cpuid_eax(0x80000008u) & 0xFF);
    return 36;
}

/* Everything around a rewrite of the range registers.  Caches are turned
   off and emptied first, so nothing stale survives the change of type. */
static void with_caches_off(void (*fn)(void *), void *arg)
{
    uint32_t cr0;
    uint64_t def;
    __asm__ volatile("cli");
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %0, %%cr0" : : "r"((cr0 | 0x40000000u) & ~0x20000000u));   /* CD=1, NW=0 */
    __asm__ volatile("wbinvd");
    def = rdmsr(MSR_MTRRDEFTYPE);
    wrmsr(MSR_MTRRDEFTYPE, def & ~(1ull << 11));                                     /* MTRRs off */
    fn(arg);
    wrmsr(MSR_MTRRDEFTYPE, def);                                                     /* and on */
    __asm__ volatile("wbinvd");
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
    __asm__ volatile("sti");
}

/* a whole table, written in one go */
struct table { int n; uint64_t base[MAX_REGS], mask[MAX_REGS]; };

static void write_table(void *arg)
{
    struct table *t = arg;
    int i;
    for (i = 0; i < t->n; i++) {
        wrmsr(MSR_PHYSMASK(i), 0);              /* disable before changing the base */
        wrmsr(MSR_PHYSBASE(i), t->base[i]);
        wrmsr(MSR_PHYSMASK(i), t->mask[i]);
    }
}

/* Make [fb, fb+size) write-combining.  0 on success; the monitor and the
   log say what happened either way. */
int fb_write_combine(uint32_t fb, uint32_t size)
{
    uint64_t cap, def, addr_mask, span, ram_top = 0, cur;
    int n, i, bits, blocks = 0, fb_in_wb = 0;
    struct table t;

    note("framebuffer: looking at the MTRRs");
    if (!(cpuid_edx(1) & (1 << 12))) { note("framebuffer: no MTRRs on this processor"); return -1; }
    cap = rdmsr(MSR_MTRRCAP);
    def = rdmsr(MSR_MTRRDEFTYPE);
    n = (int)(cap & 0xFF);
    if (n > MAX_REGS) n = MAX_REGS;
    if (!(cap & (1 << 10))) { note("framebuffer: processor cannot write-combine"); return -1; }
    if (!(def & (1 << 11)))  { note("framebuffer: MTRRs are disabled; left alone"); return -1; }
    bits = phys_bits();
    addr_mask = ((1ull << bits) - 1) & ~0xFFFull;
    mtrr_default = (int)(def & 0xFF);
    sys_logf("mtrr: default type %d, %d variable registers, %d address bits", mtrr_default, n, bits);

    /* the screen's range: a power of two, aligned to itself */
    for (span = 0x1000; span < size; span <<= 1) ;
    if (fb & (span - 1)) {
        notef("framebuffer: %08X is not aligned to a %u KB range; left alone", (int)fb, (int)(span >> 10));
        return -1;
    }

    /* ---- read the firmware's table, and keep it ---- */
    mtrr_count = 0;
    for (i = 0; i < n; i++) {
        uint64_t b = rdmsr(MSR_PHYSBASE(i)), m = rdmsr(MSR_PHYSMASK(i)), sz;
        saved_base[i] = b;
        saved_mask[i] = m;
        if (!(m & (1 << 11))) continue;
        sz = (~(m & addr_mask) & addr_mask) + 0x1000;
        sys_logf("mtrr %d: %08X%08X size %08X%08X type %d", i,
                 (uint32_t)((b & addr_mask) >> 32), (uint32_t)(b & addr_mask),
                 (uint32_t)(sz >> 32), (uint32_t)sz, (int)(b & 0xFF));
        if (mtrr_count < 16) {
            mtrr_table[mtrr_count].reg = i;
            mtrr_table[mtrr_count].base = b & addr_mask;
            mtrr_table[mtrr_count].size = sz;
            mtrr_table[mtrr_count].type = (int)(b & 0xFF);
            mtrr_count++;
        }
    }
    saved_n = n;

    /* ---- is it the layout we know how to redraw? ----
       default uncacheable; write-back ranges that begin at 0; uncacheable
       holes below 4 GB whose lowest edge is where RAM ends; the screen in
       a hole, not in RAM. */
    if (mtrr_default != TYPE_UC) { note("framebuffer: default type is not uncacheable; left alone"); return -1; }
    for (i = 0; i < mtrr_count; i++) {
        struct mtrr_entry *e = &mtrr_table[i];
        uint64_t end = e->base + e->size;
        if (e->type == TYPE_WC && e->base <= fb && fb < end) {
            note("framebuffer: already write-combining");
            return 0;
        }
        if (e->type == TYPE_WB) {
            if (e->base != 0) { notef("framebuffer: write-back range %d does not start at 0; left alone", e->reg, 0); return -1; }
            if (e->base <= fb && fb < end) fb_in_wb = 1;
            if (ram_top == 0 || end < ram_top) ram_top = end;
        } else if (e->type == TYPE_UC) {
            if (e->base >= FOUR_GB) continue;   /* above anything we touch */
            if (e->base < 0x100000) { notef("framebuffer: uncacheable range %d is low; left alone", e->reg, 0); return -1; }
            if (ram_top == 0 || e->base < ram_top) ram_top = e->base;
        } else {
            notef("framebuffer: range %d has type %d; left alone", e->reg, e->type);
            return -1;
        }
    }
    if (!fb_in_wb) {
        /* nothing describes it but the default: the simple case */
        ram_top = 0;
    }
    if (ram_top > FOUR_GB) ram_top = FOUR_GB;
    if (ram_top && fb < ram_top) { note("framebuffer: the screen lies inside RAM's range; left alone"); return -1; }

    /* ---- the new table: RAM below 4 GB in write-back blocks, then the screen ---- */
    t.n = n;
    for (i = 0; i < n; i++) { t.base[i] = 0; t.mask[i] = 0; }
    for (cur = 0; cur < ram_top; ) {
        uint64_t blk = 0x1000;
        while (blk * 2 <= ram_top - cur && !(cur & (blk * 2 - 1))) blk *= 2;
        if (blocks >= n - 1) { notef("framebuffer: RAM needs more than %d blocks; left alone", n - 1, 0); return -1; }
        t.base[blocks] = cur | TYPE_WB;
        t.mask[blocks] = (~(blk - 1) & addr_mask) | (1 << 11);
        blocks++;
        cur += blk;
    }
    t.base[blocks] = ((uint64_t)fb & addr_mask) | TYPE_WC;
    t.mask[blocks] = (~(span - 1) & addr_mask) | (1 << 11);

    with_caches_off(write_table, &t);
    sys_logf("framebuffer: RAM to %08X%08X in %d write-back blocks; %u MB at %08X write-combining",
             (uint32_t)(ram_top >> 32), (uint32_t)ram_top, blocks, (unsigned)(span >> 20), fb);
    notef("framebuffer: %u MB now write-combining; RAM redrawn in %d blocks", (int)(span >> 20), blocks);
    return 0;
}

/* the firmware's table back, on the way out */
void fb_write_combine_undo(void)
{
    struct table t;
    int i;
    if (saved_n < 0) return;
    t.n = saved_n;
    for (i = 0; i < saved_n; i++) { t.base[i] = saved_base[i]; t.mask[i] = saved_mask[i]; }
    with_caches_off(write_table, &t);
    saved_n = -1;
}
