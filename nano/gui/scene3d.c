/* scene3d.c - Solid: a small three-dimensional scene, drawn in software.
 *
 * There is no 3-D engine on the chip yet, so every triangle here is turned,
 * lit and filled by the processor: a rotation of each corner, a normal for
 * each face to cull the ones that face away and to shade the rest by how
 * squarely they meet the light, the faces sorted far to near and painted in
 * that order.  What makes it smooth is the display driver underneath: the
 * frame is drawn into memory the screen reads directly, so a spinning solid
 * at this size costs a flush, not a copy.
 *
 * A window, a floor drawn in perspective with a soft shadow under the shape,
 * and a toolbar to change the solid, turn wireframe on, and spin faster or
 * slower.  The corner shows how many frames a second the processor manages.
 */
#include <nanolibc.h>
#include "nano.h"
#include "draw.h"
#include "input.h"
#include "shell.h"
#include "gpu3d.h"

#define PANEL       0x140F0A
#define PANEL_LIT   0x2A2114
#define EDGE        0x3A2C18
#define AMBER       0xF0A020
#define AMBER_HOT   0xFFC65A
#define TEXT        0xD8C8B0
#define TEXT_DIM    0x8A7C68
#define TOOL_H      44

#define PI 3.14159265f

static float fsin(float x)
{
    float x2;
    while (x > PI) x -= 2 * PI;                  /* to [-pi, pi] */
    while (x < -PI) x += 2 * PI;
    x2 = x * x;                                  /* a Taylor series, five terms */
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72))));
}
static float fcos(float x) { return fsin(x + PI / 2); }
static float fsqrt(float x) { float r; __asm__("fsqrt" : "=t"(r) : "0"(x)); return r; }

/* ------------------------------------------------------------ the solids */
struct solid {
    const char *name;
    int nv, nf;
    const float *v;                             /* nv * 3 */
    const int *f;                               /* nf * 5: count, then up to 4 corners */
};

static const float cube_v[] = {
    -1,-1,-1,  1,-1,-1,  1,1,-1,  -1,1,-1,  -1,-1,1,  1,-1,1,  1,1,1,  -1,1,1
};
static const int cube_f[] = {
    4,0,1,2,3,  4,5,4,7,6,  4,4,0,3,7,  4,1,5,6,2,  4,4,5,1,0,  4,3,2,6,7
};

static const float oct_v[] = {
    1,0,0,  -1,0,0,  0,1,0,  0,-1,0,  0,0,1,  0,0,-1
};
static const int oct_f[] = {
    3,0,2,4,0,  3,2,1,4,0,  3,1,3,4,0,  3,3,0,4,0,
    3,2,0,5,0,  3,1,2,5,0,  3,3,1,5,0,  3,0,3,5,0
};

/* an icosahedron from the golden ratio (t), scaled to about unit radius */
#define T 1.618034f
static const float ico_v[] = {
    -1,T,0,  1,T,0,  -1,-T,0,  1,-T,0,   0,-1,T,  0,1,T,  0,-1,-T,  0,1,-T,
     T,0,-1,  T,0,1,  -T,0,-1,  -T,0,1
};
static const int ico_f[] = {
    3,0,11,5,0, 3,0,5,1,0, 3,0,1,7,0, 3,0,7,10,0, 3,0,10,11,0,
    3,1,5,9,0,  3,5,11,4,0, 3,11,10,2,0, 3,10,7,6,0, 3,7,1,8,0,
    3,3,9,4,0,  3,3,4,2,0,  3,3,2,6,0,  3,3,6,8,0,  3,3,8,9,0,
    3,4,9,5,0,  3,2,4,11,0, 3,6,2,10,0, 3,8,6,7,0,  3,9,8,1,0
};

static const struct solid solids[] = {
    { "Cube",         8,  6,  cube_v, cube_f },
    { "Octahedron",   6,  8,  oct_v,  oct_f },
    { "Icosahedron", 12, 20, ico_v,  ico_f },
};
#define NSOLIDS 3

/* ------------------------------------------------------------ state */
static int win_id = -1;
static int shape, wireframe, speed = 3, use_gpu;
static uint32_t shade(uint32_t base, int lit);

/* The engine's one texture: four squares of an amber checker at four
   brightnesses, so a face can be lit by choosing its square; and one
   dark texel in the corner that the background is cleared to. */
