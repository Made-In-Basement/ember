/* gpu.h - the display engine of an Intel graphics chip, put to two uses:
 * scanning the desktop's own buffer out directly, and a hardware pointer. */
#ifndef GPU_H
#define GPU_H

/* Point the display at buffer a, one of three page-aligned blocks of w x h
   pixels in ordinary memory, `pitch` bytes a row.  0 when the display now
   reads it; otherwise nothing has changed and the caller keeps copying. */
int  gpu_open(uint32_t *a, uint32_t *b, uint32_t *c, int w, int h, int pitch);
void gpu_close(void);                   /* the display back to the firmware's surface */
int  gpu_active(void);

/* Settle a rectangle of a buffer into memory, where the display reads:
   its cache lines are written back. */
void gpu_flush(const uint32_t *buf, int x, int y, int w, int h);

/* show a buffer from the next vertical blank; which one shows right now */
void gpu_flip(int which);
int  gpu_flip_done(void);
int  gpu_shown(void);

/* the pointer as a sprite: up to 64x64 ARGB, with its hot spot */
void gpu_cursor_image(const uint32_t *argb, int w, int h, int hot_x, int hot_y);
void gpu_cursor_move(int x, int y);
void gpu_cursor_show(int on);

const char *gpu_note(void);             /* one line on what was found, for the Monitor */

/* for the 3D service (gpu3d.c): the chip's registers and page table, a
   mapping of pages of ours, and where a scanout buffer sits for the GPU */
volatile uint32_t *gpu_regs(void);
volatile uint64_t *gpu_table(void);
void gpu_map(uint32_t gpu, uint32_t phys, int pages);
uint32_t gpu_buffer_address(const uint32_t *buffer);

#endif
