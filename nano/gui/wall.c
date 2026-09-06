/* wall.c - the desktop background.
 *
 * Either something drawn (a graded wash with the ruled grid over it, in a
 * choice of colours) or a picture read off the disk.  Pictures are plain
 * BMP files, which is deliberate: anything on a PC can save one, so no
 * conversion step stands between a photograph and the desktop.
 *
 * The picture is scaled once, when it is chosen, into a buffer the size of
 * the screen; drawing the desktop after that is a straight copy.
 */
#include <nanolibc.h>
#include "nano.h"
#include "draw.h"
#include "shell.h"

#define GRID_STEP 40

int wall_kind = WALL_EMBER;              /* what is behind everything */
char wall_file[96];
static uint32_t *picture;                /* scr_w * scr_h, when one is loaded */
static int picture_w, picture_h;

/* the drawn choices */
static const struct {
    const char *name;
    uint32_t top, bottom, grid;
} washes[] = {
    { "Ember",     0x0B0806, 0x16110A, 0x1C150E },
    { "Charcoal",  0x0A0A0B, 0x141416, 0x1B1B1E },
    { "Deep blue", 0x05070E, 0x0C1220, 0x141A2A },
    { "Forest",    0x060A07, 0x0E1610, 0x161F19 },
    { "Plum",      0x0B060D, 0x160E1A, 0x1E1524 },
};
#define WASH_COUNT (int)(sizeof washes / sizeof washes[0])

const char *wall_name(int kind)
{
    if (kind == WALL_PICTURE) return "Picture...";
    if (kind >= 0 && kind < WASH_COUNT) return washes[kind].name;
    return "?";
}

int wall_choice_count(void) { return WASH_COUNT; }

/* ---------------------------------------------------------------- JPEG */
/* picojpeg hands back one block of pixels at a time, in reading order, so
   each block can go straight into the finished wallpaper and the whole
   photograph never has to be held in memory.  Each source pixel is spread
   over the destination rectangle it covers, which scales a picture of any
   size to the screen without leaving gaps. */
#include "picojpeg.h"

static int jpg_fd;
static unsigned char jpg_buf[4096];
static int jpg_have, jpg_at;

static unsigned char jpg_feed(unsigned char *out, unsigned char want,
                              unsigned char *got, void *unused)
{
    int n = 0;
    (void)unused;
    while (n < want) {
        if (jpg_at >= jpg_have) {               /* refill from the disk */
            jpg_have = sys_read(jpg_fd, jpg_buf, sizeof jpg_buf);
            jpg_at = 0;
            if (jpg_have <= 0) break;
        }
        out[n++] = jpg_buf[jpg_at++];
    }
    *got = (unsigned char)n;
    return 0;
}

