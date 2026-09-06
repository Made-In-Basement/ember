/* png.c - PNG files, written and read.
 *
 * Writing: a deflate stream with fixed Huffman codes and a simple LZ77
 * matcher.  It is not zlib's compression, but a screen of desktop is
 * mostly flat colour and comes out ten to twenty times smaller than a
 * BMP, which on a stick the BIOS writes at a few hundred kilobytes a
 * second is the difference between a blink and a coffee break.
 *
 * Reading: the whole of inflate (stored, fixed and dynamic blocks, in
 * the manner of zlib's puff), the five scanline filters, and RGB, RGBA,
 * greyscale and paletted pictures at eight bits.  The result is the
 * picture's own size; the wallpaper code scales it to the screen.
 */
#include <nanolibc.h>
#include "nano.h"
#include "draw.h"
#include "shell.h"

/* ------------------------------------------------------------ checksums */
static uint32_t crc_table[256];

static void crc_init(void)
{
    uint32_t n, k, c;
    if (crc_table[1]) return;
    for (n = 0; n < 256; n++) {
        c = n;
        for (k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static uint32_t crc32(uint32_t c, const uint8_t *p, size_t n)
{
    crc_init();
    c = ~c;
    while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

static uint32_t adler32(const uint8_t *p, size_t n)
{
    uint32_t a = 1, b = 0;
    while (n) {
        size_t k = n > 5552 ? 5552 : n;
        n -= k;
        while (k--) { a += *p++; b += a; }
        a %= 65521;
        b %= 65521;
    }
    return (b << 16) | a;
}

/* ------------------------------------------------------------ a bit writer */
struct bits { uint8_t *out; size_t cap, n; uint32_t acc; int cnt; };

static void put_bits(struct bits *b, uint32_t v, int nbits)
{
    b->acc |= v << b->cnt;
    b->cnt += nbits;
    while (b->cnt >= 8) {
        if (b->n < b->cap) b->out[b->n] = (uint8_t)b->acc;
        b->n++;
        b->acc >>= 8;
        b->cnt -= 8;
    }
}

/* a Huffman code is sent most-significant bit first */
static void put_code(struct bits *b, uint32_t code, int len)
{
    uint32_t r = 0;
    int i;
    for (i = 0; i < len; i++) { r = (r << 1) | (code & 1); code >>= 1; }
    put_bits(b, r, len);
}

static void put_literal(struct bits *b, int lit)
{
    if (lit < 144) put_code(b, 0x30 + lit, 8);
    else put_code(b, 0x190 + (lit - 144), 9);
}

static const int len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                  67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const int len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const int dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                   1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const int dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10,
                                    11, 11, 12, 12, 13, 13 };

static void put_match(struct bits *b, int len, int dist)
{
    int i, sym;
    for (i = 28; i > 0 && len_base[i] > len; i--) ;
    sym = 257 + i;
    if (sym < 280) put_code(b, sym - 256, 7);
    else put_code(b, 0xC0 + (sym - 280), 8);
    if (len_extra[i]) put_bits(b, len - len_base[i], len_extra[i]);
    for (i = 29; i > 0 && dist_base[i] > dist; i--) ;
    put_code(b, i, 5);
    if (dist_extra[i]) put_bits(b, dist - dist_base[i], dist_extra[i]);
}

/* deflate with fixed codes: one block.  Returns the compressed size, or
   how much would have been needed if it did not fit. */
#define HASH_BITS 15
#define WINDOW    32768
static size_t deflate_fixed(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    struct bits b;
    int32_t *head, *prev;
    size_t i = 0;
    b.out = out; b.cap = cap; b.n = 0; b.acc = 0; b.cnt = 0;
    head = malloc((1 << HASH_BITS) * sizeof *head);
    prev = malloc(WINDOW * sizeof *prev);
    if (!head || !prev) { if (head) free(head); if (prev) free(prev); return (size_t)-1; }
    memset(head, 0xFF, (1 << HASH_BITS) * sizeof *head);
    put_bits(&b, 1, 1);                         /* the last block */
    put_bits(&b, 1, 2);                         /* fixed Huffman codes */
    while (i < n) {
        int best = 0, best_d = 0;
        if (i + 3 <= n) {
            uint32_t h = ((in[i] << 10) ^ (in[i + 1] << 5) ^ in[i + 2]) & ((1 << HASH_BITS) - 1);
            int32_t cand = head[h];
            int chain = 24;
            while (cand >= 0 && chain-- && i - (size_t)cand <= WINDOW - 1) {
                int l = 0, max = (int)(n - i) < 258 ? (int)(n - i) : 258;
                const uint8_t *p = in + cand, *q = in + i;
                while (l < max && p[l] == q[l]) l++;
                if (l > best) { best = l; best_d = (int)(i - cand); if (l == max) break; }
                cand = prev[cand % WINDOW];
            }
            prev[i % WINDOW] = head[h];
            head[h] = (int32_t)i;
        }
        if (best >= 3) {
            size_t k;
            put_match(&b, best, best_d);
            /* the positions inside the match still go into the table */
            for (k = i + 1; k < i + (size_t)best && k + 3 <= n; k++) {
                uint32_t h = ((in[k] << 10) ^ (in[k + 1] << 5) ^ in[k + 2]) & ((1 << HASH_BITS) - 1);
                prev[k % WINDOW] = head[h];
                head[h] = (int32_t)k;
            }
            i += best;
        } else {
            put_literal(&b, in[i]);
            i++;
        }
    }
    put_code(&b, 0, 7);                         /* end of block */
    if (b.cnt) put_bits(&b, 0, 8 - b.cnt);
    free(head);
    free(prev);
    return b.n;
}

/* ------------------------------------------------------------ writing */
static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static int chunk(int fd, const char *type, const uint8_t *data, size_t n)
{
    uint8_t head[8], tail[4];
    uint32_t c;
    be32(head, (uint32_t)n);
    memcpy(head + 4, type, 4);
    c = crc32(0, head + 4, 4);
    if (n) c = crc32(c, data, n);
    be32(tail, c);
    if (sys_write(fd, head, 8) != 8) return -1;
    {
        size_t off = 0;
        while (off < n) {
            int piece = n - off > 65536 ? 65536 : (int)(n - off);
            if (sys_write(fd, data + off, piece) != piece) return -1;
            off += piece;
        }
    }
    return sys_write(fd, tail, 4) == 4 ? 0 : -1;
}

/* w x h pixels, top row first, as 0x00RRGGBB.  0 on success. */
int png_write(const char *name, const uint32_t *px, int w, int h)
{
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    uint8_t ihdr[13], *raw, *z;
    size_t rawn = (size_t)(w * 3 + 1) * h, zcap = rawn + rawn / 8 + 64, zn;
    int fd, y, ok;

    raw = malloc(rawn);
    z = malloc(zcap);
    if (!raw || !z) { if (raw) free(raw); if (z) free(z); return -1; }
    for (y = 0; y < h; y++) {
        uint8_t *row = raw + (size_t)y * (w * 3 + 1);
        const uint32_t *src = px + (size_t)y * w;
        int x;
        row[0] = 0;                             /* no filter: the matcher does the work */
        for (x = 0; x < w; x++) {
            row[1 + x * 3] = (uint8_t)(src[x] >> 16);
            row[2 + x * 3] = (uint8_t)(src[x] >> 8);
            row[3 + x * 3] = (uint8_t)src[x];
        }
    }
    z[0] = 0x78; z[1] = 0x01;                   /* zlib: deflate, fastest */
    zn = deflate_fixed(raw, rawn, z + 2, zcap - 6);
    if (zn == (size_t)-1 || zn > zcap - 6) { free(raw); free(z); return -1; }
    be32(z + 2 + zn, adler32(raw, rawn));
    zn += 6;
    free(raw);

    fd = sys_create(name);
    if (fd < 0) { free(z); return -1; }
    be32(ihdr, w); be32(ihdr + 4, h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    ok = sys_write(fd, sig, 8) == 8 && chunk(fd, "IHDR", ihdr, 13) == 0 &&
         chunk(fd, "IDAT", z, zn) == 0 && chunk(fd, "IEND", 0, 0) == 0;
    sys_close(fd);
    free(z);
    return ok ? 0 : -1;
}

/* ------------------------------------------------------------ inflate */
struct in { const uint8_t *p; size_t n, pos; uint32_t acc; int cnt; int bad; };

static int get_bits(struct in *s, int need)
{
    while (s->cnt < need) {
        if (s->pos >= s->n) { s->bad = 1; return 0; }
        s->acc |= (uint32_t)s->p[s->pos++] << s->cnt;
        s->cnt += 8;
    }
    {
        int v = (int)(s->acc & ((1u << need) - 1));
        s->acc >>= need;
        s->cnt -= need;
        return v;
    }
}

struct huff { short count[16], symbol[320]; };

static int huff_build(struct huff *h, const short *length, int n)
{
    short offs[16];
    int sym, len, left = 1;
    for (len = 0; len < 16; len++) h->count[len] = 0;
    for (sym = 0; sym < n; sym++) h->count[length[sym]]++;
    if (h->count[0] == n) return 0;
    for (len = 1; len < 16; len++) { left <<= 1; left -= h->count[len]; if (left < 0) return -1; }
    offs[1] = 0;
    for (len = 1; len < 15; len++) offs[len + 1] = offs[len] + h->count[len];
    for (sym = 0; sym < n; sym++) if (length[sym]) h->symbol[offs[length[sym]]++] = (short)sym;
    return left;
}

static int huff_decode(struct in *s, const struct huff *h)
{
    int code = 0, first = 0, index = 0, len;
    for (len = 1; len < 16; len++) {
        code |= get_bits(s, 1);
        {
            int count = h->count[len];
            if (code - count < first) return h->symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        if (s->bad) return -1;
    }
    return -1;
}

static int codes(struct in *s, uint8_t *out, size_t cap, size_t *outn, const struct huff *lc, const struct huff *dc)
{
    for (;;) {
        int sym = huff_decode(s, lc);
        if (sym < 0 || s->bad) return -1;
        if (sym < 256) {
            if (*outn >= cap) return -1;
            out[(*outn)++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            int len, dist;
            sym -= 257;
            if (sym >= 29) return -1;
            len = len_base[sym] + get_bits(s, len_extra[sym]);
            sym = huff_decode(s, dc);
            if (sym < 0 || sym >= 30) return -1;
            dist = dist_base[sym] + get_bits(s, dist_extra[sym]);
            if ((size_t)dist > *outn || *outn + len > cap) return -1;
            while (len--) { out[*outn] = out[*outn - dist]; (*outn)++; }
        }
    }
}

static int inflate(const uint8_t *in, size_t n, uint8_t *out, size_t cap, size_t *outn)
{
    struct in s;
    struct huff lc, dc;
    short lengths[320];
    int last, type, i;
    static const short order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    s.p = in; s.n = n; s.pos = 0; s.acc = 0; s.cnt = 0; s.bad = 0;
    *outn = 0;
    do {
        last = get_bits(&s, 1);
        type = get_bits(&s, 2);
        if (type == 0) {                                        /* stored */
            unsigned len;
            s.acc = 0; s.cnt = 0;                               /* to a byte boundary */
            if (s.pos + 4 > n) return -1;
            len = in[s.pos] | (in[s.pos + 1] << 8);
            s.pos += 4;
            if (s.pos + len > n || *outn + len > cap) return -1;
            memcpy(out + *outn, in + s.pos, len);
            s.pos += len;
            *outn += len;
        } else if (type == 1) {                                 /* fixed */
            for (i = 0; i < 144; i++) lengths[i] = 8;
            for (; i < 256; i++) lengths[i] = 9;
            for (; i < 280; i++) lengths[i] = 7;
            for (; i < 288; i++) lengths[i] = 8;
            huff_build(&lc, lengths, 288);
            for (i = 0; i < 30; i++) lengths[i] = 5;
            huff_build(&dc, lengths, 30);
            if (codes(&s, out, cap, outn, &lc, &dc) != 0) return -1;
        } else if (type == 2) {                                 /* dynamic */
            int nlen = get_bits(&s, 5) + 257, ndist = get_bits(&s, 5) + 1, ncode = get_bits(&s, 4) + 4, idx;
            struct huff cl;
            if (nlen > 286 || ndist > 30) return -1;
            for (idx = 0; idx < ncode; idx++) lengths[order[idx]] = (short)get_bits(&s, 3);
            for (; idx < 19; idx++) lengths[order[idx]] = 0;
            if (huff_build(&cl, lengths, 19) != 0) return -1;
            idx = 0;
            while (idx < nlen + ndist) {
                int sym = huff_decode(&s, &cl), len;
                if (sym < 0) return -1;
                if (sym < 16) { lengths[idx++] = (short)sym; continue; }
                len = 0;
                if (sym == 16) { if (idx == 0) return -1; len = lengths[idx - 1]; sym = 3 + get_bits(&s, 2); }
                else if (sym == 17) sym = 3 + get_bits(&s, 3);
                else sym = 11 + get_bits(&s, 7);
                if (idx + sym > nlen + ndist) return -1;
                while (sym--) lengths[idx++] = (short)len;
            }
            if (huff_build(&lc, lengths, nlen) < 0) return -1;
            if (huff_build(&dc, lengths + nlen, ndist) < 0) return -1;
            if (codes(&s, out, cap, outn, &lc, &dc) != 0) return -1;
        } else {
            return -1;
        }
        if (s.bad) return -1;
    } while (!last);
    return 0;
}

/* ------------------------------------------------------------ reading */
static uint32_t rd32be(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
}

/* The file decoded to 0x00RRGGBB pixels at its own size (malloc'd), or
   0.  Eight bits a sample; RGB, RGBA, grey, grey+alpha and palette. */
uint32_t *png_decode(const char *path, int *out_w, int *out_h)
{
    uint8_t *file = 0, *idat = 0, *raw = 0, pal[768];
    uint32_t *pix = 0;
    long size, got = 0;
    size_t idatn = 0, rawn = 0, pos = 8, bpp, stride;
    int fd, w = 0, h = 0, depth = 0, ctype = 0, y, have_pal = 0;

    fd = sys_open(path);
    if (fd < 0) return 0;
    size = sys_lseek(fd, 0, SEEK_END);
    if (size <= 8 || size > 48L * 1024 * 1024 || sys_lseek(fd, 0, SEEK_SET) < 0) { sys_close(fd); return 0; }
    file = malloc((size_t)size);
    if (!file) { sys_close(fd); return 0; }
    while (got < size) {
        int piece = size - got > 65536 ? 65536 : (int)(size - got);
        int n = sys_read(fd, file + got, piece);
        if (n <= 0) break;
        got += n;
    }
    sys_close(fd);
    if (got < size || memcmp(file, "\x89PNG\r\n\x1a\n", 8) != 0) goto fail;

    idat = malloc((size_t)size);
    if (!idat) goto fail;
    while (pos + 8 <= (size_t)size) {
        uint32_t len = rd32be(file + pos);
        const uint8_t *type = file + pos + 4, *data = file + pos + 8;
        if (pos + 12 + len > (size_t)size) break;
        if (!memcmp(type, "IHDR", 4) && len >= 13) {
            w = (int)rd32be(data); h = (int)rd32be(data + 4);
            depth = data[8]; ctype = data[9];
            if (data[12] != 0) goto fail;                       /* interlaced: not read */
        } else if (!memcmp(type, "PLTE", 4) && len <= 768) {
            memcpy(pal, data, len);
            have_pal = 1;
        } else if (!memcmp(type, "IDAT", 4)) {
            memcpy(idat + idatn, data, len);
            idatn += len;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        pos += 12 + len;
    }
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || depth != 8 || idatn < 2) goto fail;
    switch (ctype) {
    case 0: bpp = 1; break;                     /* grey */
    case 2: bpp = 3; break;                     /* RGB */
    case 3: bpp = 1; if (!have_pal) goto fail; break;   /* palette */
    case 4: bpp = 2; break;                     /* grey + alpha */
    case 6: bpp = 4; break;                     /* RGBA */
    default: goto fail;
    }
    stride = (size_t)w * bpp + 1;
    raw = malloc(stride * h);
    if (!raw) goto fail;
    if (inflate(idat + 2, idatn - 2, raw, stride * h, &rawn) != 0 || rawn < stride * h) goto fail;

    /* the filters, each row from the one above */
    for (y = 0; y < h; y++) {
        uint8_t *row = raw + (size_t)y * stride, *up = y ? row - stride : 0;
        int f = row[0], x;
        uint8_t *d = row + 1;
        switch (f) {
        case 1: for (x = (int)bpp; x < (int)(stride - 1); x++) d[x] += d[x - bpp]; break;
        case 2: if (up) for (x = 0; x < (int)(stride - 1); x++) d[x] += up[1 + x]; break;
        case 3: for (x = 0; x < (int)(stride - 1); x++) {
                    int a = x >= (int)bpp ? d[x - bpp] : 0, b = up ? up[1 + x] : 0;
                    d[x] += (uint8_t)((a + b) / 2);
                } break;
        case 4: for (x = 0; x < (int)(stride - 1); x++) {
                    int a = x >= (int)bpp ? d[x - bpp] : 0, b = up ? up[1 + x] : 0, c = (up && x >= (int)bpp) ? up[1 + x - bpp] : 0;
                    d[x] += (uint8_t)paeth(a, b, c);
                } break;
        default: break;
        }
    }
    pix = malloc((size_t)w * h * 4);
    if (!pix) goto fail;
    for (y = 0; y < h; y++) {
        const uint8_t *d = raw + (size_t)y * stride + 1;
        uint32_t *out = pix + (size_t)y * w;
        int x;
        for (x = 0; x < w; x++) {
            const uint8_t *p = d + (size_t)x * bpp;
            switch (ctype) {
            case 0: case 4: out[x] = ((uint32_t)p[0] << 16) | ((uint32_t)p[0] << 8) | p[0]; break;
            case 3: out[x] = ((uint32_t)pal[p[0] * 3] << 16) | ((uint32_t)pal[p[0] * 3 + 1] << 8) | pal[p[0] * 3 + 2]; break;
            default: out[x] = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; break;
            }
        }
    }
    *out_w = w;
    *out_h = h;
    free(raw); free(idat); free(file);
    return pix;
fail:
    if (raw) free(raw);
    if (idat) free(idat);
    if (file) free(file);
    if (pix) free(pix);
    return 0;
}
