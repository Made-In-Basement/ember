/* nano libc: strings, memory, a first-fit heap, printf, small stdio */
#include <nanolibc.h>
#include "nano.h"

int errno;

/* ---------------------------------------------------------------- memory */
void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *dp = d; const unsigned char *sp = s;
    if (((uint32_t)dp & 3) == 0 && ((uint32_t)sp & 3) == 0) {
        while (n >= 4) { *(uint32_t *)dp = *(const uint32_t *)sp; dp += 4; sp += 4; n -= 4; }
    }
    while (n--) *dp++ = *sp++;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *dp = d; const unsigned char *sp = s;
    if (dp < sp || dp >= sp + n) return memcpy(d, s, n);
    dp += n; sp += n;
    while (n--) *--dp = *--sp;
    return d;
}

void *memset(void *d, int c, size_t n)
{
    unsigned char *dp = d;
    uint32_t w = (unsigned char)c; w |= w << 8; w |= w << 16;
    if (((uint32_t)dp & 3) == 0)
        while (n >= 4) { *(uint32_t *)dp = w; dp += 4; n -= 4; }
    while (n--) *dp++ = (unsigned char)c;
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    while (n--) { if (*x != *y) return *x - *y; x++; y++; }
    return 0;
}

/* ---------------------------------------------------------------- strings */
size_t strlen(const char *s) { const char *p = s; while (*p) p++; return p - s; }
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) ; return r; }
char *strncpy(char *d, const char *s, size_t n)
{
    char *r = d;
    while (n && *s) { *d++ = *s++; n--; }
    while (n--) *d++ = 0;
    return r;
}
char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
char *strncat(char *d, const char *s, size_t n)
{
    char *p = d + strlen(d);
    while (n-- && *s) *p++ = *s++;
    *p = 0;
    return d;
}
int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}
int strcasecmp(const char *a, const char *b)
{
    while (*a && toupper(*a) == toupper(*b)) { a++; b++; }
    return toupper(*a) - toupper(*b);
}
int strncasecmp(const char *a, const char *b, size_t n)
{
    while (n && *a && toupper(*a) == toupper(*b)) { a++; b++; n--; }
    return n ? toupper(*a) - toupper(*b) : 0;
}
char *strchr(const char *s, int c)
{
    for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return 0; }
}
char *strrchr(const char *s, int c)
{
    const char *r = 0;
    for (;; s++) { if (*s == (char)c) r = s; if (!*s) return (char *)r; }
}
char *strstr(const char *h, const char *n)
{
    size_t l = strlen(n);
    for (; *h; h++) if (!strncmp(h, n, l)) return (char *)h;
    return l ? 0 : (char *)h;
}
char *strdup(const char *s) { char *d = malloc(strlen(s) + 1); if (d) strcpy(d, s); return d; }

/* ---------------------------------------------------------------- ctype */
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isalnum(int c) { return isdigit(c) || isalpha(c); }
int isspace(int c) { return c == ' ' || (c >= 9 && c <= 13); }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isprint(int c) { return c >= 32 && c < 127; }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int toupper(int c) { return islower(c) ? c - 32 : c; }
int tolower(int c) { return isupper(c) ? c + 32 : c; }

/* ---------------------------------------------------------------- heap */
struct hblock { uint32_t size; uint32_t free; };  /* 8-byte header, 16-byte units */
static struct hblock *heap_first, *heap_end;

void heap_init(uint32_t start, uint32_t end)
{
    start = (start + 15) & ~15u;
    end &= ~15u;
    heap_first = (struct hblock *)start;
    heap_end = (struct hblock *)end;
    heap_first->size = end - start;
    heap_first->free = 1;
}

static struct hblock *hnext(struct hblock *b) { return (struct hblock *)((char *)b + b->size); }