static int jpeg_load(const char *path, uint32_t *dest)
{
    pjpeg_image_info_t info;
    int mcu_x = 0, mcu_y = 0;

    jpg_fd = sys_open(path);
    if (jpg_fd < 0) return -1;
    jpg_have = jpg_at = 0;
    if (pjpeg_decode_init(&info, jpg_feed, 0, 0) != 0) {
        sys_close(jpg_fd);
        return -1;
    }
    if (info.m_width <= 0 || info.m_height <= 0) {
        sys_close(jpg_fd);
        return -1;
    }

    for (;;) {
        int bx, by, block;
        if (pjpeg_decode_mcu() != 0) break;     /* PJPG_NO_MORE_BLOCKS ends it */
        for (block = 0; block < (info.m_MCUWidth / 8) * (info.m_MCUHeight / 8);
             block++) {
            int ox = (block & 1) ? 8 : 0;
            int oy = (block & 2) ? 8 : 0;
            const unsigned char *r = info.m_pMCUBufR + block * 64;
            const unsigned char *g = info.m_pMCUBufG + block * 64;
            const unsigned char *b = info.m_pMCUBufB + block * 64;
            if (info.m_MCUWidth == 8) ox = 0;   /* one block wide */
            if (info.m_MCUHeight == 8) oy = 0;
            if (info.m_MCUWidth == 8 && info.m_MCUHeight == 16)
                oy = block * 8;
            for (by = 0; by < 8; by++) {
                int sy = mcu_y * info.m_MCUHeight + oy + by;
                int y0 = (int)((long)sy * scr_h / info.m_height);
                int y1 = (int)((long)(sy + 1) * scr_h / info.m_height);
                int yy;
                if (sy >= info.m_height) break;
                if (y1 <= y0) y1 = y0 + 1;
                if (y1 > scr_h) y1 = scr_h;
                for (bx = 0; bx < 8; bx++) {
                    int sx = mcu_x * info.m_MCUWidth + ox + bx;
                    int x0, x1, xx;
                    uint32_t c;
                    if (sx >= info.m_width) break;
                    x0 = (int)((long)sx * scr_w / info.m_width);
                    x1 = (int)((long)(sx + 1) * scr_w / info.m_width);
                    if (x1 <= x0) x1 = x0 + 1;
                    if (x1 > scr_w) x1 = scr_w;
                    c = info.m_comps == 1
                        ? (((uint32_t)r[by * 8 + bx] << 16)
                           | ((uint32_t)r[by * 8 + bx] << 8) | r[by * 8 + bx])
                        : (((uint32_t)r[by * 8 + bx] << 16)
                           | ((uint32_t)g[by * 8 + bx] << 8) | b[by * 8 + bx]);
                    for (yy = y0; yy < y1; yy++)
                        for (xx = x0; xx < x1; xx++)
                            dest[(size_t)yy * scr_w + xx] = c;
                }
            }
        }
        if (++mcu_x == info.m_MCUSPerRow) {
            mcu_x = 0;
            if (++mcu_y == info.m_MCUSPerCol) break;
        }
    }
    sys_close(jpg_fd);
    return 0;
}

/* ---------------------------------------------------------------- BMP */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Read a BMP and scale it to fill the screen.  24- and 32-bit files are
   taken as they are; an 8-bit one is looked up through its palette.  Rows
   in a BMP normally run bottom to top, which is why the sign of the height
   has to be respected. */
static int is_jpeg(const char *path)
{
    const char *d = strrchr(path, '.');
    return d && (!strcasecmp(d, ".JPG") || !strcasecmp(d, ".JPEG"));
}

uint32_t *wall_decode(const char *path)
{
    uint8_t head[54], pal[1024];
    uint8_t *row = 0;
    uint32_t *dest = 0;
    int fd, w, h, bpp, flip = 1, stride, y, x, colours = 0;
    uint32_t data_off;

    if (is_jpeg(path)) {
        uint32_t *pic = malloc((size_t)scr_w * scr_h * 4);
        if (!pic) return 0;
        if (jpeg_load(path, pic) != 0) { free(pic); return 0; }
        return pic;
    }
    fd = sys_open(path);
    if (fd < 0) return 0;
    if (sys_read(fd, head, 54) != 54 || head[0] != 'B' || head[1] != 'M')
        goto fail;
    data_off = rd32(head + 10);
    w = (int)rd32(head + 18);
    h = (int)rd32(head + 22);
    bpp = rd16(head + 28);
    if (rd32(head + 30) != 0) goto fail;            /* compressed: not read */
    if (h < 0) { h = -h; flip = 0; }
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) goto fail;
    if (bpp != 24 && bpp != 32 && bpp != 8) goto fail;
    if (bpp == 8) {
        colours = (int)rd32(head + 46);
        if (colours == 0) colours = 256;
        if (colours > 256) goto fail;
        if (sys_lseek(fd, 54, SEEK_SET) < 0) goto fail;
        if (sys_read(fd, pal, colours * 4) != colours * 4) goto fail;
    }

    stride = (w * (bpp / 8) + 3) & ~3;
    dest = malloc((size_t)scr_w * scr_h * 4);
    if (!dest) goto fail;

    /* The pixel data in one piece: a thousand small reads through the
       kernel and the BIOS took seconds, one large one takes a moment. */
    {
        long total = (long)stride * h, got = 0;
        if (total <= 0 || total > 64L * 1024 * 1024) goto fail;
        row = malloc((size_t)total);
        if (!row) goto fail;
        if (sys_lseek(fd, (long)data_off, SEEK_SET) < 0) goto fail;
        while (got < total) {
            int piece = total - got > 65536 ? 65536 : (int)(total - got);
            int n = sys_read(fd, row + got, piece);
            if (n <= 0) break;
            got += n;
        }
        if (got < total) goto fail;
    }
    for (y = 0; y < scr_h; y++) {
        int sy = (int)((long)y * h / scr_h);
        int file_row = flip ? (h - 1 - sy) : sy;
        uint32_t *out = dest + (size_t)y * scr_w;
        const uint8_t *src_row = row + (size_t)file_row * stride;
        for (x = 0; x < scr_w; x++) {
            int sx = (int)((long)x * w / scr_w);
            uint32_t c;
            if (bpp == 8) {
                const uint8_t *e = pal + src_row[sx] * 4;
                c = ((uint32_t)e[2] << 16) | ((uint32_t)e[1] << 8) | e[0];
            } else {
                const uint8_t *p = src_row + sx * (bpp / 8);
                c = ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
            }
            out[x] = c;
        }
    }
    sys_close(fd);
    free(row);
    return dest;
