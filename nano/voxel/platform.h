#ifndef VOXEL_PLATFORM_H
#define VOXEL_PLATFORM_H
#include "game.h"

/* All callbacks run synchronously on the caller's thread.
   read/write return 1 only for a complete successful transfer.
   poll returns 0 to quit; seconds is elapsed time, not an absolute timestamp.
   present must consume/copy the frame before returning. No pointer retention. */
typedef struct vg_platform {
    void *user;
    int (*poll)(void *user, vg_input *input, float *seconds);
    void (*present)(void *user, const vg_frame *frame);
    int (*read_file)(void *user, const char *name, void *bytes, size_t size);
    int (*write_file)(void *user, const char *name, const void *bytes, size_t size);
    /* Optional. Copy/consume synchronously, called once after each step with events. */
    void (*audio)(void *user, const vg_event *events, size_t count);
    void (*message)(void *user, const char *text);
} vg_platform;
/* 0 = normal exit; 1 = invalid interface or allocation failure. */
int vg_run(const vg_platform *platform, uint32_t seed);
#endif
