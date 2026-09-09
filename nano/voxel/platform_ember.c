/* =============================================================================
   platform_ember.c - the voxel game, on Ember
   -----------------------------------------------------------------------------
   The game core knows nothing about this machine.  It asks for input and a
   number of seconds, hands back a list of triangles and a camera, and expects
   somebody to put them on a screen.  This file is that somebody: the
   keyboard and mouse Ember already drives, the cycle counter for time, the
   filesystem for saves, and the graphics engine for the picture.

   Nothing here is game logic.  If something looks like a rule about how the
   world behaves, it belongs in game.c instead.
   ========================================================================== */

#include <nanolibc.h>
#include "nano.h"
#include "input.h"
#include "platform.h"
#include "render.h"

/* ---- scancodes, as the keyboard sends them ---- */
enum {
    K_1 = 0x02, K_2, K_3, K_4, K_5, K_6, K_7, K_8, K_9, K_0,
    K_W = 0x11, K_E = 0x12, K_T = 0x14, K_U = 0x16,
    K_S = 0x1F, K_A = 0x1E, K_D = 0x20,
    K_C = 0x2E, K_B = 0x30, K_N = 0x31, K_M = 0x32,
    K_SPACE = 0x39, K_ESCAPE = 0x01, K_F5 = 0x3F, K_F9 = 0x43
};

/* ---- time ------------------------------------------------------------------
   The cycle counter, calibrated once against the interval timer, the same
   way Doom's port does it. */
static uint64_t tsc_hz;
static uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint16_t pit_now(void)
{
    uint16_t v;
    outb(0x43, 0x00);                            /* latch channel 0 */
    v = inb(0x40);
    v |= (uint16_t)inb(0x40) << 8;
    return v;
}

static void clock_start(void)
{
    uint64_t t0, t1;
    uint16_t a, b, gone = 0;
    a = pit_now();
    t0 = rdtsc();
    do {                                         /* the counter runs downward */
        b = pit_now();
        gone = (uint16_t)(a - b);
    } while (gone < 40000);                      /* about 33 ms of the 1.19 MHz clock */
    t1 = rdtsc();
    tsc_hz = (t1 - t0) * 1193182u / gone;
    if (tsc_hz < 100000000u || tsc_hz > 6000000000u)
        tsc_hz = 2000000000u;                    /* the timer would not answer */
}

/* The input module keeps its own timings in milliseconds; on the desktop
   that clock comes from the drawing code, which this program does not have. */
unsigned now_ms(void)
{
    if (!tsc_hz) return 0;
    return (unsigned)(rdtsc() / (tsc_hz / 1000));
}

/* ---- the platform's own state ---- */
typedef struct {
    int running;
    int paused;
    uint64_t last;
    int last_mx, last_my;
    unsigned int selection, tool;
    unsigned int craft, mode_cmd, time_cmd;
    uint32_t commands;
    int edge[128];                               /* keys already acted on */
    char notice[128];
    float notice_left;
} ember_platform;

static ember_platform ep;

/* A key that has just gone down, once per press. */
static int pressed(int sc)
{
    int down = input_key_down[sc & 0x7F] != 0;
    if (down && !ep.edge[sc & 0x7F]) { ep.edge[sc & 0x7F] = 1; return 1; }
    if (!down) ep.edge[sc & 0x7F] = 0;
    return 0;
}

static int held(int sc) { return input_key_down[sc & 0x7F] != 0; }