fail:
    sys_close(fd);
    if (row) free(row);
    if (dest) free(dest);
    return 0;
}

int wall_load(const char *path)
{
    uint32_t *pic = wall_decode(path);
    if (!pic) return -1;
    if (picture) free(picture);
    picture = pic;
    picture_w = scr_w;
    picture_h = scr_h;
    wall_kind = WALL_PICTURE;
    strncpy(wall_file, path, sizeof wall_file - 1);
    wall_file[sizeof wall_file - 1] = 0;
    return 0;
}


void wall_set(int kind)
{
    if (kind >= 0 && kind < WASH_COUNT) {
        wall_kind = kind;
        damage_all();
    }
}

/* ---------------------------------------------------------------- drawing */
void wall_draw(void)
{
    int i;
    if (wall_kind == WALL_PICTURE && picture && picture_w == scr_w) {
        /* only the part being repainted: the whole picture is megabytes,
           and a menu highlight should not cost a copy of all of it */
        int x0, y0, w, h, y;
        clip_get(&x0, &y0, &w, &h);
        for (y = y0; y < y0 + h; y++)
            memcpy(back + (size_t)y * scr_w + x0, picture + (size_t)y * scr_w + x0,
                   (size_t)w * 4);
        damage(x0, y0, w, h);
        return;
    }
    {
        int k = (wall_kind >= 0 && wall_kind < WASH_COUNT) ? wall_kind : 0;
        vgradient(0, 0, scr_w, scr_h, washes[k].top, washes[k].bottom);
        for (i = 0; i < scr_w; i += GRID_STEP)
            fill(i, 0, 1, scr_h, washes[k].grid);
        for (i = 0; i < scr_h; i += GRID_STEP)
            fill(0, i, scr_w, 1, washes[k].grid);
    }
}

/* ---------------------------------------------------------------- keeping it */
/* The choice is written to a small file so the desktop looks the same next
   time.  One line, so it can be read or edited from the DOS prompt. */
void wall_save(void)
{
    char line[160];
    int fd = sys_create("\\EMBER.CFG");
    if (fd < 0) return;
    if (wall_kind == WALL_PICTURE)
        snprintf(line, sizeof line, "background picture %s\r\n", wall_file);
    else
        snprintf(line, sizeof line, "background %d\r\n", wall_kind);
    sys_write(fd, line, (int)strlen(line));
    sys_close(fd);
}

void wall_load_config(void)
{
    char buf[200];
    int fd = sys_open("\\EMBER.CFG"), n, i;
    if (fd < 0) return;
    n = sys_read(fd, buf, sizeof buf - 1);
    sys_close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    if (memcmp(buf, "background ", 11) != 0) return;
    if (memcmp(buf + 11, "picture ", 8) == 0) {
        char path[96];
        for (i = 0; i < (int)sizeof path - 1; i++) {
            char c = buf[19 + i];
            if (c == 0 || c == '\r' || c == '\n') break;
            path[i] = c;
        }
        path[i] = 0;
        if (wall_load(path) != 0)
            wall_kind = WALL_EMBER;
    } else {
        wall_kind = buf[11] - '0';
        if (wall_kind < 0 || wall_kind >= WASH_COUNT) wall_kind = WALL_EMBER;
    }
}
