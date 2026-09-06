/* Ember NX32 runtime interface */
#ifndef NANO_H
#define NANO_H
#include <stdint.h>

struct nx_info {
    uint32_t magic, mem_start, mem_end, bounce, bounce_size, cmdline, irq0, irq1,
             irq12;
};

struct rmcall {                         /* real-mode interrupt request */
    uint16_t ax, bx, cx, dx, si, di, ds, es, flags;
    uint8_t intno, pad;
    uint16_t bp;
} __attribute__((packed));

struct pcm_info { uint32_t ring_phys, ring_size, lpib_phys, rate; };

struct dos_find {                       /* what a directory search returns */
    char name[13];
    uint32_t size;
    uint16_t date, time;
    uint8_t attr;
};

#define VBE_MAX_MODES 128

struct vbe_info {                       /* what the card says about itself */
    uint16_t version;
    uint16_t memory_64k;
    int      mode_count;
    uint16_t modes[VBE_MAX_MODES];      /* copied out: the card's list often
                                           lives inside the block itself */
};

struct vbe_mode {                       /* the parts of a VESA mode we use */
    uint16_t attributes, pitch, width, height;
    uint8_t bpp;
    uint32_t framebuffer;               /* 0 when the mode has no linear one */
};

extern volatile struct nx_info *nx_info;
extern uint8_t *nx_bounce;              /* 64 KB real-mode buffer (flat pointer) */
extern uint16_t nx_bounce_seg;
extern void (*nx_irq0_fn)(void);
extern void (*nx_irq1_fn)(void);

#define BOUNCE_DATA_MAX 0xF000          /* bytes of the bounce usable for I/O */

void rm_int(struct rmcall *rc);
void sys_bios(struct rmcall *r);        /* any interrupt, registers marshalled */
int  sys_open(const char *path);
extern int sys_open_error;              /* DOS error from the last failure */        /* read-only; DOS handle or -1 */
int  sys_create(const char *path);      /* create or truncate, open for writing */
int  sys_write(int h, const void *buf, int n);
int  sys_unlink(const char *path);
int  sys_mkdir(const char *path);
int  sys_read(int h, void *buf, int n);
long sys_lseek(int h, long off, int whence);
int  sys_close(int h);
void sys_puts(const char *s);
void sys_log(const char *s);            /* a line into EMBER.LOG */
void sys_run_after(const char *lines);  /* for the shell, once we have ended */
void sys_logf(const char *fmt, ...);
void sys_set_video_mode(int mode);
int  sys_vbe_info(struct vbe_info *info);          /* 0 if the card answers */
int  sys_vbe_mode(int mode, struct vbe_mode *m);   /* 0 if the mode exists */
int  sys_set_vbe_mode(int mode, int linear);       /* 0 on success */
int  sys_kbhit(void);                   /* the pending key, or 0 */
int  sys_getkey(void);                  /* waits: AL | scan code << 8 */
int  sys_findfirst(const char *pattern, struct dos_find *f);
int  sys_findnext(struct dos_find *f);
int  sys_long_name(char *buf, int size);  /* the last found entry's real name */
int  sys_pcm_start(struct pcm_info *pi);
void sys_pcm_stop(void);
void sys_set_irq_handlers(void (*irq0)(void), void (*irq1)(void));
void sys_set_mouse_handler(void (*irq12)(void));
void sys_exit(int code) __attribute__((noreturn));

static inline void outb(uint16_t p, uint8_t v) { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(p)); }
static inline uint8_t inb(uint16_t p) { uint8_t v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void cli(void) { __asm__ volatile("cli" ::: "memory"); }
static inline void sti(void) { __asm__ volatile("sti" ::: "memory"); }
extern int nx_has_clflush;              /* CPUID.1:EDX bit 19, set at startup */
static inline void cache_flush(const void *p, uint32_t n)
{
    const char *c = (const char *)((uint32_t)p & ~63u);
    const char *e = (const char *)p + n;
    if (!nx_has_clflush) { __asm__ volatile("wbinvd" ::: "memory"); return; }
    for (; c < e; c += 64) __asm__ volatile("clflush (%0)" : : "r"(c) : "memory");
    __asm__ volatile("" ::: "memory");
}

#endif
