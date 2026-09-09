/* =============================================================================
   oplshim.c - the synthesiser, packaged for a resident driver
   -----------------------------------------------------------------------------
   opl3.c is a chip and nothing else: no ports, no timers, no idea what a game
   is.  This wraps it in the three things SB.MOD wants - switch on, take a
   register write, give me one frame - and gives them plain names and plain
   arguments so that assembly can call them.

   The whole of this, code and tables and state, is linked as a flat image at a
   fixed offset inside SB.MOD and loaded with it.  There is no C library
   underneath and no start-up code: the monitor calls opl_reset before anything
   else, and every address in here is reached through the same selectors the
   monitor uses for itself, so it does not matter where in memory the module
   lands.  What must not change without changing modules/sb.asm to match is
   the offset it is linked at and the fact that these four names exist.
   ========================================================================== */
#include "opl3.h"

static struct opl3 chip;

/* One stereo frame, where the monitor can read it.  A frame at a time costs a
   call each, which next to eighteen voices of frequency modulation is
   nothing, and it saves the monitor having to think about buffers. */
int16_t opl_frame[2];

/* whether anything has been written to it yet: a game that never touches the
   synthesiser should not pay for one */
uint8_t opl_live;

void opl_reset(uint32_t rate)
{
    opl3_reset(&chip, rate);
    opl_live = 0;
    opl_frame[0] = 0;
    opl_frame[1] = 0;
}

void opl_write(uint32_t reg, uint32_t val)
{
    opl_live = 1;
    opl3_write(&chip, (uint16_t)reg, (uint8_t)val);
}

void opl_render1(void)
{
    opl3_render(&chip, opl_frame, 1);
}
