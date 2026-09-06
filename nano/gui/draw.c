/* draw.c - the drawing floor of the shell.
 *
 * The card is asked for the largest true-colour mode it will give us with
 * a linear framebuffer, which a 32-bit program can write to directly.
 * Everything is composed into an ordinary block of memory first and only
 * the rectangles that changed are copied to the screen; that is what keeps
 * dragging a window smooth and free of flicker, and it costs one pass over
 * a few hundred kilobytes rather than three megabytes a frame.
 *
 * The screen may be 32, 24 or 16 bits per pixel.  Nothing above this file
 * needs to know: the conversion happens once, on the way out.
 */
#include <nanolibc.h>
#include "nano.h"
#include "draw.h"
#include "guifont.h"
#include "guiart.h"

int scr_w, scr_h;
uint32_t *back;

static uint8_t *fb;                     /* the card's memory */
static int fb_pitch, fb_bpp;
static int dmg_x0, dmg_y0, dmg_x1, dmg_y1;      /* what changed */
static int cx0, cy0, cx1, cy1;                  /* the clip rectangle */

static const struct font *faces[F_COUNT];

/* ---------------------------------------------------------------- setup */
/* Walk the card's own list of modes and take the best true-colour one that
   fits the size we want, so we are not guessing mode numbers. */
static int pick_mode(int want_w, int want_h, struct vbe_mode *best)
{
    struct vbe_info info;
    struct vbe_mode m;
    int found = -1, best_score = -1, i;

    if (sys_vbe_info(&info) != 0)
        return -1;
    for (i = 0; i < info.mode_count; i++) {
        int mode = info.modes[i], score;
        if (sys_vbe_mode(mode, &m) != 0) continue;
        if (!m.framebuffer) continue;                   /* must be linear */
        if (m.bpp != 32 && m.bpp != 24 && m.bpp != 16) continue;
        if (m.width > want_w || m.height > want_h) continue;
        if (m.width < 640 || m.height < 480) continue;
        /* A screen that is not the shape of the panel gets stretched to fit
           it, so a widescreen mode is worth more than a bigger square one. */
        {
            int aspect = m.width * 100 / m.height, shape;
            if (aspect >= 172 && aspect <= 182) shape = 3;          /* 16:9 */
            else if (aspect >= 155 && aspect <= 165) shape = 2;     /* 16:10 */
            else if (aspect >= 128 && aspect <= 136) shape = 1;     /* 4:3 */
            else shape = 0;
            /* the shape decides first, the size only settles ties within it */
            score = shape * 4000000 + m.width * m.height;
            if (m.bpp == 32) score += 4;                /* prefer 32, then 24 */
            else if (m.bpp == 24) score += 2;
        }
        if (score > best_score) {
            best_score = score;
            found = mode;
            *best = m;
        }
    }
    return found;
}

int draw_open(int want_w, int want_h)
{
    struct vbe_mode m;
    int mode = pick_mode(want_w, want_h, &m);
    if (mode < 0)
        return -1;
    if (sys_set_vbe_mode(mode, 1) != 0)
        return -1;
    scr_w = m.width;
    scr_h = m.height;
    fb_pitch = m.pitch;
    fb_bpp = m.bpp;
    fb = (uint8_t *)m.framebuffer;
    back = malloc((size_t)scr_w * scr_h * 4);
    if (!back) {
        sys_set_video_mode(3);
        return -1;
    }
    faces[F_SMALL] = &font_small;
    faces[F_NORMAL] = &font_normal;
    faces[F_BOLD] = &font_bold;
    faces[F_TITLE] = &font_title;
    faces[F_CLOCK] = &font_clock;
    clip_none();
    damage_all();
    return 0;
}

void draw_close(void)
{
    sys_set_video_mode(3);
}

/* ---------------------------------------------------------------- damage */
/* What has to reach the screen.  Bounded by the clip: a repaint of one
   region must not push the whole screen. */
void damage(int x, int y, int w, int h)
{
    int x1 = x + w, y1 = y + h;
    if (x < cx0) x = cx0;
    if (y < cy0) y = cy0;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;
    w = x1 - x;
    h = y1 - y;
    if (w <= 0 || h <= 0) return;
    if (x < dmg_x0) dmg_x0 = x;
    if (y < dmg_y0) dmg_y0 = y;
    if (x + w > dmg_x1) dmg_x1 = x + w;
    if (y + h > dmg_y1) dmg_y1 = y + h;
}