void *malloc(size_t n)
{
    struct hblock *b;
    uint32_t need = (n + sizeof(struct hblock) + 15) & ~15u;
    if (!heap_first) return 0;
    for (b = heap_first; b < heap_end; b = hnext(b)) {
        if (!b->free) continue;
        /* merge following free blocks */
        while (hnext(b) < heap_end && hnext(b)->free) b->size += hnext(b)->size;
        if (b->size < need) continue;
        if (b->size >= need + 32) {
            struct hblock *rest = (struct hblock *)((char *)b + need);
            rest->size = b->size - need;
            rest->free = 1;
            b->size = need;
        }
        b->free = 0;
        return (char *)b + sizeof(struct hblock);
    }
    return 0;
}

void free(void *p)
{
    struct hblock *b;
    if (!p) return;
    b = (struct hblock *)((char *)p - sizeof(struct hblock));
    b->free = 1;
}

void *calloc(size_t n, size_t m)
{
    void *p = malloc(n * m);
    if (p) memset(p, 0, n * m);
    return p;
}

void *realloc(void *p, size_t n)
{
    struct hblock *b; void *q; uint32_t old;
    if (!p) return malloc(n);
    b = (struct hblock *)((char *)p - sizeof(struct hblock));
    old = b->size - sizeof(struct hblock);
    if (old >= n) return p;
    q = malloc(n);
    if (!q) return 0;
    memcpy(q, p, old);
    free(p);
    return q;
}

/* ---------------------------------------------------------------- numbers */
long strtol(const char *s, char **end, int base)
{
    long v = 0; int neg = 0;
    while (isspace(*s)) s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
    else if (base == 0) base = (*s == '0') ? 8 : 10;
    for (;; s++) {
        int d;
        if (isdigit(*s)) d = *s - '0';
        else if (isalpha(*s)) d = toupper(*s) - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
    }
    if (end) *end = (char *)s;
    return neg ? -v : v;
}
int atoi(const char *s) { return (int)strtol(s, 0, 10); }
long atol(const char *s) { return strtol(s, 0, 10); }
int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }
char *getenv(const char *name) { (void)name; return 0; }

static unsigned rand_state = 1;
int rand(void) { rand_state = rand_state * 1103515245u + 12345u; return (int)((rand_state >> 1) & RAND_MAX); }
void srand(unsigned seed) { rand_state = seed; }

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    /* shell sort: small, no recursion */
    char *b = base; size_t gap, i, j; char tmp[256];
    if (size > sizeof tmp) return;
    for (gap = n / 2; gap > 0; gap /= 2)
        for (i = gap; i < n; i++) {
            memcpy(tmp, b + i * size, size);
            for (j = i; j >= gap && cmp(b + (j - gap) * size, tmp) > 0; j -= gap)
                memcpy(b + j * size, b + (j - gap) * size, size);
            memcpy(b + j * size, tmp, size);
        }
}

void exit(int code) { sys_exit(code); }
void abort(void) { sys_puts("abort()\r\n"); sys_exit(3); }

/* ---------------------------------------------------------------- 64-bit division helpers (compiler-rt) */
static uint64_t udivmod64(uint64_t a, uint64_t b, uint64_t *rem)
{
    uint64_t q = 0, r = 0; int i;
    if (b == 0) { if (rem) *rem = 0; return 0; }
    for (i = 63; i >= 0; i--) {
        r = (r << 1) | ((a >> i) & 1);
        if (r >= b) { r -= b; q |= (uint64_t)1 << i; }
    }
    if (rem) *rem = r;
    return q;
}
uint64_t __udivdi3(uint64_t a, uint64_t b) { return udivmod64(a, b, 0); }
uint64_t __umoddi3(uint64_t a, uint64_t b) { uint64_t r; udivmod64(a, b, &r); return r; }
int64_t __divdi3(int64_t a, int64_t b)
{
    int neg = (a < 0) != (b < 0);
    uint64_t q = udivmod64(a < 0 ? -a : a, b < 0 ? -b : b, 0);
    return neg ? -(int64_t)q : (int64_t)q;
}
int64_t __moddi3(int64_t a, int64_t b)
{
    uint64_t r; udivmod64(a < 0 ? -a : a, b < 0 ? -b : b, &r);
    return a < 0 ? -(int64_t)r : (int64_t)r;
}

