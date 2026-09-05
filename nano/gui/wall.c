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
int wall_load(const char *path)
{
    uint8_t head[54], pal[1024];
    uint8_t *row = 0;
    uint32_t *dest = 0;
    int fd, w, h, bpp, flip = 1, stride, y, x, colours = 0;
    uint32_t data_off;

    fd = sys_open(path);
    if (fd < 0) return -1;
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
    row = malloc(stride);
    dest = malloc((size_t)scr_w * scr_h * 4);
    if (!row || !dest) goto fail;

    /* Read the file once, top to bottom of the screen, pulling whichever
       source row belongs there; that way a huge picture never has to be
       held in memory all at once. */
    for (y = 0; y < scr_h; y++) {
        int sy = (int)((long)y * h / scr_h);
        int file_row = flip ? (h - 1 - sy) : sy;
        uint32_t *out = dest + (size_t)y * scr_w;
        if (sys_lseek(fd, (long)(data_off + (long)file_row * stride), SEEK_SET) < 0)
            goto fail;
        if (sys_read(fd, row, stride) != stride) goto fail;
        for (x = 0; x < scr_w; x++) {
            int sx = (int)((long)x * w / scr_w);
            uint32_t c;
            if (bpp == 8) {
                const uint8_t *e = pal + row[sx] * 4;
                c = ((uint32_t)e[2] << 16) | ((uint32_t)e[1] << 8) | e[0];
            } else {
                const uint8_t *p = row + sx * (bpp / 8);
                c = ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
            }
            out[x] = c;
        }
    }
    sys_close(fd);
    free(row);
    if (picture) free(picture);
    picture = dest;
    picture_w = scr_w;
    picture_h = scr_h;
    wall_kind = WALL_PICTURE;
    strncpy(wall_file, path, sizeof wall_file - 1);
    wall_file[sizeof wall_file - 1] = 0;
    return 0;
fail:
    sys_close(fd);
    if (row) free(row);
    if (dest) free(dest);
    return -1;
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
        size_t n = (size_t)scr_w * scr_h;
        memcpy(back, picture, n * 4);
        damage(0, 0, scr_w, scr_h);
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