void damage_all(void)
{
    dmg_x0 = dmg_y0 = 0;
    dmg_x1 = scr_w;
    dmg_y1 = scr_h;
}

void draw_present(void)
{
    int y, x;
    if (dmg_x0 < 0) dmg_x0 = 0;
    if (dmg_y0 < 0) dmg_y0 = 0;
    if (dmg_x1 > scr_w) dmg_x1 = scr_w;
    if (dmg_y1 > scr_h) dmg_y1 = scr_h;
    if (dmg_x0 >= dmg_x1 || dmg_y0 >= dmg_y1)
        return;
    draw_present_bytes += (unsigned long)(dmg_x1 - dmg_x0) * (dmg_y1 - dmg_y0) * (fb_bpp / 8);
    for (y = dmg_y0; y < dmg_y1; y++) {
        const uint32_t *src = back + (size_t)y * scr_w + dmg_x0;
        uint8_t *dst = fb + (size_t)y * fb_pitch;
        int n = dmg_x1 - dmg_x0;
        if (fb_bpp == 32) {
            /* A block move, not a loop: the card's memory is not cached,
               and every write to it is slow enough that the difference
               shows on the screen. */
            uint32_t *d = (uint32_t *)dst + dmg_x0;
            int count = n;
            __asm__ volatile("rep movsl"
                             : "+D"(d), "+S"(src), "+c"(count)
                             : : "memory");
        } else if (fb_bpp == 24) {
            uint8_t *d = dst + dmg_x0 * 3;
            for (x = 0; x < n; x++) {
                uint32_t c = src[x];
                *d++ = (uint8_t)c;
                *d++ = (uint8_t)(c >> 8);
                *d++ = (uint8_t)(c >> 16);
            }
        } else {
            uint16_t *d = (uint16_t *)dst + dmg_x0;
            for (x = 0; x < n; x++) {
                uint32_t c = src[x];
                d[x] = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0)
                                  | ((c >> 3) & 0x001F));
            }
        }
    }
    dmg_x0 = scr_w; dmg_y0 = scr_h; dmg_x1 = 0; dmg_y1 = 0;
}

/* ---------------------------------------------------------------- clipping */
void clip_set(int x, int y, int w, int h)
{
    cx0 = x < 0 ? 0 : x;
    cy0 = y < 0 ? 0 : y;
    cx1 = x + w > scr_w ? scr_w : x + w;
    cy1 = y + h > scr_h ? scr_h : y + h;
}

/* does a box touch what is being repainted?  Whole windows, icons and
   glyphs are skipped on the strength of this, which is what makes a
   region repaint cheap. */
int clip_intersects(int x, int y, int w, int h)
{
    return x < cx1 && x + w > cx0 && y < cy1 && y + h > cy0;
}

void clip_none(void)
{
    cx0 = cy0 = 0;
    cx1 = scr_w;
    cy1 = scr_h;
}


void clip_get(int *x, int *y, int *w, int *h)
{
    *x = cx0; *y = cy0; *w = cx1 - cx0; *h = cy1 - cy0;
}

