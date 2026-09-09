/* =============================================================================
   musopl.h - Doom's MUS songs, played on the OPL core
   -----------------------------------------------------------------------------
   Two things come out of a Doom WAD: GENMIDI, a bank of 175 instruments each
   described as a pair of OPL operator settings, and a handful of songs in
   MUS, a compact cousin of MIDI that id wrote to save space.  This turns one
   into the other: notes in, register writes out.

   Nothing here knows where the WAD came from or where the sound goes.  Hand
   it the two lumps and call mus_tick 140 times a second, which is the rate
   Doom's own clock ran at.
   ========================================================================== */
#ifndef MUSOPL_H
#define MUSOPL_H

#include <stdint.h>
#include "opl3.h"

#define MUS_TICK_HZ 140
#define MUS_VOICES  18                  /* OPL3's full complement */

struct mus_voice {
    int      ch;                        /* the MUS channel using it, or -1 */
    int      note;
    int      instr;
    uint32_t age;                       /* for deciding what to steal */
    uint8_t  on;
};

struct mus_player {
    struct opl3 *chip;
    const uint8_t *gen;                 /* GENMIDI, past its header */
    int gen_ok;

    const uint8_t *score, *pos, *end;
    int  channels;

    uint8_t  ch_instr[16];
    uint8_t  ch_vol[16];
    int16_t  ch_bend[16];               /* in MUS units, 128 = none */

    struct mus_voice voices[MUS_VOICES];
    int      nvoices;
    uint32_t clock;
    int      wait;                      /* ticks still to sit out */
    int      done;
    int      loop;
    uint16_t ch_mask;                   /* channels allowed to sound, for listening in parts */
};

/* GENMIDI as it sits in the WAD, including the "#OPL_II#" header. */
int  mus_bank(struct mus_player *p, const void *genmidi, int len);

/* Point the player at a song and silence whatever was sounding. */
int  mus_load(struct mus_player *p, struct opl3 *chip, const void *mus, int len);

/* One tick of Doom's clock.  Returns 0 once the song has ended and looping
   is off. */
int  mus_tick(struct mus_player *p);

void mus_all_off(struct mus_player *p);

#endif
