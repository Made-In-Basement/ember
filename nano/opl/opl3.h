/* =============================================================================
   opl3.h - a Yamaha YMF262 (OPL3), and its OPL2 ancestor, in software
   -----------------------------------------------------------------------------
   The chip that made DOS games sing.  A program writes register pairs to two
   ports and the chip turns them into eighteen voices of frequency modulation;
   everything below is that chip, with no ports, no timers and no idea what a
   game is.  Feed it register writes, ask it for samples.

   It deliberately depends on nothing but stdint: the same object is meant to
   be linked into an Ember program today and into a resident driver later,
   where there is no C library to call.  The one exception is table setup,
   which wants a logarithm; see opl3_reset.
   ========================================================================== */
#ifndef OPL3_H
#define OPL3_H

#include <stdint.h>

#define OPL3_CHANNELS   18
#define OPL3_OPERATORS  36
#define OPL3_NATIVE_HZ  49716           /* what the real chip runs at */

struct opl3_op {
    /* what the registers say */
    uint8_t  am, vib, egt, ksr;         /* 20h: tremolo, vibrato, sustain, rate scale */
    uint8_t  mult;                      /* 20h: frequency multiplier, coded */
    uint8_t  ksl, tl;                   /* 40h: level scaling and total level */
    uint8_t  ar, dr, sl, rr;            /* 60h/80h: the four envelope corners */
    uint8_t  wave;                      /* E0h: which of the eight shapes */

    /* what the operator is doing */
    uint32_t phase;                     /* 10.10 fixed point into the wave */
    uint16_t eg_frac;                   /* the part of a level step not yet taken */
    int16_t  eg_level;                  /* attenuation now, 0 loud .. 511 silent */
    uint8_t  eg_state;                  /* OPL3_EG_* below */
    uint8_t  keyed;                     /* the channel is sounding */
    int16_t  out, prev;                 /* the last two samples, for feedback */
    struct opl3_ch *ch;                 /* the channel it belongs to */
};

enum { OPL3_EG_OFF, OPL3_EG_ATTACK, OPL3_EG_DECAY, OPL3_EG_SUSTAIN, OPL3_EG_RELEASE };

struct opl3_ch {
    uint16_t fnum;                      /* A0h/B0h: pitch, eleven bits */
    uint8_t  block;                     /* B0h: octave */
    uint8_t  keyon;
    uint8_t  fb, cnt;                   /* C0h: feedback depth, how ops connect */
    uint8_t  left, right;               /* C0h: OPL3's two speakers */
    uint8_t  alg;                       /* which of the four-operator shapes */
    struct opl3_op *op[4];              /* two, or four when paired */
    uint8_t  four;                      /* the head of a four-operator voice */
    uint8_t  muted;                     /* the tail of one: it makes no sound alone */
};

struct opl3 {
    struct opl3_op  ops[OPL3_OPERATORS];
    struct opl3_ch  chs[OPL3_CHANNELS];
    uint8_t  regs[512];

    uint8_t  opl3_mode;                 /* 105h: the new registers answer */
    uint8_t  rhythm;                    /* BDh: five percussion voices */
    uint8_t  dam, dvb;                  /* BDh: tremolo and vibrato depth */
    uint8_t  nts;                       /* 08h: which bit scales the rate */

    uint32_t eg_timer;                  /* drives every envelope */
    uint8_t  eg_state;
    uint16_t trem_pos;                  /* the tremolo triangle */
    uint8_t  trem_val;
    uint8_t  vib_pos;                   /* the vibrato staircase */
    uint32_t noise;                     /* the rhythm section's noise source */

    /* native rate to the caller's rate */
    uint32_t rate;
    uint32_t step, frac;                /* 16.16 */
    int16_t  s_l[2], s_r[2];            /* the two samples being interpolated */
};

/* Set the chip back to silence and prepare it to run at `rate` samples a
   second (44100 for Ember's HD Audio stream).  Call once before anything. */
void opl3_reset(struct opl3 *c, uint32_t rate);

/* A register write, exactly as a game would make it.  `reg` is 0x000-0x0FF
   for the first bank and 0x100-0x1FF for OPL3's second. */
void opl3_write(struct opl3 *c, uint16_t reg, uint8_t val);

/* Silence one channel at once, envelopes and all.  A player reusing a voice
   needs this: keying off starts a release, and a patch whose release rate is
   zero never finishes one, so the old note would ring under the new. */
void opl3_channel_off(struct opl3 *c, int ch);

/* Interleaved stereo, signed 16-bit, at the rate given to opl3_reset. */
void opl3_render(struct opl3 *c, int16_t *out, int frames);

/* The same, added into what is already there, for mixing alongside effects.
   `vol` is 0-255. */
void opl3_render_mix(struct opl3 *c, int16_t *out, int frames, int vol);

#endif
