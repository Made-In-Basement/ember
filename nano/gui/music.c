/* music.c - the music player window.
 *
 * A recessed display with the track and a spectrum analyser across it, a
 * transport, a seek bar, a volume slider, and the playlist underneath.
 *
 * The analyser is a 256-point transform of the samples on their way to the
 * sound chip, done in fixed point against tables worked out at build time,
 * so there is no trigonometry and no floating point anywhere in it.  Bars
 * are spaced by ear rather than evenly, and each carries a cap that falls
 * back slowly, which is what makes the thing readable at a glance.
 */
#include <nanolibc.h>
#include "nano.h"
#include "draw.h"
#include "input.h"
#include "shell.h"
#include "paudio.h"
#include "guitables.h"

#define PANEL       0x1A140D
#define WELL        0x090705          /* the sunken display */
#define WELL_EDGE   0x33271A
#define AMBER       0xF0A020
#define AMBER_DIM   0x8A5E16
#define AMBER_HOT   0xFFC65A
#define AMBER_PALE  0xFFE0A8
#define TEXT        0xD8C8B0
#define TEXT_DIM    0x8A7C68
#define TROUGH      0x120D08

#define MAX_TRACKS  256
#define NAME_MAX    56
#define ROW_H       22

static char names[MAX_TRACKS][NAME_MAX];
static uint32_t sizes[MAX_TRACKS];
static char dir_path[80];
static int track_count, playing = -1, paused, sel, top;
static int have_stream;
static int win_id = -1;

/* analyser state */
static int bars[FFT_BARS], caps[FFT_BARS];
static unsigned last_fft;

/* where the pieces sit inside the window */
#define DISP_X   12
#define DISP_Y   12
#define DISP_H   96
#define SEEK_Y   (DISP_Y + DISP_H + 12)
#define SEEK_H   12
#define CTRL_Y   (SEEK_Y + SEEK_H + 12)
#define CTRL_H   34
#define LIST_Y   (CTRL_Y + CTRL_H + 12)

/* ---------------------------------------------------------------- playlist */
static int playable(const char *name)
{
    const char *d = strrchr(name, '.');
    return d && (!strcasecmp(d, ".MP3") || !strcasecmp(d, ".WAV"));
}

static void scan(const char *where)
{
    struct dos_find f;
    char pattern[128];
    int rc;
    track_count = 0;
    strncpy(dir_path, where, sizeof dir_path - 1);
    dir_path[sizeof dir_path - 1] = 0;
    strcpy(pattern, dir_path);
    if (pattern[0] && pattern[strlen(pattern) - 1] != '\\')
        strcat(pattern, "\\");
    strcat(pattern, "*.*");
    for (rc = sys_findfirst(pattern, &f); rc == 0 && track_count < MAX_TRACKS;
         rc = sys_findnext(&f)) {
        char longname[84];
        const char *use = f.name;
        if (f.attr & 0x18) continue;                /* folders, labels */
        if (sys_long_name(longname, sizeof longname) > 0)
            use = longname;
        if (!playable(use)) continue;
        strncpy(names[track_count], use, NAME_MAX - 1);
        names[track_count][NAME_MAX - 1] = 0;
        sizes[track_count] = f.size;
        track_count++;
    }
}

static void full_path(int i, char *out)
{
    out[0] = 0;
    if (dir_path[0]) {
        strcpy(out, dir_path);
        if (out[strlen(out) - 1] != '\\') strcat(out, "\\");
    }
    strcat(out, names[i]);
}

static void play_track(int i)
{
    char path[160];
    if (i < 0 || i >= track_count) return;
    if (!have_stream) {
        if (audio_start() != 0) return;
        have_stream = 1;
    }
    full_path(i, path);
    audio_silence();
    if (audio_open(path) != 0) { playing = -1; return; }
    playing = i;
    paused = 0;
}

