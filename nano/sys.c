/* Ember NX32 system calls: every kernel service is a real-mode interrupt
   executed by the runtime on the program's behalf (INT 80h). */
#include <nanolibc.h>
#include "nano.h"

volatile struct nx_info *nx_info;
uint32_t nx_info_raw;
uint8_t *nx_bounce;
uint16_t nx_bounce_seg;
void (*nx_irq0_fn)(void);
void (*nx_irq1_fn)(void);
extern void nx_irq0_stub(void);
extern void nx_irq1_stub(void);
extern void heap_init(uint32_t start, uint32_t end);
int main(int argc, char **argv);

#define RC_OFF   0xFFE0                 /* rmcall block inside the bounce */
#define PATH_OFF 0xFF00                 /* path strings */
#define INFO_OFF 0xFF80                 /* small reply structures */
static struct rmcall *rc;

void rm_int(struct rmcall *r)
{
    __asm__ volatile("int $0x80" : : "b"((uint32_t)r) : "memory", "cc");
}

static void rc_init(uint8_t ah, uint8_t al, int intno)
{
    memset(rc, 0, sizeof *rc);
    rc->ax = (ah << 8) | al;
    rc->ds = nx_bounce_seg;
    rc->es = nx_bounce_seg;
    rc->intno = (uint8_t)intno;
}

static int carry(void) { return rc->flags & 1; }

/* Run any BIOS or DOS interrupt with the given registers.  The block the
   kernel reads must live in low memory, so the caller's copy is marshalled
   through the one in the bounce buffer. */
void sys_bios(struct rmcall *r)
{
    memcpy(rc, r, sizeof *rc);
    rm_int(rc);
    memcpy(r, rc, sizeof *rc);
}

int sys_open_error;                     /* the DOS code, when one fails */

int sys_open(const char *path)
{
    size_t n = strlen(path);
    if (n > 120) return -1;
    memcpy(nx_bounce + PATH_OFF, path, n + 1);
    rc_init(0x3D, 0x00, 0x21);
    rc->dx = PATH_OFF;
    rm_int(rc);
    if (carry()) { sys_open_error = rc->ax; return -1; }
    return rc->ax;
}

int sys_read(int h, void *buf, int n)
{
    int got = 0;
    while (n > 0) {
        int chunk = n > BOUNCE_DATA_MAX ? BOUNCE_DATA_MAX : n;
        rc_init(0x3F, 0, 0x21);
        rc->bx = (uint16_t)h;
        rc->cx = (uint16_t)chunk;
        rc->dx = 0;
        rm_int(rc);
        if (carry()) return got ? got : -1;
        memcpy((char *)buf + got, nx_bounce, rc->ax);
        got += rc->ax;
        n -= rc->ax;
        if (rc->ax < chunk) break;
    }
    return got;
}

int sys_create(const char *path)
{
    size_t n = strlen(path);
    if (n > 120) return -1;
    memcpy(nx_bounce + PATH_OFF, path, n + 1);
    rc_init(0x3C, 0x00, 0x21);
    rc->cx = 0;                                 /* a normal file */
    rc->dx = PATH_OFF;
    rm_int(rc);
    return carry() ? -1 : rc->ax;
}

int sys_write(int h, const void *buf, int n)
{
    int done = 0;
    while (n > 0) {
        int chunk = n > BOUNCE_DATA_MAX ? BOUNCE_DATA_MAX : n;
        memcpy(nx_bounce, (const char *)buf + done, chunk);
        rc_init(0x40, 0, 0x21);
        rc->bx = (uint16_t)h;
        rc->cx = (uint16_t)chunk;
        rc->dx = 0;
        rm_int(rc);
        if (carry()) return done ? done : -1;
        done += rc->ax;
        n -= rc->ax;
        if (rc->ax < chunk) break;              /* the disk is full */
    }
    return done;
}