static int poll(void *user, vg_input *in, float *seconds)
{
    uint64_t now;
    struct event e;
    int i;
    (void)user;

    /* drain the queue so the mouse and keyboard keep flowing */
    while (next_event(&e)) { }

    now = rdtsc();
    *seconds = (float)((double)(now - ep.last) / (double)tsc_hz);
    ep.last = now;
    /* A step longer than this walks the player straight through a wall: the
       game moves them the whole distance and only then asks what they hit.
       Slower than real time is better than falling out of the world. */
    if (*seconds > 0.04f) *seconds = 0.04f;

    memset(in, 0, sizeof(*in));

    if (pressed(K_ESCAPE)) ep.running = 0;
    if (pressed(K_F5)) ep.commands |= VG_SAVE;
    if (pressed(K_F9)) ep.commands |= VG_LOAD;
    if (pressed(K_N)) ep.commands |= VG_NEXT_TIME;
    if (pressed(K_C)) ep.craft = VG_CRAFT_TORCH;
    for (i = 0; i < 6; i++)
        if (pressed(K_1 + i)) ep.selection = (unsigned int)i + 1;
    if (pressed(K_T) || pressed(K_0)) ep.selection = VG_TORCH;
    if (pressed(K_B)) ep.selection = VG_WATER;
    if (pressed(K_7)) ep.tool = VG_PICKAXE;
    if (pressed(K_8)) ep.tool = VG_AXE;
    if (pressed(K_9)) ep.tool = VG_SHOVEL;
    if (pressed(K_6)) ep.tool = VG_HAND;

    in->buttons = ep.commands; ep.commands = 0;
    in->craft = ep.craft; ep.craft = 0;
    in->game_mode = ep.mode_cmd; ep.mode_cmd = 0;
    in->time_mode = ep.time_cmd; ep.time_cmd = 0;
    in->selected_block = ep.selection;
    in->selected_tool = ep.tool;

    if (!ep.running)
        return 0;

    in->forward = (float)(held(K_W) - held(K_S));
    in->strafe = (float)(held(K_D) - held(K_A));
    if (held(K_SPACE)) in->buttons |= VG_JUMP;
    if (mouse_buttons & 1) in->buttons |= VG_BREAK;
    if (mouse_buttons & 2) in->buttons |= VG_PLACE;

    /* Looking around: the pointer's travel since the last frame, then put it
       back in the middle so it never reaches an edge and stops. */
    in->look_yaw = (float)(mouse_x - ep.last_mx) * 0.0025f;
    in->look_pitch = (float)(ep.last_my - mouse_y) * 0.0025f;
    mouse_x = mouse_max_x / 2;
    mouse_y = mouse_max_y / 2;
    ep.last_mx = mouse_x;
    ep.last_my = mouse_y;

    if (ep.notice_left > 0) {
        ep.notice_left -= *seconds;
        if (ep.notice_left <= 0) ep.notice[0] = 0;
    }
    return 1;
}

static void present(void *user, const vg_frame *f)
{
    (void)user;
    vox_render(f, ep.notice[0] ? ep.notice : 0);
}

static int read_file(void *user, const char *name, void *bytes, size_t size)
{
    int h, got;
    (void)user;
    h = sys_open(name);
    if (h < 0) return 0;
    got = sys_read(h, bytes, (int)size);
    sys_close(h);
    return got == (int)size;
}

static int write_file(void *user, const char *name, const void *bytes, size_t size)
{
    int h, put;
    (void)user;
    /* There is no rename to lean on here, so the save is written in place.
       A failure part way through loses the previous world; worth revisiting
       if saves ever get large enough for that to be a real risk. */
    h = sys_create(name);
    if (h < 0) return 0;
    put = sys_write(h, bytes, (int)size);
    sys_close(h);
    return put == (int)size;
}

static void message(void *user, const char *text)
{
    (void)user;
    strncpy(ep.notice, text, sizeof(ep.notice) - 1);
    ep.notice[sizeof(ep.notice) - 1] = 0;
    ep.notice_left = 3.0f;
    sys_logf("VOXEL: %s", text);
}

static void audio(void *user, const vg_event *events, size_t count)
{
    (void)user; (void)events; (void)count;
    /* the sounds come later; the game runs silent until then */
}

int main(int argc, char **argv)
{
    vg_platform platform;
    uint32_t seed = 1234567u;
    int rc;
    (void)argc; (void)argv;

    /* A stamp so a log can never be mistaken for an older run's. */
    sys_logf("VOXEL: build %s", VOX_BUILD);

    clock_start();
    /* Listen rather than take charge.  Resetting the pointing device gets no
       answer on this machine - the desktop found the same and opens it
       passively too - so the firmware keeps driving it and we watch. */
    if (input_open(4096, 4096, 1) < 0)
        sys_log("VOXEL: no mouse; the keyboard still works");
    input_start_keyboard();
    mouse_max_x = 4096;
    mouse_max_y = 4096;
    mouse_x = ep.last_mx = mouse_max_x / 2;
    mouse_y = ep.last_my = mouse_max_y / 2;

    if (vox_render_open() != 0) {
        input_close();
        sys_puts("The graphics engine would not start.\r\n");
        return 1;
    }

    memset(&platform, 0, sizeof(platform));
    platform.user = &ep;
    platform.poll = poll;
    platform.present = present;
    platform.read_file = read_file;
    platform.write_file = write_file;
    platform.message = message;
    platform.audio = audio;

    ep.running = 1;
    ep.selection = VG_DIRT;
    ep.tool = VG_HAND;
    ep.last = rdtsc();
    rc = vg_run(&platform, seed);

    vox_render_close();
    input_close();
    sys_logf("VOXEL: left with %d", rc);
    return rc;
}