static void stop_playing(void)
{
    audio_close();
    audio_silence();
    playing = -1;
    paused = 0;
}

/* ---------------------------------------------------------------- analyser */
/* An in-place transform of the last FFT_N samples.  Everything is Q15:
   the values never leave 32-bit arithmetic. */
static void spectrum(void)
{
    static int re[FFT_N], im[FFT_N];
    int i, size, half, step, j, k;
    int pos = audio_scope_pos;

    for (i = 0; i < FFT_N; i++) {
        int s = audio_scope[(pos + i) & (SCOPE_LEN - 1)];
        /* a raised cosine over the whole window: 0.5 - 0.5*cos(2*pi*i/N),
           which is what stops one note from smearing across its neighbours */
        int w = 16384 - (sin_q15[(i + FFT_N / 4) & (FFT_N - 1)] / 2);
        re[bit_rev[i]] = (s * w) >> 15;
        im[bit_rev[i]] = 0;
    }
    for (size = 2; size <= FFT_N; size <<= 1) {
        half = size / 2;
        step = FFT_N / size;
        for (i = 0; i < FFT_N; i += size) {
            for (j = i, k = 0; j < i + half; j++, k += step) {
                int wr = sin_q15[(k + FFT_N / 4) % FFT_N];      /* cos */
                int wi = -sin_q15[k % FFT_N];
                int tr = (re[j + half] * wr - im[j + half] * wi) >> 15;
                int ti = (re[j + half] * wi + im[j + half] * wr) >> 15;
                re[j + half] = re[j] - tr;
                im[j + half] = im[j] - ti;
                re[j] += tr;
                im[j] += ti;
            }
        }
    }
    for (i = 0; i < FFT_BARS; i++) {
        int lo = bar_edge[i], hi = bar_edge[i + 1], best = 0, b;
        for (b = lo; b < hi && b < FFT_N / 2; b++) {
            int mag = (re[b] < 0 ? -re[b] : re[b]) + (im[b] < 0 ? -im[b] : im[b]);
            if (mag > best) best = mag;
        }
        /* squeeze the range so quiet detail is still visible */
        best >>= 7;
        if (best > 255) best = 255;
        best = best * best / 255;                   /* and keep the floor dark */
        if (best > bars[i]) bars[i] = best;
        else bars[i] -= (bars[i] - best) / 3 + 1;
        if (bars[i] < 0) bars[i] = 0;
        if (bars[i] >= caps[i]) caps[i] = bars[i];
        else if (caps[i] > 0) caps[i] -= 2;
    }
}

static void draw_analyser(struct window *w, int x, int y, int width, int height)
{
    int i, bw = width / FFT_BARS;
    for (i = 0; i < FFT_BARS; i++) {
        int bx = x + i * bw;
        int h = bars[i] * height / 255;
        int cap = caps[i] * height / 255;
        int seg;
        for (seg = 0; seg < h; seg += 3) {
            uint32_t c = seg * 255 / height > 170 ? AMBER_PALE
                       : (seg * 255 / height > 90 ? AMBER_HOT : AMBER);
            fill(bx, y + height - seg - 2, bw - 2, 2, c);
        }
        if (cap > 2)
            fill(bx, y + height - cap - 2, bw - 2, 1, AMBER_PALE);
    }
    (void)w;
}