int sys_mkdir(const char *path)
{
    size_t n = strlen(path);
    if (n > 120) return -1;
    memcpy(nx_bounce + PATH_OFF, path, n + 1);
    rc_init(0x39, 0x00, 0x21);
    rc->dx = PATH_OFF;
    rm_int(rc);
    return carry() ? -1 : 0;
}

int sys_unlink(const char *path)
{
    size_t n = strlen(path);
    if (n > 120) return -1;
    memcpy(nx_bounce + PATH_OFF, path, n + 1);
    rc_init(0x41, 0x00, 0x21);
    rc->dx = PATH_OFF;
    rm_int(rc);
    return carry() ? -1 : 0;
}

long sys_lseek(int h, long off, int whence)
{
    rc_init(0x42, (uint8_t)whence, 0x21);
    rc->bx = (uint16_t)h;
    rc->cx = (uint16_t)((uint32_t)off >> 16);
    rc->dx = (uint16_t)off;
    rm_int(rc);
    if (carry()) return -1;
    return (long)(((uint32_t)rc->dx << 16) | rc->ax);
}

int sys_close(int h)
{
    rc_init(0x3E, 0, 0x21);
    rc->bx = (uint16_t)h;
    rm_int(rc);
    return carry() ? -1 : 0;
}

void sys_puts(const char *s)
{
    size_t n = strlen(s);
    while (n) {
        size_t k = n > 0x800 ? 0x800 : n;
        memcpy(nx_bounce, s, k);
        rc_init(0x40, 0, 0x21);
        rc->bx = 1;
        rc->cx = (uint16_t)k;
        rc->dx = 0;
        rm_int(rc);
        s += k; n -= k;
    }
}

/* The string goes through the path area of the bounce buffer, which is
   128 bytes: anything longer runs into the reply area and then the call
   block itself, and the call never comes back. */
void sys_log(const char *s)
{
    size_t n = strlen(s);
    if (n > 120) n = 120;
    memcpy(nx_bounce + PATH_OFF, s, n);
    nx_bounce[PATH_OFF + n] = 0;
    rc_init(0xF2, 0, 0x21);
    rc->dx = PATH_OFF;
    rm_int(rc);
}

/* Lines for the kernel's shell to run once this program has ended: how
   the desktop starts a DOS program and is started again after it. */
#define AFTER_OFF 0xF900
void sys_run_after(const char *lines)
{
    size_t n = strlen(lines);
    if (n > 500) n = 500;
    memcpy(nx_bounce + AFTER_OFF, lines, n);
    nx_bounce[AFTER_OFF + n] = 0;
    rc_init(0xF4, 0, 0x21);
    rc->dx = AFTER_OFF;
    rm_int(rc);
}

void sys_logf(const char *fmt, ...)
{
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    sys_log(buf);
}

#define VBE_OFF  0xFA00                 /* the 512-byte VBE block */
#define MODE_OFF 0xFC00                 /* a 256-byte mode block, clear of it */
#define DTA_OFF  0xFD00

int sys_vbe_info(struct vbe_info *info)
{
    const uint8_t *b = nx_bounce + VBE_OFF;
    memset(nx_bounce + VBE_OFF, 0, 512);
    memcpy(nx_bounce + VBE_OFF, "VBE2", 4);     /* ask for the 2.0 block */
    rc_init(0x4F, 0x00, 0x10);
    rc->es = nx_bounce_seg;
    rc->di = VBE_OFF;
    rm_int(rc);
    if (rc->ax != 0x004F || memcmp(b, "VESA", 4))
        return -1;
    info->version    = *(const uint16_t *)(b + 4);
    info->memory_64k = *(const uint16_t *)(b + 18);
    {
        uint16_t off = *(const uint16_t *)(b + 14);
        uint16_t seg = *(const uint16_t *)(b + 16);
        const uint16_t *list = (const uint16_t *)(((uint32_t)seg << 4) + off);
        int n = 0;
        while (n < VBE_MAX_MODES && list[n] != 0xFFFF)
            n++;
        memcpy(info->modes, list, n * 2);
        info->mode_count = n;
    }
    return 0;
}

