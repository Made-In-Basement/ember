/* player.c - PLAYER.N32, a music player for Ember.
 *
 * Give it a file, a directory, or nothing at all (it then looks in the
 * current one), and it lists what it can play and gets on with it.
 *
 *   Up / Down   choose a track        Enter  play it
 *   Space       pause                 N / P  next, previous
 *   + / -       volume                Esc    leave
 */
#include <nanolibc.h>
#include "nano.h"
#include "pgfx.h"
#include "paudio.h"

#define MAX_TRACKS 256
#define NAME_MAX   14

static char dir_path[96];
static char names[MAX_TRACKS][NAME_MAX];
static uint32_t sizes[MAX_TRACKS];
static int track_count, selected, playing = -1, paused, top_row;

/* the interface, laid out once and then only touched where it changes */
static int list_x, list_y, list_w, list_h, rows;
static int info_x, info_y, info_w;
static int bar_x, bar_y, bar_w;
static int vu_x, vu_y, vu_w, vu_h;

static uint64_t rdtsc64(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* ---------------------------------------------------------------- playlist */
static int playable(const char *name)
{
    const char *d = strrchr(name, '.');
    if (!d) return 0;
    return !strcasecmp(d, ".MP3") || !strcasecmp(d, ".WAV");
}

static void add_pattern(const char *pattern)
{
    struct dos_find f;
    int r = sys_findfirst(pattern, &f);
    while (r == 0 && track_count < MAX_TRACKS) {
        if (playable(f.name)) {
            strncpy(names[track_count], f.name, NAME_MAX - 1);
            names[track_count][NAME_MAX - 1] = 0;
            sizes[track_count] = f.size;
            track_count++;
        }
        r = sys_findnext(&f);
    }
}

static void build_playlist(const char *where)
{
    char pattern[128];
    int n;
    strncpy(dir_path, where, sizeof dir_path - 1);
    dir_path[sizeof dir_path - 1] = 0;
    n = (int)strlen(dir_path);
    while (n > 1 && (dir_path[n - 1] == '\\' || dir_path[n - 1] == '/'))
        dir_path[--n] = 0;
    pattern[0] = 0;
    if (n) {
        strcpy(pattern, dir_path);
        if (pattern[n - 1] != '\\' && pattern[n - 1] != ':') strcat(pattern, "\\");
    }
    strcat(pattern, "*.MP3");
    add_pattern(pattern);
    pattern[strlen(pattern) - 3] = 0;
    strcat(pattern, "WAV");
    add_pattern(pattern);
}

static void full_path(int i, char *out)
{
    int n = (int)strlen(dir_path);
    out[0] = 0;
    if (n) {
        strcpy(out, dir_path);
        if (out[n - 1] != '\\' && out[n - 1] != ':') strcat(out, "\\");
    }
    strcat(out, names[i]);
}

/* ---------------------------------------------------------------- drawing */
static void draw_static(void)
{
    int i;
    gfx_clear(PAL_BACK);
    /* a title bar across the top */
    for (i = 0; i < 26; i++)
        gfx_rect(0, i, gfx_w, 1, i < 13 ? PAL_ACCENT : PAL_ACCENT - 0);
    gfx_rect(0, 26, gfx_w, 1, PAL_PANEL_LO);
    gfx_text(10, 5, "Ember Player", PAL_WHITE, -1);

    list_x = 10;
    list_y = 36;
    list_w = gfx_w / 2 - 16;
    list_h = gfx_h - 36 - 46;
    rows = (list_h - 8) / gfx_font_h;
    gfx_panel(list_x, list_y, list_w, list_h);

    info_x = gfx_w / 2 + 6;
    info_y = 36;
    info_w = gfx_w - info_x - 10;
    gfx_panel(info_x, info_y, info_w, list_h);
    gfx_text(info_x + 8, info_y + 8, "Now playing", PAL_ACCENT_HI, -1);

    bar_x = info_x + 8;
    bar_y = info_y + 8 + gfx_font_h * 6;
    bar_w = info_w - 16;
    gfx_rect(bar_x, bar_y, bar_w, 12, PAL_TROUGH);
    gfx_bevel(bar_x, bar_y, bar_w, 12, PAL_PANEL_LO, PAL_PANEL_HI);

    vu_x = info_x + 8;
    vu_y = bar_y + 26;
    vu_w = info_w - 16;
    vu_h = 10;
    gfx_text(info_x + 8, vu_y - gfx_font_h - 2, "Level", PAL_DIM, -1);
    gfx_rect(vu_x, vu_y, vu_w, vu_h, PAL_TROUGH);
    gfx_rect(vu_x, vu_y + vu_h + 4, vu_w, vu_h, PAL_TROUGH);

    gfx_rect(0, gfx_h - 22, gfx_w, 22, PAL_PANEL);
    gfx_rect(0, gfx_h - 22, gfx_w, 1, PAL_PANEL_HI);
    gfx_text(10, gfx_h - 19,
             "Up/Dn pick  Enter play  Space pause  N/P skip  +/- volume  Esc quit",
             PAL_TEXT, -1);
}

static void draw_list(void)
{
    int i, y = list_y + 4;
    int cols = (list_w - 16) / 8;
    if (cols > 60) cols = 60;
    if (selected < top_row) top_row = selected;
    if (selected >= top_row + rows) top_row = selected - rows + 1;
    for (i = 0; i < rows; i++, y += gfx_font_h) {
        int t = top_row + i;
        char line[72];
        int bg = PAL_PANEL, fg = PAL_TEXT;
        if (t < track_count) {
            if (t == selected) { bg = PAL_ACCENT; fg = PAL_WHITE; }
            else if (t == playing) fg = PAL_ACCENT_HI;
            snprintf(line, sizeof line, "%c %-12s %4u KB",
                     t == playing ? (paused ? '=' : '>') : ' ',
                     names[t], (unsigned)(sizes[t] / 1024));
        } else {
            line[0] = 0;
        }
        gfx_rect(list_x + 4, y, list_w - 8, gfx_font_h, bg);
        gfx_textn(list_x + 8, y, line, cols, fg, bg);
    }
}

static void draw_info(void)
{
    char s[80];
    int y = info_y + 8 + gfx_font_h * 2;
    int cols = (info_w - 16) / 8;
    if (cols > 40) cols = 40;
    gfx_rect(info_x + 4, info_y + 8 + gfx_font_h, info_w - 8, gfx_font_h * 4, PAL_PANEL);
    if (playing >= 0) {
        gfx_textn(info_x + 8, info_y + 8 + gfx_font_h, names[playing], cols,
                  PAL_WHITE, PAL_PANEL);
        snprintf(s, sizeof s, "%d Hz  %s  %d kbps", audio_rate,
                 audio_channels > 1 ? "stereo" : "mono", audio_bitrate);
        gfx_textn(info_x + 8, y, s, cols, PAL_TEXT, PAL_PANEL);
        snprintf(s, sizeof s, "%ld:%02ld of %ld:%02ld     volume %d",
                 audio_seconds / 60, audio_seconds % 60,
                 audio_total_seconds / 60, audio_total_seconds % 60,
                 audio_volume);
        gfx_textn(info_x + 8, y + gfx_font_h, s, cols, PAL_TEXT, PAL_PANEL);
        if (paused)
            gfx_textn(info_x + 8, y + gfx_font_h * 2, "paused", cols,
                      PAL_ACCENT_HI, PAL_PANEL);
    } else {
        gfx_textn(info_x + 8, info_y + 8 + gfx_font_h, "nothing", cols,
                  PAL_DIM, PAL_PANEL);
    }
}

static void draw_progress(void)
{
    int filled = 0;
    if (playing >= 0 && audio_total_seconds > 0)
        filled = (int)(audio_seconds * (bar_w - 4) / audio_total_seconds);
    if (filled < 0) filled = 0;
    if (filled > bar_w - 4) filled = bar_w - 4;
    gfx_rect(bar_x + 2, bar_y + 2, bar_w - 4, 8, PAL_TROUGH);
    gfx_rect(bar_x + 2, bar_y + 2, filled, 8, PAL_ACCENT_HI);
}

static void draw_meter(int y, int peak)
{
    int n = vu_w * peak / 32768, i;
    if (n > vu_w) n = vu_w;
    gfx_rect(vu_x, y, vu_w, vu_h, PAL_TROUGH);
    for (i = 0; i < n; i += 4) {
        int frac = i * 5 / vu_w;
        gfx_rect(vu_x + i, y, 3, vu_h, PAL_VU1 + (frac > 4 ? 4 : frac));
    }
}

/* ---------------------------------------------------------------- playing */
static void start_track(int i)
{
    char path[128];
    if (i < 0 || i >= track_count) return;
    full_path(i, path);
    audio_silence();
    if (audio_open(path) != 0) {
        playing = -1;
        return;
    }
    playing = i;
    paused = 0;
    audio_peak_l = audio_peak_r = 0;
}

int main(int argc, char **argv)
{
    const char *where = "";
    int quit = 0, redraw_list = 1, redraw_info = 1;
    uint64_t next_frame = 0, per_frame;
    char first[128];

    if (argc > 1) where = argv[1];
    /* a single file: play its directory, starting on that file */
    first[0] = 0;
    if (argc > 1 && playable(argv[1])) {
        const char *slash = strrchr(argv[1], '\\');
        if (!slash) slash = strrchr(argv[1], '/');
        if (slash) {
            int n = (int)(slash - argv[1]);
            memcpy(dir_path, argv[1], n);
            dir_path[n] = 0;
            where = dir_path;
            strncpy(first, slash + 1, sizeof first - 1);
        } else {
            where = "";
            strncpy(first, argv[1], sizeof first - 1);
        }
        first[sizeof first - 1] = 0;
    }

    build_playlist(where);
    if (track_count == 0) {
        printf("PLAYER: no .MP3 or .WAV files in %s\n", dir_path[0] ? dir_path : "this directory");
        return 1;
    }
    if (first[0]) {
        int i;
        for (i = 0; i < track_count; i++)
            if (!strcasecmp(names[i], first)) { selected = i; break; }
    }

    if (audio_start() != 0) {
        printf("PLAYER: no sound hardware to play through\n");
        return 1;
    }
    gfx_init();
    draw_static();
    per_frame = 20000000;               /* about a twentieth of a second */

    start_track(selected);              /* straight into the music */

    while (!quit) {
        int key;
        if (playing >= 0 && !paused) {
            if (!audio_pump()) {        /* the track ended: go to the next */
                if (playing + 1 < track_count) {
                    selected = playing + 1;
                    start_track(selected);
                } else {
                    audio_close();
                    audio_silence();
                    playing = -1;
                }
                redraw_list = redraw_info = 1;
            }
        }
        while ((key = sys_kbhit()) != 0) {
            int scan = (key >> 8) & 0xFF, ch = key & 0xFF;
            sys_getkey();
            switch (scan) {
            case 0x48:                                  /* up */
                if (selected > 0) { selected--; redraw_list = 1; }
                break;
            case 0x50:                                  /* down */
                if (selected + 1 < track_count) { selected++; redraw_list = 1; }
                break;
            case 0x49:                                  /* page up */
                selected -= rows;
                if (selected < 0) selected = 0;
                redraw_list = 1;
                break;
            case 0x51:                                  /* page down */
                selected += rows;
                if (selected >= track_count) selected = track_count - 1;
                redraw_list = 1;
                break;
            case 0x1C:                                  /* enter */
                start_track(selected);
                redraw_list = redraw_info = 1;
                break;
            default:
                if (ch == 27) quit = 1;
                else if (ch == ' ') {
                    paused = !paused;
                    if (paused) audio_silence();
                    redraw_list = redraw_info = 1;
                } else if (ch == 'n' || ch == 'N') {
                    if (playing + 1 < track_count) {
                        selected = playing + 1;
                        start_track(selected);
                        redraw_list = redraw_info = 1;
                    }
                } else if (ch == 'p' || ch == 'P') {
                    if (playing > 0) {
                        selected = playing - 1;
                        start_track(selected);
                        redraw_list = redraw_info = 1;
                    }
                } else if (ch == '+' || ch == '=') {
                    if (audio_volume < 10) audio_volume++;
                    redraw_info = 1;
                } else if (ch == '-' || ch == '_') {
                    if (audio_volume > 0) audio_volume--;
                    redraw_info = 1;
                }
                break;
            }
        }
        if (rdtsc64() >= next_frame) {
            next_frame = rdtsc64() + per_frame;
            if (redraw_list) { draw_list(); redraw_list = 0; }
            draw_info();
            draw_progress();
            draw_meter(vu_y, audio_peak_l);
            draw_meter(vu_y + vu_h + 4, audio_peak_r);
            audio_decay_peaks();
            redraw_info = 0;
        }
    }

    audio_close();
    audio_silence();
    audio_stop();
    gfx_done();
    return 0;
}