/* ---------------------------------------------------------------- math */
double fabs(double x) { return x < 0 ? -x : x; }
double floor(double x) { long i = (long)x; return (x < 0 && (double)i != x) ? i - 1 : i; }
double sqrt(double x) { double r; __asm__("fsqrt" : "=t"(r) : "0"(x)); return r; }
float fabsf(float x) { return x < 0 ? -x : x; }
float floorf(float x) { long i = (long)x; return (x < 0 && (float)i != x) ? (float)(i - 1) : (float)i; }
float sqrtf(float x) { float r; __asm__("fsqrt" : "=t"(r) : "0"(x)); return r; }
/* The x87 reduces the argument itself; angles here are bounded well inside
   what it will accept, so there is nothing to do beforehand. */
float sinf(float x) { float r; __asm__("fsin" : "=t"(r) : "0"(x)); return r; }
float cosf(float x) { float r; __asm__("fcos" : "=t"(r) : "0"(x)); return r; }
float fmodf(float x, float y)
{
    float q;
    if (y == 0 || !isfinite(x) || !isfinite(y)) return 0;
    q = x / y;
    if (q > 2147483000.0f || q < -2147483000.0f) return 0;
    q = (float)(long)q;                 /* toward zero, as fmod truncates */
    return x - q * y;
}
double pow(double x, double y)
{
    /* x^y = 2^(y*log2(x)) with the x87 */
    double r;
    if (x <= 0) return 0;
    __asm__("fyl2x\n\t"                     /* v = y*log2(x) */
            "fld %%st(0)\n\t"               /* v, v */
            "frndint\n\t"                   /* int(v), v */
            "fxch\n\t"                      /* v, int(v) */
            "fsub %%st(1), %%st(0)\n\t"     /* frac, int(v) */
            "f2xm1\n\t"
            "fld1\n\t"
            "faddp\n\t"
            "fscale\n\t"
            "fstp %%st(1)"
            : "=t"(r) : "0"(x), "u"(y) : "st(1)", "st(2)");
    return r;
}

/* ---------------------------------------------------------------- printf */
struct outbuf { char *p; size_t left; size_t total; FILE *f; };

static void out_ch(struct outbuf *o, char c)
{
    if (o->f) { fputc(c, o->f); }
    else if (o->left > 1) { *o->p++ = c; o->left--; }
    o->total++;
}

static void out_pad(struct outbuf *o, int n, char c) { while (n-- > 0) out_ch(o, c); }

