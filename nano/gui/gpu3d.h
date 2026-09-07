/* gpu3d.h - the chip's 3D engine as a service for the desktop's programs.
 *
 * A program hands over triangles in screen space - for each corner x*w,
 * y*w, z*w, w and a texture coordinate - and a rectangle of the window to
 * draw them in.  At the next present, after the frame's own pixels have
 * reached memory, the engine clears that rectangle to a texel, sorts the
 * triangles with its depth buffer, samples the one 64x64 texture, and
 * writes the pixels straight into the buffer the display will show. */
#ifndef GPU3D_H
#define GPU3D_H

int  gpu3d_open(void);                  /* 0 when the engine is ready; needs the display driver */
void gpu3d_close(void);
int  gpu3d_active(void);
const char *gpu3d_note(void);           /* one line for the Monitor */
unsigned gpu3d_last_us(void);           /* the engine's time on the last job */
unsigned gpu3d_mhz(void);               /* the clock it is running at */

void gpu3d_texture(const uint32_t *argb);   /* 64 x 64, row-major */

/* verts: n * 6 floats, screen coordinates already offset into the window;
   bg_u, bg_v: the texel the rectangle is cleared to.  Up to 256 corners. */
int  gpu3d_queue(int x, int y, int w, int h, const float *verts, int n, float bg_u, float bg_v);

/* for draw.c: run what was queued into this buffer, once it is flushed */
void gpu3d_run(uint32_t *buffer);

#endif
