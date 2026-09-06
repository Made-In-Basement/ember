/* mtrr.c - tell the processor the framebuffer is write-combining.
 *
 * The processor decides how to treat each region of memory from its
 * memory type range registers.  The firmware marks the card's memory
 * "uncacheable", which is correct for registers but makes every pixel
 * write its own bus transaction.  "Write-combining" lets the processor
 * gather writes into 64-byte bursts, which is what a graphics driver
 * does for a framebuffer and makes pushing pixels several times faster.
 *
 * The change follows the sequence Intel prescribes: interrupts off,
 * caches off and flushed, the range registers disabled, the new range
 * written, everything back on.  It refuses rather than guesses when a
 * firmware range already covers the framebuffer as uncacheable (that
 * would win over ours) or when no register is free.
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

static int used_reg = -1;               /* the register we took, to give back */
const char *fb_wc_note = "not attempted";   /* the outcome, for the monitor */
static char note_buf[96];

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

/* Everything around the write of a range register.  Caches are turned
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

struct range { int reg; uint64_t base, mask; };

static void write_range(void *arg)
{
    struct range *r = arg;
    wrmsr(MSR_PHYSBASE(r->reg), r->base);
    wrmsr(MSR_PHYSMASK(r->reg), r->mask);
}

static void clear_range(void *arg)
{
    struct range *r = arg;
    wrmsr(MSR_PHYSMASK(r->reg), 0);
    wrmsr(MSR_PHYSBASE(r->reg), 0);
}

/* Make [base, base+size) write-combining.  0 on success; the log says
   what happened either way. */
int fb_write_combine(uint32_t base, uint32_t size)
{
    uint64_t cap, def, addr_mask;
    uint32_t span;
    int n, i, free_reg = -1, bits;
    struct range r;

    note("framebuffer: looking at the MTRRs");
    if (!(cpuid_edx(1) & (1 << 12))) { note("framebuffer: no MTRRs on this processor"); return -1; }
    cap = rdmsr(MSR_MTRRCAP);
    def = rdmsr(MSR_MTRRDEFTYPE);
    n = (int)(cap & 0xFF);
    if (!(cap & (1 << 10))) { note("framebuffer: processor cannot write-combine"); return -1; }
    if (!(def & (1 << 11)))  { note("framebuffer: MTRRs are disabled; left alone"); return -1; }

    /* a power of two, aligned to itself, that covers the screen */
    for (span = 0x1000; span < size; span <<= 1) ;
    if (base & (span - 1)) {
        notef("framebuffer: %08X is not aligned to a %u KB range; left alone", (int)base, (int)(span >> 10));
        return -1;
    }
    bits = phys_bits();
    addr_mask = ((1ull << bits) - 1) & ~0xFFFull;

    sys_logf("mtrr: default type %d, %d variable registers, %d address bits",
             (int)(def & 0xFF), n, bits);

    /* what covers it already?  an explicit uncacheable range would win */
    for (i = 0; i < n; i++) {
        uint64_t b = rdmsr(MSR_PHYSBASE(i)), m = rdmsr(MSR_PHYSMASK(i));
        if (!(m & (1 << 11))) { if (free_reg < 0) free_reg = i; continue; }
        {
            uint64_t span_of = (~(m & addr_mask) & addr_mask) + 0x1000;   /* the range's size */
            sys_logf("mtrr %d: %08X%08X size %08X%08X type %d", i,
                     (uint32_t)((b & addr_mask) >> 32), (uint32_t)(b & addr_mask),
                     (uint32_t)(span_of >> 32), (uint32_t)span_of, (int)(b & 0xFF));
        }
        if (((uint64_t)base & m & addr_mask) == (b & m & addr_mask)) {
            int t = (int)(b & 0xFF);
            if (t == TYPE_WC) { note("framebuffer: already write-combining"); return 0; }
            /* an explicit range of any other type is the firmware's
               decision, and uncacheable would win over ours anyway */
            notef("framebuffer: MTRR %d already covers it (type %d); left alone", i, t);
            return -1;
        }
    }
    if (free_reg < 0) { note("framebuffer: no MTRR free; left alone"); return -1; }

    r.reg = free_reg;
    r.base = ((uint64_t)base & addr_mask) | TYPE_WC;
    r.mask = ((~(uint64_t)(span - 1)) & addr_mask) | (1 << 11);
    with_caches_off(write_range, &r);
    used_reg = free_reg;
    sys_logf("framebuffer: %u MB at %08X now write-combining (MTRR %d of %d)",
             span >> 20, base, free_reg, n);
    notef("framebuffer: %u MB now write-combining, MTRR %d", (int)(span >> 20), free_reg);
    return 0;
}

/* give the register back on the way out */
void fb_write_combine_undo(void)
{
    struct range r;
    if (used_reg < 0) return;
    r.reg = used_reg;
    with_caches_off(clear_range, &r);
    used_reg = -1;
}