static int format(struct outbuf *o, const char *fmt, va_list ap)
{
    char tmp[32];
    for (; *fmt; fmt++) {
        int left = 0, zero = 0, plus = 0, width = 0, prec = -1, lng = 0;
        const char *s; int len, neg, base, upper, c;
        unsigned long long uv; long long sv;
        if (*fmt != '%') { out_ch(o, *fmt); continue; }
        fmt++;
        for (;; fmt++) {
            if (*fmt == '-') left = 1; else if (*fmt == '0') zero = 1;
            else if (*fmt == '+') plus = 1; else if (*fmt == ' ' || *fmt == '#') ;
            else break;
        }
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; }
        else while (isdigit(*fmt)) width = width * 10 + *fmt++ - '0';
        if (*fmt == '.') {
            fmt++; prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (isdigit(*fmt)) prec = prec * 10 + *fmt++ - '0';
        }
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') { if (*fmt == 'l') lng++; fmt++; }
        c = *fmt;
        if (!c) break;
        neg = 0; base = 10; upper = 0;
        switch (c) {
        case '%': out_ch(o, '%'); continue;
        case 'c': tmp[0] = (char)va_arg(ap, int); s = tmp; len = 1; goto emit;
        case 's':
            s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            len = strlen(s);
            if (prec >= 0 && len > prec) len = prec;
            goto emit;
        case 'd': case 'i':
            sv = lng > 1 ? va_arg(ap, long long) : va_arg(ap, int);
            if (sv < 0) { neg = 1; uv = -(unsigned long long)sv; } else uv = sv;
            goto number;
        case 'u': uv = lng > 1 ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned); goto number;
        case 'X': upper = 1; /* fall through */
        case 'x': base = 16; uv = lng > 1 ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned); goto number;
        case 'p': base = 16; uv = (uint32_t)va_arg(ap, void *); goto number;
        case 'o': base = 8; uv = va_arg(ap, unsigned); goto number;
        case 'f': case 'g': case 'e': {
            double d = va_arg(ap, double); long ip; unsigned frac; int p = prec < 0 ? 6 : prec; int i;
            if (d < 0) { neg = 1; d = -d; }
            ip = (long)d; d -= ip;
            for (i = 0; i < p; i++) d *= 10;
            frac = (unsigned)(d + 0.5);
            len = 0;
            {
                char ib[16]; int n = 0; long v = ip;
                do { ib[n++] = '0' + v % 10; v /= 10; } while (v);
                while (n) tmp[len++] = ib[--n];
            }
            if (p) { char fb[16]; int n = p; tmp[len++] = '.'; while (n) { fb[--n] = '0' + frac % 10; frac /= 10; } for (n = 0; n < p; n++) tmp[len++] = fb[n]; }
            tmp[len] = 0; s = tmp;
            goto emit_signed;
        }
        default: out_ch(o, '%'); out_ch(o, c); continue;
        }
number:
        len = 0;
        if (uv == 0) tmp[len++] = '0';
        while (uv) {
            int d = (int)(uv % base);
            tmp[len++] = d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10;
            uv /= base;
        }
        while (len < prec && len < 30) tmp[len++] = '0';   /* %.3d: minimum digits */
        /* reverse */
        { int i; for (i = 0; i < len / 2; i++) { char t = tmp[i]; tmp[i] = tmp[len - 1 - i]; tmp[len - 1 - i] = t; } }
        tmp[len] = 0; s = tmp;
        if (prec > len) { out_pad(o, 0, ' '); }
emit_signed:
        {
            int signlen = (neg || plus) ? 1 : 0;
            int pad = width - len - signlen;
            if (!left && !zero) out_pad(o, pad, ' ');
            if (neg) out_ch(o, '-'); else if (plus) out_ch(o, '+');
            if (!left && zero) out_pad(o, pad, '0');
            while (len--) out_ch(o, *s++);
            if (left) out_pad(o, pad, ' ');
            continue;
        }
emit:
        {
            int pad = width - len;
            if (!left) out_pad(o, pad, ' ');
            while (len--) out_ch(o, *s++);
            if (left) out_pad(o, pad, ' ');
        }
    }
    return (int)o->total;
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    struct outbuf o = { buf, n, 0, 0 };
    int r = format(&o, fmt, ap);
    if (n) *o.p = 0;
    return r;
}
int vsprintf(char *buf, const char *fmt, va_list ap) { return vsnprintf(buf, 0x7fffffff, fmt, ap); }
int snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap; int r; va_start(ap, fmt); r = vsnprintf(buf, n, fmt, ap); va_end(ap); return r;
}
int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap; int r; va_start(ap, fmt); r = vsprintf(buf, fmt, ap); va_end(ap); return r;
}
int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    struct outbuf o = { 0, 0, 0, f };
    return format(&o, fmt, ap);
}
int vprintf(const char *fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }
int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap; int r; va_start(ap, fmt); r = vfprintf(f, fmt, ap); va_end(ap); return r;
}
int printf(const char *fmt, ...)
{
    va_list ap; int r; va_start(ap, fmt); r = vfprintf(stdout, fmt, ap); va_end(ap); return r;
}

