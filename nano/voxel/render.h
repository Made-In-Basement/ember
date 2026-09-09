/* =============================================================================
   render.h - putting the game's triangles on the screen
   -----------------------------------------------------------------------------
   The game hands over a list of triangles in world coordinates and a camera
   to look at them from.  Everything behind this interface is about turning
   that into pixels: the processor transforms, projects and clips, and the
   graphics engine rasterises, samples the texture and sorts by depth.
   ========================================================================== */
#ifndef VOX_RENDER_H
#define VOX_RENDER_H

#include "game.h"

/* Take the screen and wake the engine.  0 on success. */
int  vox_render_open(void);

/* One frame.  `notice`, when not null, is a line of text to show. */
void vox_render(const vg_frame *frame, const char *notice);

/* Give the screen back. */
void vox_render_close(void);

#endif
