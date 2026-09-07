/* gpu.h - the display engine of an Intel graphics chip, put to two uses:
 * scanning the desktop's own buffer out directly, and a hardware pointer. */
#ifndef GPU_H
#define GPU_H

/* Point the display at `buffer`, a page-aligned block of w x h pixels in
   ordinary memory, `pitch` bytes a row.  0 when the display now reads it;
   otherwise nothing has changed and the caller keeps copying frames. */
int  gpu_open(uint32_t *buffer, int w, int h, int pitch);
void gpu_close(void);                   /* the display back to the firmware's surface */
int  gpu_active(void);

/* Make a rectangle of the buffer visible: its cache lines go to memory,
   where the display reads. */
void gpu_flush(int x, int y, int w, int h);

/* the pointer as a sprite: up to 64x64 ARGB, with its hot spot */
void gpu_cursor_image(const uint32_t *argb, int w, int h, int hot_x, int hot_y);
void gpu_cursor_move(int x, int y);
void gpu_cursor_show(int on);

const char *gpu_note(void);             /* one line on what was found, for the Monitor */

#endif
