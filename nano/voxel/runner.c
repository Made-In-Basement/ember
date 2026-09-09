#include "platform.h"
#include <stdlib.h>

static void tell(const vg_platform *p, const char *text)
{
    if (p->message) p->message(p->user, text);
}
int vg_run(const vg_platform *p, uint32_t seed)
{
    vg_game *g;
    unsigned char *save;
    uint32_t previous = 0;
    if (!p || !p->poll || !p->present) return 1;
    g = vg_create(seed);
    if (!g) return 1;
    save = (unsigned char *)malloc(VG_SAVE_BYTES);
    if (!save) { vg_destroy(g); return 1; }
    for (;;) {
        vg_input input = {0};
        vg_frame frame;
        float seconds = 0.0f;
        uint32_t pressed;
        if (!p->poll(p->user, &input, &seconds)) break;
        pressed = input.buttons & ~previous;
        previous = input.buttons;
        if (pressed & VG_LOAD) {
            int loaded=0;
            if (p->read_file) {
                if (p->read_file(p->user,"world.vg",save,VG_SAVE_BYTES)) loaded=vg_import_world(g,save,VG_SAVE_BYTES);
                else if (p->read_file(p->user,"world.vg",save,VG_LEGACY_SAVE_BYTES)) loaded=vg_import_world(g,save,VG_LEGACY_SAVE_BYTES);
            }
            tell(p,loaded ? "World and inventory loaded" : "Load failed or unavailable");
        }
        vg_step(g, &input, seconds);
        if (pressed & VG_SAVE) {
            if (p->write_file && vg_export_world(g, save, VG_SAVE_BYTES)
                && p->write_file(p->user, "world.vg", save, VG_SAVE_BYTES)) tell(p, "World saved");
            else tell(p, "Save failed or unavailable");
        }
        vg_build_frame(g, &frame);
        if (p->audio && frame.event_count) p->audio(p->user,frame.events,frame.event_count);
        p->present(p->user, &frame);
    }
    free(save);
    vg_destroy(g);
    return 0;
}
