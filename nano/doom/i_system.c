/* Doom for Ember: system layer (timer, memory, errors)

   Game time comes from the CPU cycle counter (RDTSC), calibrated against
   PIT channel 2 at startup, so it does not depend on the timer interrupt
   reaching protected mode.  The 140 Hz timer interrupt is still installed
   and counted, for diagnostics. */
#include <nanolibc.h>
#include "nano.h"
#include "doomdef.h"
#include "m_misc.h"
#include "i_video.h"
#include "i_sound.h"
#include "d_net.h"
#include "g_game.h"
#include "i_system.h"

int mb_used = 8;

void I_Tactile(int on, int off, int total)
{
    (void)on; (void)off; (void)total;
}

ticcmd_t emptycmd;
ticcmd_t *I_BaseTiccmd(void)
{
    return &emptycmd;
}

int I_GetHeapSize(void)
{
    return mb_used * 1024 * 1024;
}

byte *I_ZoneBase(int *size)
{
    byte *p;
    unsigned avail = nx_info->mem_end - nx_info->mem_start - 512 * 1024;
    *size = mb_used * 1024 * 1024;
    if ((unsigned)*size > avail) *size = (int)(avail & ~0xFFFFu);
    p = malloc(*size);
    if (!p) I_Error("I_ZoneBase: could not allocate %d bytes", *size);
    return p;
}

/* ---- time ---- */
static volatile unsigned timer_ticks;           /* IRQ0 at 140 Hz */
static void timer_irq(void)
{
    timer_ticks++;
}

static uint64_t tsc_hz, tsc_base, tsc_per_tic;

static uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* PIT channel 2 (the speaker timer, untouched by the BIOS tick) counts
   65535 cycles of 1.193182 MHz = 54.9 ms; time it with the TSC. */
static void calibrate_tsc(void)
{
    uint8_t p61 = inb(0x61);
    uint64_t t0, t1;
    unsigned guard = 0;
    outb(0x61, (p61 & ~0x02) | 0x01);           /* gate on, speaker off */
    outb(0x43, 0xB0);                           /* channel 2, lo/hi, mode 0 */
    outb(0x42, 0xFF);
    outb(0x42, 0xFF);
    t0 = rdtsc();
    while (!(inb(0x61) & 0x20)) {               /* OUT2 rises at terminal count */
        if (++guard > 200000000u) break;
    }
    t1 = rdtsc();
    outb(0x61, p61 & ~0x03);
    if (guard > 200000000u || t1 - t0 < 100000)
        tsc_hz = 2000000000u;                   /* no PIT readback: guess 2 GHz */
    else
        tsc_hz = (t1 - t0) * 1193182u / 65535u;
    tsc_per_tic = tsc_hz / TICRATE;
    tsc_base = rdtsc();
}

int I_GetTime(void)
{
    return (int)((rdtsc() - tsc_base) / tsc_per_tic);
}

static void tsc_delay_ms(unsigned ms)
{
    uint64_t until = rdtsc() + tsc_hz / 1000 * ms;
    while (rdtsc() < until) ;
}

void I_WaitVBL(int count)
{
    tsc_delay_ms((unsigned)count * 1000 / 70);
}

extern void I_KeyboardIrq(void);
extern unsigned kb_irq_count, kb_poll_count;

void I_Init(void)
{
    unsigned t;
    calibrate_tsc();
    sys_logf("NDOOM: TSC %u MHz", (unsigned)(tsc_hz / 1000000u));
    /* sound first: the kernel's HDA setup measures its timeouts in BIOS
       ticks, so the timer must still run at 18.2 Hz while it initialises */
    I_InitSound();
    /* PIT channel 0 at 140 Hz */
    outb(0x43, 0x36);
    outb(0x40, 8523 & 0xFF);
    outb(0x40, 8523 >> 8);
    sys_set_irq_handlers(timer_irq, I_KeyboardIrq);
    t = timer_ticks;
    tsc_delay_ms(200);
    sys_logf("NDOOM: timer IRQ ticks in 200 ms: %u (28 expected)", timer_ticks - t);
}

void I_Quit(void)
{
    D_QuitNetGame();
    I_ShutdownSound();
    I_ShutdownMusic();
    M_SaveDefaults();
    I_ShutdownGraphics();
    sys_logf("NDOOM: quit at tic %d; keyboard bytes: irq %u, polled %u",
             I_GetTime(), kb_irq_count, kb_poll_count);
    sys_exit(0);
}

byte *I_AllocLow(int length)
{
    byte *p = malloc(length);
    if (p) memset(p, 0, length);
    return p;
}

void I_BeginRead(void) {}
void I_EndRead(void) {}

void I_Error(char *error, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, error);
    vsnprintf(msg, sizeof msg, error, ap);
    va_end(ap);
    sys_set_irq_handlers(0, 0);
    sys_pcm_stop();
    sys_set_video_mode(3);
    sys_puts("Error: ");
    sys_puts(msg);
    sys_puts("\r\n");
    sys_logf("NDOOM: I_Error: %s", msg);
    sys_exit(1);
}