int sys_vbe_mode(int mode, struct vbe_mode *m)
{
    const uint8_t *b = nx_bounce + MODE_OFF;
    memset(nx_bounce + MODE_OFF, 0, 256);
    rc_init(0x4F, 0x01, 0x10);
    rc->cx = (uint16_t)mode;
    rc->es = nx_bounce_seg;
    rc->di = MODE_OFF;
    rm_int(rc);
    if (rc->ax != 0x004F) return -1;
    m->attributes = *(const uint16_t *)(b + 0);
    m->pitch      = *(const uint16_t *)(b + 16);
    m->width      = *(const uint16_t *)(b + 18);
    m->height     = *(const uint16_t *)(b + 20);
    m->bpp        = b[25];
    m->framebuffer = (m->attributes & 0x80) ? *(const uint32_t *)(b + 40) : 0;
    return (m->attributes & 1) ? 0 : -1;
}

int sys_set_vbe_mode(int mode, int linear)
{
    rc_init(0x4F, 0x02, 0x10);
    rc->bx = (uint16_t)(mode | (linear ? 0x4000 : 0));
    rm_int(rc);
    return rc->ax == 0x004F ? 0 : -1;
}

/* ---- the keyboard: a queue the default interrupt handler fills ---- */
#define KQ_SIZE 32
static volatile uint16_t kq[KQ_SIZE];
static volatile int kq_head, kq_tail;
static int kb_shift, kb_e0;