/* ---------------------------------------------------------------- drawing */
static void draw_transport(struct window *w, int x, int y)
{
    /* the buttons, drawn as shapes: previous, play or pause, stop, next */
    int i;
    static const int order[4] = { 0, 1, 2, 3 };
    for (i = 0; i < 4; i++) {
        int bx = x + i * 42, by = y;
        int cx = bx + 16, cy = by + CTRL_H / 2;
        int p[8];
        round_fill(bx, by, 34, CTRL_H, 4, 0x241C12);
        round_frame(bx, by, 34, CTRL_H, 4, WELL_EDGE);
        switch (order[i]) {
        case 0:                                     /* previous */
            p[0] = cx + 5; p[1] = cy - 8;
            p[2] = cx + 5; p[3] = cy + 8;
            p[4] = cx - 4; p[5] = cy;
            poly_fill(p, 3, AMBER);
            fill(cx - 7, cy - 8, 2, 16, AMBER);
            break;
        case 1:
            if (playing >= 0 && !paused) {          /* pause */
                fill(cx - 6, cy - 8, 4, 16, AMBER_HOT);
                fill(cx + 1, cy - 8, 4, 16, AMBER_HOT);
            } else {                                /* play */
                p[0] = cx - 5; p[1] = cy - 8;
                p[2] = cx - 5; p[3] = cy + 8;
                p[4] = cx + 6; p[5] = cy;
                poly_fill(p, 3, AMBER_HOT);
            }
            break;
        case 2:                                     /* stop */
            fill(cx - 6, cy - 6, 13, 13, AMBER);
            break;
        default:                                    /* next */
            p[0] = cx - 5; p[1] = cy - 8;
            p[2] = cx - 5; p[3] = cy + 8;
            p[4] = cx + 4; p[5] = cy;
            poly_fill(p, 3, AMBER);
            fill(cx + 5, cy - 8, 2, 16, AMBER);
            break;
        }
    }
    (void)w;
}

static void draw_slider(int x, int y, int width, int value, int max, int lit)
{
    int pos = max > 0 ? width * value / max : 0;
    fill(x, y + 4, width, 4, TROUGH);
    fill(x, y + 4, width, 1, 0x241C12);
    if (pos > 0) fill(x, y + 4, pos, 4, lit ? AMBER_HOT : AMBER_DIM);
    round_fill(x + pos - 3, y, 7, 12, 3, lit ? AMBER_PALE : AMBER);
}