int sscanf(const char *s, const char *fmt, ...)
{
    va_list ap; int n = 0;
    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (isspace(*fmt)) { while (isspace(*s)) s++; continue; }
        if (*fmt != '%') { if (*s != *fmt) break; s++; continue; }
        fmt++;
        while (isdigit(*fmt)) fmt++;
        while (*fmt == 'l' || *fmt == 'h') fmt++;
        if (*fmt == 'd' || *fmt == 'i' || *fmt == 'x' || *fmt == 'u') {
            char *end; long v;
            while (isspace(*s)) s++;
            v = strtol(s, &end, *fmt == 'x' ? 16 : *fmt == 'i' ? 0 : 10);
            if (end == s) break;
            *va_arg(ap, int *) = (int)v; s = end; n++;
        } else if (*fmt == 's') {
            char *d = va_arg(ap, char *);
            while (isspace(*s)) s++;
            if (!*s) break;
            while (*s && !isspace(*s)) *d++ = *s++;
            *d = 0; n++;
        } else if (*fmt == 'c') {
            if (!*s) break;
            *va_arg(ap, char *) = *s++; n++;
        } else break;
    }
    va_end(ap);
    return n;
}

/* ---------------------------------------------------------------- stdio */
struct nano_file {
    int fd; int eof; long pos;
    int bpos, blen;
    int writing;                        /* buf holds bytes on their way out */
    unsigned char buf[1024];
};
static FILE con_in = { 0 }, con_out = { 1 }, con_err = { 2 };
FILE *stdin = &con_in, *stdout = &con_out, *stderr = &con_err;

static void con_write(const char *s, size_t n)
{
    char tmp[128];
    while (n) {
        size_t k = n > sizeof tmp - 1 ? sizeof tmp - 1 : n, i, j = 0;
        for (i = 0; i < k; i++) tmp[j++] = s[i];
        tmp[j] = 0;
        sys_puts(tmp);
        s += k; n -= k;
    }
}

FILE *fopen(const char *path, const char *mode)
{
    FILE *f; int fd, writing = 0;
    if (strchr(mode, 'w')) writing = 1;
    else if (strchr(mode, 'a') || strchr(mode, '+')) return 0;   /* not supported */
    fd = writing ? sys_create(path) : sys_open(path);
    if (fd < 0) return 0;
    f = calloc(1, sizeof *f);
    if (!f) { sys_close(fd); return 0; }
    f->fd = fd;
    f->writing = writing;
    return f;
}

/* push whatever is buffered out to the file */
static int flush_out(FILE *f)
{
    int n;
    if (!f->writing || f->bpos == 0) return 0;
    n = sys_write(f->fd, f->buf, f->bpos);
    f->bpos = 0;
    return n < 0 ? EOF : 0;
}

int fclose(FILE *f)
{
    if (!f || f->fd <= 2) return 0;
    flush_out(f);
    sys_close(f->fd);
    free(f);
    return 0;
}

static int fill(FILE *f)
{
    int n = sys_read(f->fd, f->buf, sizeof f->buf);
    f->bpos = 0; f->blen = n > 0 ? n : 0;
    if (n <= 0) { f->eof = 1; return 0; }
    return 1;
}

int fgetc(FILE *f)
{
    if (f->fd <= 2) return EOF;
    if (f->bpos >= f->blen && !fill(f)) return EOF;
    f->pos++;
    return f->buf[f->bpos++];
}