/* scancode set 1, US layout: what each key types, plain and shifted */
static const char kb_plain[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', 8, 9,
    'q','w','e','r','t','y','u','i','o','p','[',']', 13, 0, 'a','s',
    'd','f','g','h','j','k','l',';','\'','`', 0, '\\','z','x','c','v',
    'b','n','m',',','.','/', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '-', 0, 0, 0, '+', 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
static const char kb_shifted[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', 8, 9,
    'Q','W','E','R','T','Y','U','I','O','P','{','}', 13, 0, 'A','S',
    'D','F','G','H','J','K','L',':','"','~', 0, '|','Z','X','C','V',
    'B','N','M','<','>','?', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '-', 0, 0, 0, '+', 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

static void kb_default_irq(void)
{
    uint8_t sc = inb(0x60);
    int ch = 0, next;
    if (sc == 0xE0) { kb_e0 = 1; return; }
    if (sc & 0x80) {                            /* a key going up */
        sc &= 0x7F;
        if (sc == 0x2A || sc == 0x36) kb_shift = 0;
        kb_e0 = 0;
        return;
    }
    if (sc == 0x2A || sc == 0x36) { kb_shift = 1; kb_e0 = 0; return; }
    if (kb_e0) {
        if (sc == 0x1C) ch = 13;                /* the keypad's Enter */
        else if (sc == 0x35) ch = '/';
    } else if (sc < 128) {
        ch = (uint8_t)(kb_shift ? kb_shifted : kb_plain)[sc];
    }
    kb_e0 = 0;
    next = (kq_head + 1) % KQ_SIZE;
    if (next != kq_tail) {
        kq[kq_head] = (uint16_t)((sc << 8) | ch);
        kq_head = next;
    }
}

int sys_kbhit(void)
{
    if (kq_tail == kq_head) return 0;
    return kq[kq_tail];
}

int sys_getkey(void)
{
    int k;
    while (kq_tail == kq_head) __asm__ volatile("hlt");
    k = kq[kq_tail];
    kq_tail = (kq_tail + 1) % KQ_SIZE;
    return k;
}

static void find_copy(struct dos_find *f)
{
    const uint8_t *d = nx_bounce + DTA_OFF;
    int i;
    f->attr = d[0x15];
    f->time = *(const uint16_t *)(d + 0x16);
    f->date = *(const uint16_t *)(d + 0x18);
    f->size = *(const uint32_t *)(d + 0x1A);
    for (i = 0; i < 12 && d[0x1E + i]; i++) f->name[i] = (char)d[0x1E + i];
    f->name[i] = 0;
}

static void set_dta(void)
{
    rc_init(0x1A, 0, 0x21);
    rc->ds = nx_bounce_seg;
    rc->dx = DTA_OFF;
    rm_int(rc);
}

int sys_findfirst(const char *pattern, struct dos_find *f)
{
    size_t n = strlen(pattern);
    if (n > 120) return -1;
    set_dta();
    memcpy(nx_bounce + PATH_OFF, pattern, n + 1);
    rc_init(0x4E, 0, 0x21);
    rc->cx = 0x37;                              /* files and directories */
    rc->dx = PATH_OFF;
    rm_int(rc);
    if (carry()) return -1;
    find_copy(f);
    return 0;
}

int sys_findnext(struct dos_find *f)
{
    set_dta();
    rc_init(0x4F, 0, 0x21);
    rm_int(rc);
    if (carry()) return -1;
    find_copy(f);
    return 0;
}

int sys_long_name(char *buf, int size)
{
    int n;
    rc_init(0xF3, 0, 0x21);
    rc->dx = PATH_OFF;
    rm_int(rc);
    n = rc->ax;
    if (n > size - 1) n = size - 1;
    memcpy(buf, nx_bounce + PATH_OFF, n);
    buf[n] = 0;
    return n;
}

void sys_set_video_mode(int mode)
{
    rc_init(0x00, (uint8_t)mode, 0x10);
    rm_int(rc);
}

int sys_pcm_start(struct pcm_info *pi)
{
    rc_init(0xF0, 0, 0x21);
    rc->dx = INFO_OFF;
    rm_int(rc);
    if (carry()) return -(int)rc->ax;           /* -status from the driver */
    memcpy(pi, nx_bounce + INFO_OFF, sizeof *pi);
    return 0;
}

void sys_pcm_stop(void)
{
    rc_init(0xF1, 0, 0x21);
    rm_int(rc);
}

void (*nx_irq12_fn)(void);
extern void nx_irq12_stub(void);

void sys_set_mouse_handler(void (*irq12)(void))
{
    cli();
    nx_irq12_fn = irq12;
    nx_info->irq12 = irq12 ? (uint32_t)nx_irq12_stub : 0;
    sti();
}

void sys_set_irq_handlers(void (*irq0)(void), void (*irq1)(void))
{
    cli();
    nx_irq0_fn = irq0;
    nx_irq1_fn = irq1;
    nx_info->irq0 = irq0 ? (uint32_t)nx_irq0_stub : 0;
    nx_info->irq1 = irq1 ? (uint32_t)nx_irq1_stub : 0;
    sti();
}

void sys_exit(int code)
{
    (void)code;
    cli();
    nx_info->irq0 = 0;
    nx_info->irq1 = 0;
    nx_info->irq12 = 0;
    sti();
    rc_init(0, 0, 0xFF);
    rm_int(rc);
    for (;;) ;
}

int nx_has_clflush;

void nano_main(void)
{
    static char *argv[32];
    static char cmdline[136];
    int argc = 1; char *p;
    uint32_t a, b, c, d;

    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    nx_has_clflush = (d >> 19) & 1;
    nx_info = (volatile struct nx_info *)nx_info_raw;
    nx_bounce = (uint8_t *)nx_info->bounce;
    nx_bounce_seg = (uint16_t)(nx_info->bounce >> 4);
    rc = (struct rmcall *)(nx_bounce + RC_OFF);
    heap_init(nx_info->mem_start, nx_info->mem_end);
    sys_set_irq_handlers(0, kb_default_irq);    /* until the program says otherwise */

    strncpy(cmdline, (const char *)nx_info->cmdline, sizeof cmdline - 1);
    argv[0] = "NDOOM";
    for (p = cmdline; *p && argc < 31;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\r' || *p == '\n') break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
        if (*p) *p++ = 0;
    }
    argv[argc] = 0;
    sys_exit(main(argc, argv));
}