static void music_draw(struct window *w)
{
    int x = w->x, y = w->y, ww = w->w;
    char buf[96];
    int i, rows;

    /* ---- the display, sunk into the panel ---- */
    round_fill(x + DISP_X, y + DISP_Y, ww - DISP_X * 2, DISP_H, 3, WELL);
    round_frame(x + DISP_X, y + DISP_Y, ww - DISP_X * 2, DISP_H, 3, WELL_EDGE);
    fill(x + DISP_X + 1, y + DISP_Y + 1, ww - DISP_X * 2 - 2, 1, 0x000000);

    /* the time, big, on the left */
    if (playing >= 0)
        snprintf(buf, sizeof buf, "%ld:%02ld", audio_seconds / 60, audio_seconds % 60);
    else
        strcpy(buf, "--:--");
    text(F_CLOCK, x + DISP_X + 14, y + DISP_Y + 10, buf,
         playing >= 0 ? AMBER_HOT : AMBER_DIM);

    if (playing >= 0) {
        snprintf(buf, sizeof buf, "%d kbps  %s", audio_bitrate,
                 audio_channels > 1 ? "stereo" : "mono");
        text(F_SMALL, x + DISP_X + 16, y + DISP_Y + 56, buf, TEXT_DIM);
        snprintf(buf, sizeof buf, "%d Hz", audio_rate);
        text(F_SMALL, x + DISP_X + 16, y + DISP_Y + 72, buf, TEXT_DIM);
    }

    /* the analyser fills the right of the display */
    {
        int ax = x + DISP_X + 150;
        int aw = ww - DISP_X * 2 - 162;
        draw_analyser(w, ax, y + DISP_Y + 8, aw, DISP_H - 30);
        text_clipped(F_SMALL, ax, y + DISP_Y + DISP_H - 20, aw,
                     playing >= 0 ? names[playing] : "nothing playing",
                     playing >= 0 ? AMBER : TEXT_DIM);
    }

    /* ---- the seek bar ---- */
    {
        int sx = x + DISP_X, sw = ww - DISP_X * 2;
        int pos = (audio_total_seconds > 0 && playing >= 0)
                ? (int)(audio_seconds * 1000 / audio_total_seconds) : 0;
        fill(sx, y + SEEK_Y + 3, sw, 6, TROUGH);
        fill(sx, y + SEEK_Y + 3, sw, 1, 0x241C12);
        if (pos > 0) hgradient(sx, y + SEEK_Y + 3, sw * pos / 1000, 6,
                               AMBER_DIM, AMBER_HOT);
        if (playing >= 0) {
            snprintf(buf, sizeof buf, "%ld:%02ld",
                     audio_total_seconds / 60, audio_total_seconds % 60);
            text(F_SMALL, x + ww - DISP_X - text_width(F_SMALL, buf),
                 y + SEEK_Y + 12, buf, TEXT_DIM);
        }
    }

    /* ---- transport and volume ---- */
    draw_transport(w, x + DISP_X, y + CTRL_Y);
    {
        int vx = x + ww - DISP_X - 130;
        text(F_SMALL, vx - 34, y + CTRL_Y + 9, "VOL", TEXT_DIM);
        draw_slider(vx, y + CTRL_Y + 11, 130, audio_volume, 100, 1);
    }

    /* ---- the playlist ---- */
    rows = (w->h - LIST_Y - 8) / ROW_H;
    fill(x + DISP_X, y + LIST_Y - 6, ww - DISP_X * 2, 1, WELL_EDGE);
    if (sel < top) top = sel;
    if (sel >= top + rows) top = sel - rows + 1;
    for (i = 0; i < rows; i++) {
        int idx = top + i, ry = y + LIST_Y + i * ROW_H;
        uint32_t c = TEXT;
        if (idx >= track_count) break;
        if (idx == sel) {
            fill(x + DISP_X, ry - 2, ww - DISP_X * 2, ROW_H, 0x2A1D0C);
            c = AMBER_HOT;
        }
        if (idx == playing) {
            fill(x + DISP_X, ry - 2, 3, ROW_H, AMBER);
            if (idx != sel) c = AMBER;
        }
        snprintf(buf, sizeof buf, "%d.", idx + 1);
        text(F_SMALL, x + DISP_X + 10, ry, buf, TEXT_DIM);
        text_clipped(F_NORMAL, x + DISP_X + 40, ry - 2, ww - 150, names[idx], c);
        snprintf(buf, sizeof buf, "%u KB", (unsigned)(sizes[idx] / 1024));
        text(F_SMALL, x + ww - DISP_X - 12 - text_width(F_SMALL, buf), ry, buf,
             TEXT_DIM);
    }
    if (track_count == 0)
        text(F_NORMAL, x + DISP_X + 12, y + LIST_Y + 6,
             "No .MP3 or .WAV files in \\MUSIC", TEXT_DIM);
}

