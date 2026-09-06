/* shot.c - a picture of the screen.
 *
 * The back buffer is written as a 24-bit BMP, \SHOT0001.BMP and up, the
 * first name not yet taken.  Print Screen or the menu does it; the viewer
 * can open the result, and any other machine can read it.
 */
#include <nanolibc.h>
#include "nano.h"
#include "draw.h"
#include "shell.h"

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

/* 0 on success, with the name written to out */
int screenshot_save(char *out, int out_size)
{
    uint8_t head[54], *buf, *row;
    int stride = (scr_w * 3 + 3) & ~3, n, h, y, off = 0;
    uint32_t size = 54 + (uint32_t)stride * scr_h;

    for (n = 1; n < 10000; n++) {
        snprintf(out, out_size, "\\SHOT%04d.BMP", n);
        h = sys_open(out);
        if (h < 0) break;
        sys_close(h);
    }
    /* Assembled in memory first: a stick written one screen row at a time
       took a minute; in 64 KB pieces it takes a few seconds. */
    buf = malloc((size_t)stride * scr_h);
    if (!buf) return -1;
    h = sys_create(out);
    if (h < 0) { free(buf); return -1; }

    memset(head, 0, sizeof head);
    head[0] = 'B'; head[1] = 'M';
    put32(head + 2, size);
    put32(head + 10, 54);
    put32(head + 14, 40);
    put32(head + 18, scr_w);
    put32(head + 22, scr_h);                    /* positive: bottom row first */
    put16(head + 26, 1);
    put16(head + 28, 24);
    put32(head + 34, (uint32_t)stride * scr_h);
    put32(head + 38, 2835);
    put32(head + 42, 2835);
    if (sys_write(h, head, 54) != 54) { free(buf); sys_close(h); return -1; }

    for (y = scr_h - 1; y >= 0; y--) {
        const uint32_t *src = back + (size_t)y * scr_w;
        int x;
        row = buf + (size_t)(scr_h - 1 - y) * stride;
        for (x = 0; x < scr_w; x++) {
            row[x * 3] = (uint8_t)src[x];
            row[x * 3 + 1] = (uint8_t)(src[x] >> 8);
            row[x * 3 + 2] = (uint8_t)(src[x] >> 16);
        }
        for (x = scr_w * 3; x < stride; x++) row[x] = 0;
    }
    while (off < stride * scr_h) {
        int piece = stride * scr_h - off;
        if (piece > 65536) piece = 65536;
        if (sys_write(h, buf + off, piece) != piece) { free(buf); sys_close(h); return -1; }
        off += piece;
    }
    free(buf);
    sys_close(h);
    return 0;
}

void app_screenshot(void)
{
    char name[32];
    if (screenshot_save(name, sizeof name) == 0) app_notice("Screenshot", name, "saved; open it in Pictures");
    else app_notice("Screenshot", "Could not write the file", "");
}
