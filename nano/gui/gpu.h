/* gpu.h - the display engine of an Intel graphics chip, put to two uses:
 * scanning the desktop's own buffer out directly, and a hardware pointer. */
#ifndef GPU_H
#define GPU_H

/* Point the display at buffer a, one of two page-aligned blocks of w x h
   pixels in ordinary memory, `pitch` bytes a row.  0 when the display now
   reads it; otherwise nothing has changed and the caller keeps copying. */
int  gpu_open(uint32_t *a, uint32_t *b, int w, int h, int pitch);
void gpu_close(void);                   /* the display back to the firmware's surface */
int  gpu_active(void);

/* Settle a rectangle of a buffer into memory, where the display reads:
   its cache lines are written back. */
void gpu_flush(const uint32_t *buf, int x, int y, int w, int h);

/* show buffer 0 or 1 from the next vertical blank; whether it shows yet */
void gpu_flip(int which);
int  gpu_flip_done(void);

/* the pointer as a sprite: up to 64x64 ARGB, with its hot spot */
void gpu_cursor_image(const uint32_t *argb, int w, int h, int hot_x, int hot_y);
void gpu_cursor_move(int x, int y);
void gpu_cursor_show(int on);

const char *gpu_note(void);             /* one line on what was found, for the Monitor */

#endif