char *fgets(char *s, int n, FILE *f)
{
    int i = 0, c;
    while (i < n - 1) {
        c = fgetc(f);
        if (c == EOF) break;
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    s[i] = 0;
    return i ? s : 0;
}

size_t fread(void *buf, size_t size, size_t n, FILE *f)
{
    size_t want = size * n, got = 0; char *d = buf;
    if (f->fd <= 2) return 0;
    while (got < want) {
        if (f->bpos < f->blen) {
            size_t k = f->blen - f->bpos;
            if (k > want - got) k = want - got;
            memcpy(d + got, f->buf + f->bpos, k);
            f->bpos += k; got += k;
        } else if (want - got >= sizeof f->buf) {
            int r = sys_read(f->fd, d + got, (int)((want - got) & ~1023u));
            if (r <= 0) { f->eof = 1; break; }
            got += r;
        } else if (!fill(f)) break;
    }
    f->pos += got;
    return size ? got / size : 0;
}

size_t fwrite(const void *buf, size_t size, size_t n, FILE *f)
{
    size_t want = size * n, done = 0;
    const char *s = buf;
    if (f->fd == 1 || f->fd == 2) { con_write(buf, want); return n; }
    if (!f->writing) return 0;
    while (done < want) {
        size_t room = sizeof f->buf - f->bpos, k = want - done;
        if (k > room) k = room;
        memcpy(f->buf + f->bpos, s + done, k);
        f->bpos += k;
        done += k;
        f->pos += k;
        if (f->bpos == sizeof f->buf && flush_out(f) != 0) break;
    }
    return size ? done / size : 0;
}

int fseek(FILE *f, long off, int whence)
{
    long r;
    if (f->fd <= 2) return -1;
    if (whence == SEEK_CUR) { off += f->pos; whence = SEEK_SET; }
    r = sys_lseek(f->fd, off, whence);
    if (r < 0) return -1;
    f->pos = r; f->bpos = f->blen = 0; f->eof = 0;
    return 0;
}
long ftell(FILE *f) { return f->pos; }
int feof(FILE *f) { return f->eof; }
int fflush(FILE *f) { return f ? flush_out(f) : 0; }
int fputc(int c, FILE *f)
{
    char ch = (char)c;
    if (f->fd == 1 || f->fd == 2) { if (ch == '\n') con_write("\r", 1); con_write(&ch, 1); return c; }
    if (!f->writing) return EOF;
    if (ch == '\n') fputc('\r', f);           /* text files keep DOS line ends */
    f->buf[f->bpos++] = (unsigned char)ch;
    f->pos++;
    if (f->bpos == sizeof f->buf && flush_out(f) != 0) return EOF;
    return c;
}
int fputs(const char *s, FILE *f) { while (*s) fputc(*s++, f); return 0; }
int puts(const char *s) { fputs(s, stdout); fputc('\n', stdout); return 0; }
int putchar(int c) { return fputc(c, stdout); }

/* ---------------------------------------------------------------- unistd */
int open(const char *path, int flags, ...);
int close(int fd);
int open(const char *path, int flags, ...)
{
    if (flags & O_CREAT) return sys_create(path);
    if (flags & (O_WRONLY | O_RDWR)) {
        int fd = sys_open(path);                /* an existing file to rewrite */
        if (fd < 0) return -1;
        return fd;
    }
    return sys_open(path);
}
int read(int fd, void *buf, size_t n) { return fd > 2 ? sys_read(fd, buf, (int)n) : 0; }
int write(int fd, const void *buf, size_t n)
{
    if (fd == 1 || fd == 2) { con_write(buf, n); return (int)n; }
    return sys_write(fd, buf, (int)n);
}
long lseek(int fd, long off, int whence) { return sys_lseek(fd, off, whence); }
int close(int fd) { return sys_close(fd); }
int access(const char *path, int mode)
{
    int fd = sys_open(path);
    (void)mode;
    if (fd < 0) return -1;
    sys_close(fd);
    return 0;
}
int mkdir(const char *path, mode_t mode) { (void)path; (void)mode; return -1; }
int unlink(const char *path) { return sys_unlink(path); }
int fstat(int fd, struct stat *st)
{
    long cur = sys_lseek(fd, 0, SEEK_CUR), end = sys_lseek(fd, 0, SEEK_END);
    sys_lseek(fd, cur, SEEK_SET);
    st->st_size = end;
    return end < 0 ? -1 : 0;
}
unsigned sleep(unsigned s) { (void)s; return 0; }
int usleep(unsigned us) { (void)us; return 0; }

void setbuf(FILE *f, char *buf) { (void)f; (void)buf; }
int getchar(void) { return EOF; }