static void gpu_texture(void)
{
    static uint32_t px[64 * 64];
    static const int bright[4] = { 90, 140, 200, 256 };
    int x, y;
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++) {
            int q = (y / 32) * 2 + x / 32, check = (((x % 32) / 8) + ((y % 32) / 8)) & 1;
            uint32_t base = check ? 0xF0A020 : 0xB0741A;
            px[y * 64 + x] = 0xFF000000u | shade(base, bright[q]);
        }
    px[63 * 64 + 63] = 0xFF120D08u;             /* the background */
    gpu3d_texture(px);
}
static float ang_x, ang_y;
static unsigned last_ms, frame_ms, shown_fps;
static int frames_since, fps;

/* rotated then camera-shifted corners, and their projection */
static float vx[12], vy[12], vz[12];
static int sx[12], sy[12];

static uint32_t shade(uint32_t base, int lit)   /* lit 0..256 */
{
    int r = (int)((base >> 16) & 0xFF) * lit >> 8;
    int g = (int)((base >> 8) & 0xFF) * lit >> 8;
    int b = (int)(base & 0xFF) * lit >> 8;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* ------------------------------------------------------------ drawing */
static void scene3d_draw(struct window *w)
{
    const struct solid *s = &solids[shape];
    int ax = w->x, ay = w->y, aw = w->w, ah = w->h - TOOL_H;
    int cx = ax + aw / 2, cy = ay + TOOL_H + ah / 2, i, f, order[24];
    float depth[24], sinx = fsin(ang_x), cosx = fcos(ang_x), siny = fsin(ang_y), cosy = fcos(ang_y);
    float scale = (aw < ah ? aw : ah) * 0.30f, focal = 3.2f, camz = 4.2f;
    char buf[96];

    /* ---- the toolbar ---- */
    vgradient(ax, ay, aw, TOOL_H, PANEL_LIT, PANEL);
    fill(ax, ay + TOOL_H - 1, aw, 1, EDGE);
    {
        static const char *labels[] = { "Shape", "Wire", "Slower", "Faster", "GPU" };
        int bx = ax + 10, k;
        for (k = 0; k < 5; k++) {
            int bw = k == 0 ? 130 : 92, lit = (k == 1 && wireframe) || (k == 4 && use_gpu && gpu3d_active());
            round_fill(bx, ay + 6, bw, 32, 4, lit ? 0x4A3618 : 0x241B10);
            round_frame(bx, ay + 6, bw, 32, 4, lit ? AMBER : EDGE);
            if (k == 0) {
                snprintf(buf, sizeof buf, "%s", s->name);
                text(F_SMALL, bx + 12, ay + 6 + (32 - text_height(F_SMALL)) / 2, buf, AMBER_HOT);
            } else {
                text(F_SMALL, bx + (bw - text_width(F_SMALL, labels[k])) / 2,
                     ay + 6 + (32 - text_height(F_SMALL)) / 2, labels[k], lit ? AMBER_HOT : TEXT);
            }
            bx += bw + 8;
        }
    }

    /* ---- the floor: lines to a vanishing point, and a graded ground ---- */
    vgradient(ax, ay + TOOL_H, aw, ah, 0x0E0B08, 0x1A130C);
    {
        int horizon = cy + (int)(scale * 0.9f), fx, fy = ay + ah + ay;   /* fy unused past clamp */
        (void)fy;
        for (i = -6; i <= 6; i++) {                 /* receding cross lines */
            int y = horizon + (int)(scale * 1.6f) * (i + 7) / 14;
            if (y > ay + TOOL_H && y < ay + w->h) fill(ax, y, aw, 1, 0x181109);
        }
        for (i = -6; i <= 6; i++) {                 /* lines to the vanishing point */
            fx = cx + i * (aw / 10);
            line(fx, ay + w->h, cx, horizon, 1, 0x1E1509);
        }
    }

    /* ---- turn every corner, and shift it back from the camera ---- */
    for (i = 0; i < s->nv; i++) {
        float x = s->v[i * 3], y = s->v[i * 3 + 1], z = s->v[i * 3 + 2];
        float x1 = x * cosy + z * siny, z1 = -x * siny + z * cosy;   /* about Y */
        float y2 = y * cosx - z1 * sinx, z2 = y * sinx + z1 * cosx;  /* about X */
        vx[i] = x1; vy[i] = y2; vz[i] = z2 + camz;
        sx[i] = cx + (int)(focal * scale * x1 / vz[i]);
        sy[i] = cy - (int)(focal * scale * y2 / vz[i]);
    }

    /* ---- on the engine: the faces as triangles, lit by their square ---- */
    if (use_gpu && gpu3d_active()) {
        static float verts[256 * 6];
        int n = 0;
        float near = camz - 1.8f, far = camz + 1.8f;
        for (f = 0; f < s->nf && n + 6 <= 256; f++) {
            const int *fc = &s->f[f * 5];
            int a = fc[1], b = fc[2], c = fc[3], k, q;
            float ux = vx[b] - vx[a], uy = vy[b] - vy[a], uz = vz[b] - vz[a];
            float wx = vx[c] - vx[a], wy = vy[c] - vy[a], wz = vz[c] - vz[a];
            float nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
            float len = fsqrt(nx * nx + ny * ny + nz * nz), lit, qx, qy;
            if (len < 0.0001f) len = 1;
            lit = (nx * -0.4f + ny * 0.5f + nz * -0.77f) / len;
            if (lit < 0) lit = -lit;
            q = (int)(lit * 3.99f);
            qx = (q & 1) ? 0.52f : 0.02f;
            qy = (q & 2) ? 0.52f : 0.02f;
            for (k = 1; k + 1 < fc[0]; k++) {       /* a fan: 1 or 2 triangles */
                int tri[3] = { fc[1], fc[1 + k], fc[2 + k] };
                static const float fu[4] = { 0, 0.45f, 0.45f, 0 }, fv[4] = { 0, 0, 0.45f, 0.45f };
                int corner[3] = { 0, k, k + 1 };
                int m;
                for (m = 0; m < 3; m++) {
                    int v = tri[m];
                    float z = vz[v], d = (1.0f / near - 1.0f / z) / (1.0f / near - 1.0f / far);
                    float *o = verts + n * 6;
                    o[0] = (float)sx[v] * z; o[1] = (float)sy[v] * z; o[2] = d * z; o[3] = z;
                    o[4] = qx + fu[corner[m]]; o[5] = qy + fv[corner[m]];
                    n++;
                }
            }
        }
        if (gpu3d_queue(ax, ay + TOOL_H, aw, ah - 30, verts, n, 63.5f / 64, 63.5f / 64) != 0)
            gpu3d_note_refused(ax, ay + TOOL_H, aw, ah - 30, n);
        snprintf(buf, sizeof buf, "%s, %d faces   %u fps   engine %u us at %u MHz", s->name, s->nf, shown_fps,
                 gpu3d_last_us(), gpu3d_mhz());
        text(F_SMALL, ax + 12, ay + w->h - text_height(F_SMALL) - 8, buf, TEXT_DIM);
        return;
    }

    /* ---- a soft shadow under the shape, on the floor ---- */
    {
        int scy = cy + (int)(scale * 0.95f);
        soft_ellipse(cx, scy, (int)(scale * 0.9f), (int)(scale * 0.28f), 0x000000, 120);
    }

    /* ---- order the faces far to near ---- */
    for (f = 0; f < s->nf; f++) {
        const int *fc = &s->f[f * 5];
        float z = 0;
        int k;
        for (k = 0; k < fc[0]; k++) z += vz[fc[1 + k]];
        depth[f] = z / fc[0];
        order[f] = f;
    }
    for (i = 1; i < s->nf; i++) {                   /* an insertion sort, biggest z first */
        int key = order[i], j = i - 1;
        while (j >= 0 && depth[order[j]] < depth[key]) { order[j + 1] = order[j]; j--; }
        order[j + 1] = key;
    }

    /* ---- draw them ---- */
    for (i = 0; i < s->nf; i++) {
        const int *fc = &s->f[order[i] * 5];
        int n = fc[0], a = fc[1], b = fc[2], c = fc[3], pts[8], k;
        /* the normal, from two edges in view space */
        float ux = vx[b] - vx[a], uy = vy[b] - vy[a], uz = vz[b] - vz[a];
        float wx = vx[c] - vx[a], wy = vy[c] - vy[a], wz = vz[c] - vz[a];
        float nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
        float cxv = 0, cyv = 0, czv = 0, len, facing, lit;
        for (k = 0; k < n; k++) { cxv += vx[fc[1 + k]]; cyv += vy[fc[1 + k]]; czv += vz[fc[1 + k]]; }
        cxv /= n; cyv /= n; czv /= n;
        facing = nx * cxv + ny * cyv + nz * czv;    /* face away from the camera? */
        if (facing > 0 && !wireframe) continue;
        len = fsqrt(nx * nx + ny * ny + nz * nz);
        if (len < 0.0001f) len = 1;
        /* light from over the left shoulder */
        lit = (nx * -0.4f + ny * 0.5f + nz * -0.77f) / len;
        if (lit < 0) lit = -lit;                    /* both sides catch some light */
        {
            int l = 60 + (int)(lit * 196);
            uint32_t base = 0xF0A020;
            if (wireframe) {
                for (k = 0; k < n; k++)
                    line(sx[fc[1 + k]], sy[fc[1 + k]], sx[fc[1 + (k + 1) % n]], sy[fc[1 + (k + 1) % n]], 2,
                         facing > 0 ? 0x4A3618 : AMBER);
            } else {
                for (k = 0; k < n; k++) { pts[k * 2] = sx[fc[1 + k]]; pts[k * 2 + 1] = sy[fc[1 + k]]; }
                poly_fill(pts, n, shade(base, l));
                /* a bright edge catches the light */
                for (k = 0; k < n; k++)
                    line(sx[fc[1 + k]], sy[fc[1 + k]], sx[fc[1 + (k + 1) % n]], sy[fc[1 + (k + 1) % n]], 1, shade(AMBER_HOT, l));
            }
        }
    }

    /* ---- the reading ---- */
    snprintf(buf, sizeof buf, "%s, %d faces   %u fps   software", s->name, s->nf, shown_fps);
    text(F_SMALL, ax + 12, ay + w->h - text_height(F_SMALL) - 8, buf, TEXT_DIM);
}

/* ------------------------------------------------------------ events */
static int scene3d_event(struct window *w, struct event *e)
{
    if (e->type == EV_MOUSE_DOWN && e->b < TOOL_H) {
        int bx = 10, k, bw;
        for (k = 0; k < 5; k++) {
            bw = k == 0 ? 130 : 92;
            if (e->a >= bx && e->a < bx + bw) {
                if (k == 0) shape = (shape + 1) % NSOLIDS;
                else if (k == 1) wireframe = !wireframe;
                else if (k == 2 && speed > 1) speed--;
                else if (k == 3 && speed < 8) speed++;
                else if (k == 4) {
                    use_gpu = !use_gpu;
                    if (use_gpu && !gpu3d_active() && gpu3d_open() == 0) gpu_texture();
                    if (!gpu3d_active()) use_gpu = 0;
                }
                return 1;
            }
            bx += bw + 8;
        }
        return 1;
    }
    if (e->type == EV_KEY) {
        int ch = e->b;                              /* the character; e->a is the scan code */
        if (ch == ' ') { wireframe = !wireframe; return 1; }
        if (ch == 's' || ch == 'S') { shape = (shape + 1) % NSOLIDS; return 1; }
        if (ch == 'g' || ch == 'G') {
            use_gpu = !use_gpu;
            if (use_gpu && !gpu3d_active() && gpu3d_open() == 0) gpu_texture();
            if (!gpu3d_active()) use_gpu = 0;
            return 1;
        }
        if (e->a == K_LEFT && speed > 1) { speed--; return 1; }
        if (e->a == K_RIGHT && speed < 8) { speed++; return 1; }
    }
    return 0;
}

/* the animation: turn a little, and ask for a repaint about thirty a second */
int scene3d_tick(void)
{
    unsigned now = now_ms(), dt;
    if (win_id < 0) return 0;
    if (!last_ms) last_ms = now;
    dt = now - last_ms;
    if (dt < 28) return 0;
    ang_y += 0.0009f * speed * dt;                  /* steady, whatever the frame rate */
    ang_x += 0.0004f * speed * dt;
    frames_since++;
    frame_ms += dt;
    if (frame_ms >= 500) { shown_fps = frames_since * 1000 / frame_ms; frames_since = 0; frame_ms = 0; }
    last_ms = now;
    return 1;
}

int  scene3d_window(void) { return win_id; }
void scene3d_closed(int id) { if (id == win_id) { win_id = -1; use_gpu = 0; gpu3d_close(); } }

void app_scene3d(void)
{
    if (win_id >= 0) return;
    shape = 0; wireframe = 0; speed = 3;
    ang_x = 0.4f; ang_y = 0;
    last_ms = 0; frames_since = 0; frame_ms = 0; shown_fps = 0;
    win_id = win_open("Solid", 720, 560, scene3d_draw, scene3d_event);
}