/* ---------------------------------------------------------------- events */
static int music_event(struct window *w, struct event *e)
{
    int rows = (w->h - LIST_Y - 8) / ROW_H;
    if (e->type == EV_MOUSE_DOWN) {
        int x = e->a, y = e->b;
        /* the transport */
        if (y >= CTRL_Y && y < CTRL_Y + CTRL_H && x >= DISP_X && x < DISP_X + 4 * 42) {
            int b = (x - DISP_X) / 42;
            if (b == 0) { if (playing > 0) play_track(playing - 1); }
            else if (b == 1) {
                if (playing < 0) play_track(sel >= 0 ? sel : 0);
                else { paused = !paused; if (paused) audio_silence(); }
            } else if (b == 2) stop_playing();
            else if (playing + 1 < track_count) play_track(playing + 1);
            return 1;
        }
        /* the volume */
        if (y >= CTRL_Y && y < CTRL_Y + CTRL_H && x >= w->w - DISP_X - 130) {
            int v = (x - (w->w - DISP_X - 130)) * 100 / 130;
            audio_volume = v < 0 ? 0 : (v > 100 ? 100 : v);
            return 1;
        }
        /* the seek bar */
        if (y >= SEEK_Y - 4 && y < SEEK_Y + SEEK_H + 4 && playing >= 0) {
            int p = (x - DISP_X) * 1000 / (w->w - DISP_X * 2);
            audio_seek_permille(p);
            return 1;
        }
        /* the playlist */
        if (y >= LIST_Y) {
            int idx = top + (y - LIST_Y) / ROW_H;
            if (idx < track_count) {
                if (idx == sel) play_track(idx);
                else sel = idx;
            }
            return 1;
        }
    }
    if (e->type == EV_KEY) {
        if (e->a == K_UP && sel > 0) sel--;
        else if (e->a == K_DOWN && sel + 1 < track_count) sel++;
        else if (e->a == K_ENTER) play_track(sel);
        else if (e->b == ' ') {
            if (playing < 0) play_track(sel);
            else { paused = !paused; if (paused) audio_silence(); }
        } else if (e->b == '+' || e->b == '=') {
            audio_volume += 5;
            if (audio_volume > 100) audio_volume = 100;
        } else if (e->b == '-') {
            audio_volume -= 5;
            if (audio_volume < 0) audio_volume = 0;
        } else return 0;
        return 1;
    }
    (void)rows;
    return 0;
}

/* Called every frame by the shell: keep the sound fed and the bars moving.
   Returns 1 when something changed that is worth redrawing. */
int music_tick(void)
{
    int changed = 0;
    if (playing == -2) {                        /* the start-up chime */
        if (!audio_pump()) {
            audio_close();
            playing = -1;
            if (win_id < 0 && have_stream) { audio_stop(); have_stream = 0; }
        }
        return 0;
    }
    if (win_id < 0) return 0;
    if (playing >= 0 && !paused) {
        if (!audio_pump()) {                        /* the track ended */
            if (playing + 1 < track_count) play_track(playing + 1);
            else stop_playing();
        }
        if (now_ms() - last_fft > 33) {             /* about thirty a second */
            last_fft = now_ms();
            spectrum();
            changed = 1;
        }
    } else if (now_ms() - last_fft > 33) {         /* the bars settle at the same pace */
        int i, any = 0;
        last_fft = now_ms();
        for (i = 0; i < FFT_BARS; i++) {
            if (bars[i] > 0) { bars[i] -= 4; if (bars[i] < 0) bars[i] = 0; any = 1; }
            if (caps[i] > 0) { caps[i] -= 3; if (caps[i] < 0) caps[i] = 0; any = 1; }
        }
        changed = any;
    }
    return changed;
}

/* the display alone: what a tick of the analyser changes */
void music_display_rect(int *x, int *y, int *w, int *h)
{
    struct window *win = win_at(win_id);
    *x = win->x + DISP_X;
    *y = win->y + DISP_Y;
    *w = win->w - DISP_X * 2;
    *h = DISP_H;
}

int music_window(void) { return win_id; }

/* true while there is sound to keep feeding: the loop must not sleep then */
int music_active(void)
{
    return playing != -1 && !paused;
}

void music_closed(int id)
{
    if (id == win_id) {
        stop_playing();
        if (have_stream) { audio_stop(); have_stream = 0; }
        win_id = -1;
    }
}

/* the sound the desktop makes when it opens */
void music_chime(void)
{
    if (audio_start() != 0)
        return;
    have_stream = 1;
    if (audio_open("\\EMBER.WAV") != 0)
        return;
    playing = -2;                               /* playing, but not a track */
    paused = 0;
}

void app_music(void)
{
    if (win_id >= 0) return;                        /* only one of these */
    scan("\\MUSIC");
    if (track_count == 0) scan("");
    win_id = win_open("Music", 560, 470, music_draw, music_event);
    sel = 0;
    top = 0;
}