/* ---------------------------------------------------------------- pixels */
uint32_t mix(uint32_t a, uint32_t b, int t)
{
    int r = ((int)((a >> 16) & 0xFF) * (255 - t) + (int)((b >> 16) & 0xFF) * t) >> 8;
    int g = ((int)((a >> 8) & 0xFF) * (255 - t) + (int)((b >> 8) & 0xFF) * t) >> 8;
    int bl = ((int)(a & 0xFF) * (255 - t) + (int)(b & 0xFF) * t) >> 8;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

void pixel_blend(int x, int y, uint32_t c, int alpha)
{
    uint32_t *p;
    if (x < cx0 || x >= cx1 || y < cy0 || y >= cy1 || alpha <= 0) return;
    p = back + (size_t)y * scr_w + x;
    *p = alpha >= 255 ? c : mix(*p, c, alpha);
}

void fill(int x, int y, int w, int h, uint32_t c)
{
    int yy, xx;
    int x0 = x < cx0 ? cx0 : x, y0 = y < cy0 ? cy0 : y;
    int x1 = x + w > cx1 ? cx1 : x + w, y1 = y + h > cy1 ? cy1 : y + h;
    if (x0 >= x1 || y0 >= y1) return;
    damage(x0, y0, x1 - x0, y1 - y0);
    for (yy = y0; yy < y1; yy++) {
        uint32_t *p = back + (size_t)yy * scr_w + x0;
        for (xx = x0; xx < x1; xx++) *p++ = c;
    }
}

void fill_alpha(int x, int y, int w, int h, uint32_t c, int alpha)
{
    if (!clip_intersects(x, y, w, h)) return;   /* nothing of it shows */
    int yy, xx;
    int x0 = x < cx0 ? cx0 : x, y0 = y < cy0 ? cy0 : y;
    int x1 = x + w > cx1 ? cx1 : x + w, y1 = y + h > cy1 ? cy1 : y + h;
    if (x0 >= x1 || y0 >= y1 || alpha <= 0) return;
    if (alpha >= 255) { fill(x, y, w, h, c); return; }
    damage(x0, y0, x1 - x0, y1 - y0);
    for (yy = y0; yy < y1; yy++) {
        uint32_t *p = back + (size_t)yy * scr_w + x0;
        for (xx = x0; xx < x1; xx++, p++) *p = mix(*p, c, alpha);
    }
}

void vgradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom)
{
    if (!clip_intersects(x, y, w, h)) return;   /* nothing of it shows */
    int yy;
    if (h <= 0) return;
    for (yy = 0; yy < h; yy++)
        fill(x, y + yy, w, 1, mix(top, bottom, h == 1 ? 0 : yy * 255 / (h - 1)));
}

void hgradient(int x, int y, int w, int h, uint32_t left, uint32_t right)
{
    if (!clip_intersects(x, y, w, h)) return;   /* nothing of it shows */
    int xx;
    if (w <= 0) return;
    for (xx = 0; xx < w; xx++)
        fill(x + xx, y, 1, h, mix(left, right, w == 1 ? 0 : xx * 255 / (w - 1)));
}

void frame(int x, int y, int w, int h, uint32_t c)
{
    fill(x, y, w, 1, c);
    fill(x, y + h - 1, w, 1, c);
    fill(x, y, 1, h, c);
    fill(x + w - 1, y, 1, h, c);
}

/* Rounded corners, worked out with the circle equation and softened at the
   edge so they do not look like stairs. */
static void corner_span(int cx, int cy, int r, int y, int *from, int *to)
{
    int dy = y - cy;
    int dx2 = r * r - dy * dy;
    int dx = 0;
    while ((dx + 1) * (dx + 1) <= dx2) dx++;
    *from = cx - dx;
    *to = cx + dx;
}

void round_fill(int x, int y, int w, int h, int r, uint32_t c)
{
    if (!clip_intersects(x, y, w, h)) return;   /* nothing of it shows */
    int yy;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r <= 0) { fill(x, y, w, h, c); return; }
    fill(x, y + r, w, h - 2 * r, c);
    for (yy = 0; yy < r; yy++) {
        int from, to;
        corner_span(x + r, y + r, r, y + yy, &from, &to);
        fill(from, y + yy, (x + w - r) + (to - (x + r)) - from, 1, c);
        corner_span(x + r, y + h - 1 - r, r, y + h - 1 - yy, &from, &to);
        fill(from, y + h - 1 - yy, (x + w - r) + (to - (x + r)) - from, 1, c);
    }
}

void round_frame(int x, int y, int w, int h, int r, uint32_t c)
{
    if (!clip_intersects(x, y, w, h)) return;   /* nothing of it shows */
    int yy;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    fill(x + r, y, w - 2 * r, 1, c);
    fill(x + r, y + h - 1, w - 2 * r, 1, c);
    fill(x, y + r, 1, h - 2 * r, c);
    fill(x + w - 1, y + r, 1, h - 2 * r, c);
    for (yy = 0; yy < r; yy++) {
        int from, to;
        corner_span(x + r, y + r, r, y + yy, &from, &to);
        pixel_blend(from, y + yy, c, 255);
        pixel_blend(x + w - 1 - (x + r - from), y + yy, c, 255);
        corner_span(x + r, y + h - 1 - r, r, y + h - 1 - yy, &from, &to);
        pixel_blend(from, y + h - 1 - yy, c, 255);
        pixel_blend(x + w - 1 - (x + r - from), y + h - 1 - yy, c, 255);
    }
}

