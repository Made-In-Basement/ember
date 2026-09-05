/* HELLO32: exercises the NX32 runtime (console, files, timer IRQ, heap) */
#include <nanolibc.h>
#include "nano.h"

static volatile unsigned ticks;
static void tick(void) { ticks++; }

int main(int argc, char **argv)
{
    int i, fd, n;
    char buf[64];
    unsigned t0;
    void *p;

    printf("Hello from 32-bit protected mode!\n");
    printf("  memory %u KB at %08X..%08X, bounce at %05X\n",
           (nx_info->mem_end - nx_info->mem_start) / 1024, nx_info->mem_start,
           nx_info->mem_end, nx_info->bounce);
    printf("  argc=%d:", argc);
    for (i = 1; i < argc; i++) printf(" [%s]", argv[i]);
    printf("\n");
    p = malloc(4 * 1024 * 1024);
    printf("  malloc(4 MB) = %08X\n", (unsigned)p);
    if (p) { memset(p, 0x5A, 4 * 1024 * 1024); printf("  filled it: byte[3999999]=%02X\n", ((unsigned char *)p)[3999999]); }
    fd = sys_open("\\README.TXT");
    printf("  open README.TXT = %d\n", fd);
    if (fd >= 0) {
        n = sys_read(fd, buf, 40);
        buf[n > 0 ? n : 0] = 0;
        for (i = 0; i < n; i++) if (buf[i] == '\r' || buf[i] == '\n') buf[i] = ' ';
        printf("  read %d bytes: \"%s\"\n", n, buf);
        printf("  size via lseek = %ld\n", sys_lseek(fd, 0, SEEK_END));
        sys_close(fd);
    }
    sys_set_irq_handlers(tick, 0);
    t0 = ticks;
    for (i = 0; i < 20000000; i++) __asm__ volatile("" ::: "memory");
    printf("  timer IRQ ticks during a busy loop: %u\n", ticks - t0);
    printf("  x87: sqrt(2) = %f, pow(2, 0.5) = %f\n", sqrt(2.0), pow(2.0, 0.5));
    printf("  64-bit: %d\n", (int)(1234567890123LL / 1000000LL));
    {
        struct pcm_info pi;
        if (sys_pcm_start(&pi) < 0) {
            printf("  pcm stream: not available\n");
        } else {
            volatile uint32_t *lpib = (volatile uint32_t *)pi.lpib_phys;
            volatile int16_t *ring = (volatile int16_t *)pi.ring_phys;
            unsigned frames = pi.ring_size / 4, k;
            printf("  pcm ring %08X size %u lpib reg %08X rate %u\n",
                   pi.ring_phys, pi.ring_size, pi.lpib_phys, pi.rate);
            for (k = 0; k < frames; k++) {          /* 440 Hz square wave */
                int16_t v = ((k / 50) & 1) ? 8000 : -8000;
                ring[k * 2] = v; ring[k * 2 + 1] = v;
            }
            cache_flush((const void *)ring, pi.ring_size);
            for (k = 0; k < 4; k++) {
                t0 = ticks;
                while (ticks - t0 < 8) ;                /* ~0.44 s at 18.2 Hz */
                printf("  lpib = %08X\n", *lpib);
            }
            sys_pcm_stop();
        }
    }
    printf("Done.\n");
    return 0;
}