static void round_fill_alpha_edge(int x, int y, int w, int h, int r, int alpha);
void round_frame_alpha(int x, int y, int w, int h, int r, uint32_t c, int alpha);

/* A soft edge under a window, so it lifts off the desktop. */
void shadow(int x, int y, int w, int h, int r, int spread)
{
    if (!clip_intersects(x - spread, y - spread, w + 2 * spread, h + 2 * spread)) return;   /* nothing of it shows */
    int i;
    for (i = spread; i >= 1; i--) {
        int a = 40 / i;
        if (a < 3) a = 3;
        round_fill_alpha_edge(x - i, y - i + 2, w + 2 * i, h + 2 * i, r + i, a);
    }
}

void round_fill_alpha(int x, int y, int w, int h, int r, uint32_t c, int alpha)
{
    if (!clip_intersects(x, y, w, h)) return;   /* nothing of it shows */
    int yy;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r <= 0) { fill_alpha(x, y, w, h, c, alpha); return; }
    fill_alpha(x, y + r, w, h - 2 * r, c, alpha);
    for (yy = 0; yy < r; yy++) {
        int from, to;
        corner_span(x + r, y + r, r, y + yy, &from, &to);
        fill_alpha(from, y + yy, (x + w - r) + (to - (x + r)) - from, 1, c, alpha);
        corner_span(x + r, y + h - 1 - r, r, y + h - 1 - yy, &from, &to);
        fill_alpha(from, y + h - 1 - yy, (x + w - r) + (to - (x + r)) - from, 1,
                   c, alpha);
    }
}

static void round_fill_alpha_edge(int x, int y, int w, int h, int r, int alpha)
{
    round_fill_alpha(x, y, w, h, r, 0x000000, alpha);
}

/* A filled polygon, by scanline: what the crystal and the icons are cut from. */
void poly_fill_alpha(const int *pts, int n, uint32_t c, int alpha)
{
    int y, ymin = pts[1], ymax = pts[1], i;
    for (i = 1; i < n; i++) {
        if (pts[i * 2 + 1] < ymin) ymin = pts[i * 2 + 1];
        if (pts[i * 2 + 1] > ymax) ymax = pts[i * 2 + 1];
    }
    if (ymin < cy0) ymin = cy0;
    if (ymax > cy1 - 1) ymax = cy1 - 1;
    for (y = ymin; y <= ymax; y++) {
        int xs[16], count = 0, j, k;
        for (i = 0, j = n - 1; i < n; j = i++) {
            int y0 = pts[j * 2 + 1], y1 = pts[i * 2 + 1];
            int x0 = pts[j * 2], x1 = pts[i * 2];
            if ((y0 <= y && y1 > y) || (y1 <= y && y0 > y)) {
                if (count < 16)
                    xs[count++] = x0 + (y - y0) * (x1 - x0) / (y1 - y0);
            }
        }
        for (i = 1; i < count; i++) {           /* the crossings, in order */
            int v = xs[i];
            for (k = i; k > 0 && xs[k - 1] > v; k--) xs[k] = xs[k - 1];
            xs[k] = v;
        }
        for (i = 0; i + 1 < count; i += 2)
            fill_alpha(xs[i], y, xs[i + 1] - xs[i] + 1, 1, c, alpha);
    }
}

void poly_fill(const int *pts, int n, uint32_t c)
{
    poly_fill_alpha(pts, n, c, 255);
}

/* A pool of light: alpha falls off smoothly with distance, so it reads as
   a glow rather than as a stack of rectangles. */
void soft_ellipse(int cx, int cy, int rx, int ry, uint32_t c, int max_alpha)
{
    if (!clip_intersects(cx - rx, cy - ry, 2 * rx, 2 * ry)) return;   /* nothing of it shows */
    int x, y;
    int x0 = cx - rx, x1 = cx + rx, y0 = cy - ry, y1 = cy + ry;
    if (rx <= 0 || ry <= 0) return;
    if (x0 < cx0) x0 = cx0;
    if (y0 < cy0) y0 = cy0;
    if (x1 > cx1 - 1) x1 = cx1 - 1;
    if (y1 > cy1 - 1) y1 = cy1 - 1;
    for (y = y0; y <= y1; y++) {
        int dy = y - cy;
        int ny = (dy * dy * 65536) / (ry * ry);
        uint32_t *row = back + (size_t)y * scr_w;
        if (ny >= 65536) continue;
        for (x = x0; x <= x1; x++) {
            int dx = x - cx;
            int d = ny + (dx * dx * 65536) / (rx * rx);
            int t, a;
            if (d >= 65536) continue;
            t = (65536 - d) >> 8;               /* 0..256 */
            a = max_alpha * t * t / 65536;
            if (a > 0) row[x] = mix(row[x], c, a);
        }
    }
    damage(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
}

/* a halo, for whatever has the focus */
void glow(int x, int y, int w, int h, int r, uint32_t c, int rings)
{
    if (!clip_intersects(x - rings, y - rings, w + 2 * rings, h + 2 * rings)) return;   /* nothing of it shows */
    int i;
    for (i = rings; i >= 1; i--) {
        int a = 60 / (i + 1);
        int ox, oy, ow, oh;
        clip_get(&ox, &oy, &ow, &oh);
        (void)ox; (void)oy; (void)ow; (void)oh;
        round_frame_alpha(x - i, y - i, w + 2 * i, h + 2 * i, r + i, c, a);
    }
}

void round_frame_alpha(int x, int y, int w, int h, int r, uint32_t c, int alpha)
{
    if (!clip_intersects(x, y, w, h)) return;   /* nothing of it shows */
    int yy;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    fill_alpha(x + r, y, w - 2 * r, 1, c, alpha);
    fill_alpha(x + r, y + h - 1, w - 2 * r, 1, c, alpha);
    fill_alpha(x, y + r, 1, h - 2 * r, c, alpha);
    fill_alpha(x + w - 1, y + r, 1, h - 2 * r, c, alpha);
    for (yy = 0; yy < r; yy++) {
        int from, to;
        corner_span(x + r, y + r, r, y + yy, &from, &to);
        pixel_blend(from, y + yy, c, alpha);
        pixel_blend(x + w - 1 - (x + r - from), y + yy, c, alpha);
        corner_span(x + r, y + h - 1 - r, r, y + h - 1 - yy, &from, &to);
        pixel_blend(from, y + h - 1 - yy, c, alpha);
        pixel_blend(x + w - 1 - (x + r - from), y + h - 1 - yy, c, alpha);
    }
}

/* a lit top edge and a dark bottom one: the cheapest way to give a panel depth */
void inset(int x, int y, int w, int h, int r, uint32_t light, uint32_t dark)
{
    fill(x + r, y, w - 2 * r, 1, light);
    fill(x + r, y + h - 1, w - 2 * r, 1, dark);
}

/* ---------------------------------------------------------------- the clock */
static uint64_t tsc_hz = 2000000000u, tsc_base;

static uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void clock_start(void)
{
    uint8_t p61 = inb(0x61);
    uint64_t t0, t1;
    unsigned guard = 0;
    outb(0x61, (uint8_t)((p61 & ~0x02) | 0x01));    /* gate on, speaker off */
    outb(0x43, 0xB0);                               /* channel 2, lo/hi, mode 0 */
    outb(0x42, 0xFF);
    outb(0x42, 0xFF);
    t0 = rdtsc();
    while (!(inb(0x61) & 0x20))
        if (++guard > 200000000u) break;
    t1 = rdtsc();
    outb(0x61, (uint8_t)(p61 & ~0x03));
    if (guard <= 200000000u && t1 - t0 >= 100000)
        tsc_hz = (t1 - t0) * 1193182u / 65535u;
    tsc_base = rdtsc();
}

unsigned now_ms(void)
{
    return (unsigned)((rdtsc() - tsc_base) / (tsc_hz / 1000u));
}

unsigned now_us(void)
{
    return (unsigned)((rdtsc() - tsc_base) / (tsc_hz / 1000000u));
}

uint32_t draw_fb_phys(void) { return (uint32_t)fb; }
int      draw_fb_bpp(void)  { return fb_bpp; }
unsigned long draw_present_bytes;       /* pushed to the card so far */

/* ---------------------------------------------------------------- pictures */
/* A column range of a picture, blended over what is already there.  Alpha
   is the artist's, so soft edges stay soft over any background. */
void image_draw_part(const struct image *im, int sx, int sw, int x, int y)
{
    int row, col;
    if (!im || !im->bgra) return;
    if (sx < 0) { sw += sx; x -= sx; sx = 0; }
    if (sx + sw > im->w) sw = im->w - sx;
    if (sw <= 0) return;
    if (!clip_intersects(x, y, sw, im->h)) return;
    for (row = 0; row < im->h; row++) {
        int py = y + row;
        const unsigned char *p;
        uint32_t *out;
        if (py < cy0 || py >= cy1) continue;
        p = im->bgra + ((size_t)row * im->w + sx) * 4;
        out = back + (size_t)py * scr_w;
        for (col = 0; col < sw; col++, p += 4) {
            int px = x + col;
            int a = p[3];
            uint32_t c;
            if (px < cx0 || px >= cx1 || a == 0) continue;
            c = ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
            out[px] = (a == 255) ? c : mix(out[px], c, a);
        }
    }
    damage(x, y, sw, im->h);
}

void image_draw(const struct image *im, int x, int y)
{
    if (im) image_draw_part(im, 0, im->w, x, y);
}

/* the same, pulled towards a colour: how an icon lights up when picked */
void image_draw_tinted(const struct image *im, int x, int y, uint32_t tint,
                       int amount)
{
    int row, col;
    if (!im || !im->bgra) return;
    for (row = 0; row < im->h; row++) {
        int py = y + row;
        const unsigned char *p;
        uint32_t *out;
        if (py < cy0 || py >= cy1) continue;
        p = im->bgra + (size_t)row * im->w * 4;
        out = back + (size_t)py * scr_w;
        for (col = 0; col < im->w; col++, p += 4) {
            int px = x + col;
            int a = p[3];
            uint32_t c;
            if (px < cx0 || px >= cx1 || a == 0) continue;
            c = ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
            c = mix(c, tint, amount);
            out[px] = (a == 255) ? c : mix(out[px], c, a);
        }
    }
    damage(x, y, im->w, im->h);
}

/* ---------------------------------------------------------------- text */
static const struct glyph *glyph_of(const struct font *f, int ch)
{
    if (ch < f->first || ch > f->first + 94) return 0;
    return &f->glyphs[ch - f->first];
}

int text_height(int face)
{
    return faces[face]->height;
}

int text_width(int face, const char *s)
{
    const struct font *f = faces[face];
    int w = 0;
    for (; *s; s++) {
        const struct glyph *g = glyph_of(f, (uint8_t)*s);
        if (g) w += g->advance;
    }
    return w;
}

void text(int face, int x, int y, const char *s, uint32_t c)
{
    const struct font *f = faces[face];
    for (; *s; s++) {
        const struct glyph *g = glyph_of(f, (uint8_t)*s);
        int row, col;
        if (!g) continue;
        if (!clip_intersects(x + g->bx, y + g->by, g->w, g->h)) {
            x += g->advance;                    /* nothing of it shows */
            continue;
        }
        for (row = 0; row < g->h; row++) {
            const uint8_t *cov = f->pixels + g->offset + row * g->w;
            int py = y + g->by + row;
            for (col = 0; col < g->w; col++)
                pixel_blend(x + g->bx + col, py, c, cov[col]);
        }
        if (g->w && g->h)
            damage(x + g->bx, y + g->by, g->w, g->h);
        x += g->advance;
    }
}

void text_clipped(int face, int x, int y, int max_w, const char *s, uint32_t c)
{
    int ox, oy, ow, oh;
    clip_get(&ox, &oy, &ow, &oh);
    clip_set(x, oy, max_w, oh);
    if (ox > x) clip_set(ox, oy, x + max_w - ox, oh);
    text(face, x, y, s, c);
    clip_set(ox, oy, ow, oh);
}
