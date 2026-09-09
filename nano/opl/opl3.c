/* =============================================================================
   opl3.c - the YMF262, sample by sample
   -----------------------------------------------------------------------------
   Every voice is a pair (or a quartet) of operators.  An operator is a sine
   wave whose phase runs at a rate the registers set, multiplied by an
   envelope that opens and shuts.  Point one operator's output at another
   operator's phase and the second one's tone acquires the first one's
   harmonics: that is the whole of frequency modulation, and the whole of why
   a 1990 sound card could sound like a bell, a bass and a snare drum with
   nothing but sine waves and adders.

   Everything happens in the logarithmic domain, as it does in the silicon.
   A sine is stored as its own logarithm, attenuations are added rather than
   multiplied, and one exponential table at the end turns the sum back into a
   number.  It is cheaper than multiplying, and it is why the chip's output
   has the particular grain that it does; a floating point synthesiser that
   skips this step does not sound like an OPL.

   Envelope timing here is modelled rather than copied: the rate table below
   is derived from the datasheet's 2^(rate/4) law instead of the hardware's
   step patterns.  Timings match; the last bit or two of a decay may not.
   ========================================================================== */

#include "opl3.h"

/* ---- the two tables the chip is built around -------------------------------
   logsin holds -log2(sin x) for the first quarter cycle, in 1/256ths, and
   exp holds 2^x - 1 for x in [0,1) scaled by 1024.  Between them any sine of
   any amplitude is two lookups and an add.

   They are written out rather than computed because this file has to link
   where there is no libm: inside a resident driver, and inside Doom, whose
   freestanding runtime has no logarithm to call.  tools/opltables.py
   regenerates them from the same formulas the comments give. */
static const uint16_t logsinrom[256] = {
     2137,  1731,  1543,  1419,  1326,  1252,  1190,  1137,
     1091,  1050,  1013,   979,   949,   920,   894,   869,
      846,   825,   804,   785,   767,   749,   732,   717,
      701,   687,   672,   659,   646,   633,   621,   609,
      598,   587,   576,   566,   556,   546,   536,   527,
      518,   509,   501,   492,   484,   476,   468,   461,
      453,   446,   439,   432,   425,   418,   411,   405,
      399,   392,   386,   380,   375,   369,   363,   358,
      352,   347,   341,   336,   331,   326,   321,   316,
      311,   307,   302,   297,   293,   289,   284,   280,
      276,   271,   267,   263,   259,   255,   251,   248,
      244,   240,   236,   233,   229,   226,   222,   219,
      215,   212,   209,   205,   202,   199,   196,   193,
      190,   187,   184,   181,   178,   175,   172,   169,
      167,   164,   161,   159,   156,   153,   151,   148,
      146,   143,   141,   138,   136,   134,   131,   129,
      127,   125,   122,   120,   118,   116,   114,   112,
      110,   108,   106,   104,   102,   100,    98,    96,
       94,    92,    91,    89,    87,    85,    83,    82,
       80,    78,    77,    75,    74,    72,    70,    69,
       67,    66,    64,    63,    62,    60,    59,    57,
       56,    55,    53,    52,    51,    49,    48,    47,
       46,    45,    43,    42,    41,    40,    39,    38,
       37,    36,    35,    34,    33,    32,    31,    30,
       29,    28,    27,    26,    25,    24,    23,    23,
       22,    21,    20,    20,    19,    18,    17,    17,
       16,    15,    15,    14,    13,    13,    12,    12,
       11,    10,    10,     9,     9,     8,     8,     7,
        7,     7,     6,     6,     5,     5,     5,     4,
        4,     4,     3,     3,     3,     2,     2,     2,
        2,     1,     1,     1,     1,     1,     1,     1,
        0,     0,     0,     0,     0,     0,     0,     0,
};

static const uint16_t exprom[256] = {
        0,     3,     6,     8,    11,    14,    17,    20,
       22,    25,    28,    31,    34,    37,    40,    42,
       45,    48,    51,    54,    57,    60,    63,    66,
       69,    72,    75,    78,    81,    84,    87,    90,
       93,    96,    99,   102,   105,   108,   111,   114,
      117,   120,   123,   126,   130,   133,   136,   139,
      142,   145,   148,   152,   155,   158,   161,   164,
      168,   171,   174,   177,   181,   184,   187,   190,
      194,   197,   200,   204,   207,   210,   214,   217,
      220,   224,   227,   231,   234,   237,   241,   244,
      248,   251,   255,   258,   262,   265,   268,   272,
      276,   279,   283,   286,   290,   293,   297,   300,
      304,   308,   311,   315,   318,   322,   326,   329,
      333,   337,   340,   344,   348,   352,   355,   359,
      363,   367,   370,   374,   378,   382,   385,   389,
      393,   397,   401,   405,   409,   412,   416,   420,
      424,   428,   432,   436,   440,   444,   448,   452,
      456,   460,   464,   468,   472,   476,   480,   484,
      488,   492,   496,   501,   505,   509,   513,   517,
      521,   526,   530,   534,   538,   542,   547,   551,
      555,   560,   564,   568,   572,   577,   581,   585,
      590,   594,   599,   603,   607,   612,   616,   621,
      625,   630,   634,   639,   643,   648,   652,   657,
      661,   666,   670,   675,   680,   684,   689,   693,
      698,   703,   708,   712,   717,   722,   726,   731,
      736,   741,   745,   750,   755,   760,   765,   770,
      774,   779,   784,   789,   794,   799,   804,   809,
      814,   819,   824,   829,   834,   839,   844,   849,
      854,   859,   864,   869,   874,   880,   885,   890,
      895,   900,   906,   911,   916,   921,   927,   932,
      937,   942,   948,   953,   959,   964,   969,   975,
      980,   986,   991,   996,  1002,  1007,  1013,  1018,
};

/* Envelope steps per sample, 16.16, one per rate.  Each four steps of the
   rate field doubles the speed, which is the datasheet's law; the constant
   in front of it is fitted by ear rather than derived, because this file
   models the envelope instead of copying the hardware's step tables.  At the
   value below, Doom's hi-hats and cymbals stop ringing under later notes.
   tools/opltables.py regenerates this. */
static const uint32_t eg_inc[64] = {
            64,         76,         90,        107,        128,        152,
           181,        215,        256,        304,        362,        430,
           512,        608,        724,        861,       1024,       1217,
          1448,       1722,       2048,       2435,       2896,       3444,
          4096,       4870,       5792,       6888,       8192,       9741,
         11585,      13777,      16384,      19483,      23170,      27554,
         32768,      38967,      46340,      55108,      65536,      77935,
         92681,     110217,     131072,     155871,     185363,     220435,
        262144,     311743,     370727,     440871,     524288,     623487,
        741455,     881743,    1048576,    1246974,    1482910,    1763487,
       2097152,    2493948,    2965820,    3526975,
};

/* how much of the octave each "multiple" setting takes, doubled */
static const uint8_t mt[16] = { 1, 2, 4, 6, 8, 10, 12, 14,
                                16, 18, 20, 20, 24, 24, 30, 30 };
/* level scaling: attenuation with pitch, and how much of it each setting uses */
static const uint8_t kslrom[16] = { 0, 32, 40, 45, 48, 51, 53, 55,
                                    56, 58, 59, 60, 61, 62, 63, 64 };
static const uint8_t kslshift[4] = { 8, 1, 2, 0 };
/* the vibrato staircase, in eighths of the depth */
static const int8_t vibpat[8] = { 0, 1, 2, 1, 0, -1, -2, -1 };
/* register offset of each of the eighteen operators within its bank */
static const uint8_t op_off[18] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
                                    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D,
                                    0x10, 0x11, 0x12, 0x13, 0x14, 0x15 };

/* ---- wiring ----------------------------------------------------------------
   Operators and channels are laid out so that channel n of a bank owns
   operators n and n+3 of its group of six. */
static void wire(struct opl3 *c)
{
    int bank, n, i;
    for (bank = 0; bank < 2; bank++) {
        for (n = 0; n < 9; n++) {
            struct opl3_ch *ch = &c->chs[bank * 9 + n];
            int g = (n / 3) * 6 + (n % 3);
            ch->op[0] = &c->ops[bank * 18 + g];
            ch->op[1] = &c->ops[bank * 18 + g + 3];
            ch->op[2] = ch->op[3] = 0;
        }
    }
    for (i = 0; i < OPL3_OPERATORS; i++)
        c->ops[i].ch = 0;
    for (i = 0; i < OPL3_CHANNELS; i++) {
        c->chs[i].op[0]->ch = &c->chs[i];
        c->chs[i].op[1]->ch = &c->chs[i];
    }
}

/* An operator's index within its bank, given a register offset, or -1. */
static int op_from_off(uint8_t off)
{
    int i;
    for (i = 0; i < 18; i++)
        if (op_off[i] == off)
            return i;
    return -1;
}

void opl3_reset(struct opl3 *c, uint32_t rate)
{
    int i;
    uint8_t *p = (uint8_t *)c;
    for (i = 0; i < (int)sizeof(*c); i++)
        p[i] = 0;
    wire(c);
    for (i = 0; i < OPL3_OPERATORS; i++) {
        c->ops[i].eg_level = 511;
        c->ops[i].eg_state = OPL3_EG_OFF;
        c->ops[i].mult = 0;
    }
    for (i = 0; i < OPL3_CHANNELS; i++)
        c->chs[i].left = c->chs[i].right = 1;    /* OPL2 is heard on both */
    c->noise = 1;
    c->rate = rate ? rate : 44100;
    c->step = (uint32_t)(((uint64_t)OPL3_NATIVE_HZ << 16) / c->rate);
    c->frac = 0;
}

/* ---- four-operator pairing -------------------------------------------------
   OPL3 can staple channel n to channel n+3 and run one voice through all
   four operators.  The tail channel then makes no sound of its own. */
static void refresh_four(struct opl3 *c)
{
    int bank, n;
    uint8_t bits = c->opl3_mode ? c->regs[0x104] : 0;
    for (bank = 0; bank < 2; bank++) {
        for (n = 0; n < 3; n++) {
            struct opl3_ch *head = &c->chs[bank * 9 + n];
            struct opl3_ch *tail = &c->chs[bank * 9 + n + 3];
            int on = (bits >> (bank * 3 + n)) & 1;
            head->four = (uint8_t)on;
            tail->muted = (uint8_t)on;
            if (on) {
                head->op[2] = tail->op[0];
                head->op[3] = tail->op[1];
                head->alg = (uint8_t)((head->cnt & 1) | ((tail->cnt & 1) << 1));
            } else {
                head->op[2] = head->op[3] = 0;
            }
        }
    }
}

static void key_on(struct opl3_op *o)
{
    o->eg_state = OPL3_EG_ATTACK;
    o->keyed = 1;
    o->phase = 0;
    /* A key-on during a release picks up from wherever the level had got to,
       which is what makes a re-struck note sound joined rather than clicked. */
}

static void key_off(struct opl3_op *o)
{
    if (o->keyed) {
        o->keyed = 0;
        if (o->eg_state != OPL3_EG_OFF)
            o->eg_state = OPL3_EG_RELEASE;
    }
}

static void ch_key(struct opl3 *c, struct opl3_ch *ch, int on)
{
    int i;
    for (i = 0; i < 4; i++) {
        if (!ch->op[i])
            continue;
        if (on)
            key_on(ch->op[i]);
        else
            key_off(ch->op[i]);
    }
    (void)c;
}

void opl3_channel_off(struct opl3 *c, int ch)
{
    int i;
    if (ch < 0 || ch >= OPL3_CHANNELS)
        return;
    c->chs[ch].keyon = 0;
    for (i = 0; i < 4; i++) {
        struct opl3_op *o = c->chs[ch].op[i];
        if (!o)
            continue;
        o->eg_state = OPL3_EG_OFF;
        o->eg_level = 511;
        o->eg_frac = 0;
        o->keyed = 0;
        o->phase = 0;
        o->out = o->prev = 0;
    }
}

void opl3_write(struct opl3 *c, uint16_t reg, uint8_t val)
{
    int bank = (reg >> 8) & 1;
    uint8_t r = (uint8_t)(reg & 0xFF);
    int idx, n;

    reg &= 0x1FF;
    c->regs[reg] = val;

    /* ---- the handful of registers that are not per operator ---- */
    if (bank == 1 && r == 0x05) {               /* 105h: OPL3 awake */
        c->opl3_mode = val & 1;
        if (!c->opl3_mode) {
            for (n = 0; n < OPL3_CHANNELS; n++)
                c->chs[n].left = c->chs[n].right = 1;
        }
        refresh_four(c);
        return;
    }
    if (bank == 1 && r == 0x04) {               /* 104h: which voices pair up */
        refresh_four(c);
        return;
    }
    if (bank == 0 && r == 0x08) {               /* 08h: note select */
        c->nts = (val >> 6) & 1;
        return;
    }
    if (r == 0xBD && bank == 0) {               /* BDh: depths and percussion */
        c->dam = (val >> 7) & 1;
        c->dvb = (val >> 6) & 1;
        c->rhythm = (val >> 5) & 1;
        if (c->rhythm) {
            /* bit 4 bass drum, 3 snare, 2 tom, 1 cymbal, 0 hi-hat; the four
               that are not the bass drum each borrow one operator of a
               channel rather than the whole voice */
            static const struct { int ch, op; } hit[5] = {
                { 6, -1 }, { 7, 1 }, { 8, 0 }, { 8, 1 }, { 7, 0 }
            };
            for (n = 0; n < 5; n++) {
                struct opl3_ch *ch = &c->chs[hit[n].ch];
                int on = (val >> (4 - n)) & 1;
                if (hit[n].op < 0)
                    ch_key(c, ch, on);
                else if (on)
                    key_on(ch->op[hit[n].op]);
                else
                    key_off(ch->op[hit[n].op]);
            }
        }
        return;
    }

    /* ---- per operator ---- */
    if (r >= 0x20 && r <= 0x35) {
        idx = op_from_off((uint8_t)(r - 0x20));
        if (idx < 0) return;
        {
            struct opl3_op *o = &c->ops[bank * 18 + idx];
            o->am = (val >> 7) & 1;
            o->vib = (val >> 6) & 1;
            o->egt = (val >> 5) & 1;
            o->ksr = (val >> 4) & 1;
            o->mult = val & 0x0F;
        }
        return;
    }
    if (r >= 0x40 && r <= 0x55) {
        idx = op_from_off((uint8_t)(r - 0x40));
        if (idx < 0) return;
        c->ops[bank * 18 + idx].ksl = (val >> 6) & 3;
        c->ops[bank * 18 + idx].tl = val & 0x3F;
        return;
    }
    if (r >= 0x60 && r <= 0x75) {
        idx = op_from_off((uint8_t)(r - 0x60));
        if (idx < 0) return;
        c->ops[bank * 18 + idx].ar = (val >> 4) & 0x0F;
        c->ops[bank * 18 + idx].dr = val & 0x0F;
        return;
    }
    if (r >= 0x80 && r <= 0x95) {
        idx = op_from_off((uint8_t)(r - 0x80));
        if (idx < 0) return;
        c->ops[bank * 18 + idx].sl = (val >> 4) & 0x0F;
        c->ops[bank * 18 + idx].rr = val & 0x0F;
        return;
    }
    if (r >= 0xE0 && r <= 0xF5) {
        idx = op_from_off((uint8_t)(r - 0xE0));
        if (idx < 0) return;
        c->ops[bank * 18 + idx].wave = val & (c->opl3_mode ? 7 : 3);
        return;
    }

    /* ---- per channel ---- */
    if (r >= 0xA0 && r <= 0xA8) {
        struct opl3_ch *ch = &c->chs[bank * 9 + (r - 0xA0)];
        ch->fnum = (uint16_t)((ch->fnum & 0x300) | val);
        return;
    }
    if (r >= 0xB0 && r <= 0xB8) {
        struct opl3_ch *ch = &c->chs[bank * 9 + (r - 0xB0)];
        ch->fnum = (uint16_t)((ch->fnum & 0xFF) | ((val & 3) << 8));
        ch->block = (val >> 2) & 7;
        n = (val >> 5) & 1;
        if (n != ch->keyon) {
            ch->keyon = (uint8_t)n;
            ch_key(c, ch, n);
        }
        return;
    }
    if (r >= 0xC0 && r <= 0xC8) {
        struct opl3_ch *ch = &c->chs[bank * 9 + (r - 0xC0)];
        ch->fb = (val >> 1) & 7;
        ch->cnt = val & 1;
        if (c->opl3_mode) {
            ch->left = (val >> 4) & 1;
            ch->right = (val >> 5) & 1;
        }
        refresh_four(c);
        return;
    }
}

/* ---- one operator ---------------------------------------------------------- */

/* The phase step for this operator, before vibrato. */
static uint32_t phase_step(struct opl3 *c, struct opl3_op *o)
{
    struct opl3_ch *ch = o->ch;
    uint32_t f = ((uint32_t)ch->fnum << ch->block) >> 1;
    uint32_t s = (f * mt[o->mult]) >> 1;
    if (o->vib) {
        int d = (ch->fnum >> 7) & 7;
        int v = vibpat[c->vib_pos & 7] * d;
        if (!c->dvb)
            v >>= 1;
        s = (uint32_t)((int32_t)s + ((v * (int32_t)(1 << ch->block)) >> 3));
    }
    return s;
}

/* Attenuation from level scaling, in the envelope's own units. */
static int ksl_of(struct opl3_op *o)
{
    struct opl3_ch *ch = o->ch;
    int v;
    if (!o->ksl)
        return 0;
    v = kslrom[(ch->fnum >> 6) & 0x0F] - ((8 - ch->block) << 5);
    if (v < 0)
        v = 0;
    return v >> kslshift[o->ksl];
}

/* One step of the envelope.  Called once per native sample per operator. */
static void eg_step(struct opl3 *c, struct opl3_op *o)
{
    struct opl3_ch *ch = o->ch;
    int reg, rate, ks, whole;
    uint32_t inc;

    switch (o->eg_state) {
    case OPL3_EG_ATTACK:  reg = o->ar; break;
    case OPL3_EG_DECAY:   reg = o->dr; break;
    case OPL3_EG_RELEASE: reg = o->rr; break;
    case OPL3_EG_SUSTAIN: reg = o->egt ? 0 : o->rr; break;
    default:              return;
    }
    if (reg == 0)
        return;                                 /* held where it is */

    /* Higher notes decay faster; how much faster is the key scale rate. */
    ks = o->ksr ? ((ch->block << 1) | ((ch->fnum >> (c->nts ? 8 : 9)) & 1))
                : (ch->block >> 1);
    rate = reg * 4 + ks;
    if (rate > 63)
        rate = 63;
    inc = eg_inc[rate];

    {
        uint32_t acc = (uint32_t)o->eg_frac + inc;
        whole = (int)(acc >> 16);
        o->eg_frac = (uint16_t)(acc & 0xFFFF);
    }
    if (!whole)
        return;

    switch (o->eg_state) {
    case OPL3_EG_ATTACK:
        /* Fast where it is quiet, slower as it arrives: the curve that makes
           a struck note sound struck. */
        o->eg_level -= (int16_t)(((o->eg_level + 1) * whole) >> 3);
        if (o->eg_level <= 0) {
            o->eg_level = 0;
            o->eg_state = OPL3_EG_DECAY;
        }
        break;
    case OPL3_EG_DECAY:
        o->eg_level = (int16_t)(o->eg_level + whole);
        {
            int sus = o->sl == 15 ? 496 : o->sl * 16;
            if (o->eg_level >= sus) {
                o->eg_level = (int16_t)sus;
                o->eg_state = OPL3_EG_SUSTAIN;
            }
        }
        break;
    default:                                    /* sustain that decays, release */
        o->eg_level = (int16_t)(o->eg_level + whole);
        if (o->eg_level >= 511) {
            o->eg_level = 511;
            if (o->eg_state == OPL3_EG_RELEASE)
                o->eg_state = OPL3_EG_OFF;
        }
        break;
    }
}

/* The operator's sample, given the phase modulation coming into it. */
static int16_t op_out(struct opl3 *c, struct opl3_op *o, int mod)
{
    /* The modulator's output goes into the phase index unscaled: a full-scale
       operator swings the carrier through several cycles, and that violence
       is where a bell, a crash cymbal and a snare come from.  Halving it
       leaves every patch in the bank sounding politely pitched. */
    uint32_t idx = ((o->phase >> 10) + (uint32_t)mod) & 0x3FF;
    uint32_t lg, neg = 0, att, m, e;
    int env;

    switch (o->wave) {
    case 0:                                     /* the whole sine */
        neg = idx & 0x200;
        if (idx & 0x100) idx = ~idx;
        lg = logsinrom[idx & 0xFF];
        break;
    case 1:                                     /* only the top half */
        if (idx & 0x200) { lg = 0x1000; break; }
        if (idx & 0x100) idx = ~idx;
        lg = logsinrom[idx & 0xFF];
        break;
    case 2:                                     /* both halves turned up */
        if (idx & 0x100) idx = ~idx;
        lg = logsinrom[idx & 0xFF];
        break;
    case 3:                                     /* the rising quarters only */
        if (idx & 0x100) { lg = 0x1000; break; }
        lg = logsinrom[idx & 0xFF];
        break;
    case 4:                                     /* twice the rate, half the time */
        if (idx & 0x200) { lg = 0x1000; break; }
        neg = idx & 0x100;
        idx = (idx << 1) & 0x1FF;
        if (idx & 0x100) idx = ~idx;
        lg = logsinrom[idx & 0xFF];
        break;
    case 5:                                     /* the same, rectified */
        if (idx & 0x200) { lg = 0x1000; break; }
        idx = (idx << 1) & 0x1FF;
        if (idx & 0x100) idx = ~idx;
        lg = logsinrom[idx & 0xFF];
        break;
    case 6:                                     /* a square, and no apology */
        neg = idx & 0x200;
        lg = 0;
        break;
    default:                                    /* an exponential ramp */
        neg = idx & 0x200;
        lg = (idx & 0x1FF) << 3;
        if (neg) lg = (0x200 - (idx & 0x1FF)) << 3;
        break;
    }

    env = o->eg_level + (o->tl << 2) + ksl_of(o);
    if (o->am)
        env += c->dam ? c->trem_val : (c->trem_val >> 2);
    if (env > 511)
        env = 511;

    att = lg + ((uint32_t)env << 3);
    if (att > 0x1FFF)
        att = 0x1FFF;
    m = ((uint32_t)exprom[(att & 0xFF) ^ 0xFF] | 0x400) << 1;
    e = att >> 8;
    m >>= e;
    return neg ? (int16_t)-(int32_t)m : (int16_t)m;
}

/* ---- one native sample ----------------------------------------------------- */

static void chip_sample(struct opl3 *c, int32_t *left, int32_t *right)
{
    int i, n;
    int32_t l = 0, r = 0;

    /* the two low-frequency shapes everything else leans on */
    c->eg_timer++;
    if ((c->eg_timer & 0xFF) == 0) {
        c->trem_pos++;
        c->trem_val = (uint8_t)(c->trem_pos & 0x20 ? 52 - (c->trem_pos & 0x1F) * 2
                                                   : (c->trem_pos & 0x1F) * 2);
        if (c->trem_val > 26) c->trem_val = 26;
    }
    if ((c->eg_timer & 0x3FF) == 0)
        c->vib_pos++;
    /* the noise the percussion voices are built out of */
    c->noise = (c->noise >> 1) | ((((c->noise) ^ (c->noise >> 14)) & 1) << 22);

    for (i = 0; i < OPL3_OPERATORS; i++) {
        struct opl3_op *o = &c->ops[i];
        if (!o->ch)
            continue;
        eg_step(c, o);
        o->phase = (o->phase + phase_step(c, o)) & 0xFFFFF;
    }

    for (n = 0; n < OPL3_CHANNELS; n++) {
        struct opl3_ch *ch = &c->chs[n];
        int32_t out = 0, fb, m;
        int rhythm_ch = c->rhythm && (n == 6 || n == 7 || n == 8);

        if (ch->muted)
            continue;
        if (ch->op[0]->eg_state == OPL3_EG_OFF && ch->op[1]->eg_state == OPL3_EG_OFF
            && !ch->four)
            continue;

        /* feedback is the modulator listening to itself, one sample behind */
        fb = ch->fb ? ((ch->op[0]->prev + ch->op[0]->out) >> (9 - ch->fb)) : 0;

        if (ch->four) {
            int32_t a, b, cc, d;
            a = op_out(c, ch->op[0], (int)fb);
            ch->op[0]->prev = ch->op[0]->out; ch->op[0]->out = (int16_t)a;
            switch (ch->alg) {
            case 0:                             /* all four in a chain */
                b = op_out(c, ch->op[1], (int)a);
                cc = op_out(c, ch->op[2], (int)b);
                d = op_out(c, ch->op[3], (int)cc);
                out = d;
                break;
            case 1:                             /* one modulated pair plus a pair */
                b = op_out(c, ch->op[1], (int)a);
                cc = op_out(c, ch->op[2], 0);
                d = op_out(c, ch->op[3], (int)cc);
                out = b + d;
                break;
            case 2:                             /* the first alone, then a chain */
                b = op_out(c, ch->op[1], 0);
                cc = op_out(c, ch->op[2], (int)b);
                d = op_out(c, ch->op[3], (int)cc);
                out = a + d;
                break;
            default:                            /* a chain, then two voices added */
                b = op_out(c, ch->op[1], 0);
                cc = op_out(c, ch->op[2], (int)b);
                d = op_out(c, ch->op[3], 0);
                out = a + cc + d;
                break;
            }
        } else if (rhythm_ch && n != 6) {
            /* Snare, hi-hat, tom and cymbal: the same operators, but their
               phases come partly from the noise generator, which is what
               makes them noise rather than notes. */
            struct opl3_op *mo = ch->op[0], *co = ch->op[1];
            uint32_t hh = c->ops[13].phase, tc = c->ops[17].phase;
            int bit = (((hh >> 12) ^ (hh >> 17)) | ((tc >> 15) ^ (tc >> 17))) & 1;
            int nz = (int)(c->noise & 1);
            if (n == 7) {                       /* hi-hat and snare drum */
                uint32_t p = (uint32_t)((bit << 9) | (0x34 << ((bit ^ nz) << 1)));
                mo->phase = (p & 0x3FF) << 10;
                out += op_out(c, mo, 0);
                mo->prev = mo->out; mo->out = (int16_t)out;
                {
                    uint32_t sp = (uint32_t)((0x100 << ((hh >> 18) & 1)) ^ (nz << 8));
                    co->phase = (sp & 0x3FF) << 10;
                    out += op_out(c, co, 0);
                }
            } else {                            /* tom-tom and top cymbal */
                out += op_out(c, mo, 0);
                mo->prev = mo->out; mo->out = (int16_t)out;
                {
                    uint32_t cp = (uint32_t)(0x100 | ((bit ^ 1) << 9));
                    co->phase = (cp & 0x3FF) << 10;
                    out += op_out(c, co, 0);
                }
            }
        } else {
            m = op_out(c, ch->op[0], (int)fb);
            ch->op[0]->prev = ch->op[0]->out;
            ch->op[0]->out = (int16_t)m;
            if (ch->cnt)                        /* side by side */
                out = m + op_out(c, ch->op[1], 0);
            else                                /* one through the other */
                out = op_out(c, ch->op[1], (int)m);
        }

        if (ch->left)  l += out;
        if (ch->right) r += out;
    }
    *left = l;
    *right = r;
}

/* ---- the caller's rate ----------------------------------------------------- */

static void advance(struct opl3 *c)
{
    int32_t l, r;
    chip_sample(c, &l, &r);
    c->s_l[0] = c->s_l[1];
    c->s_r[0] = c->s_r[1];
    if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
    if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
    c->s_l[1] = (int16_t)l;
    c->s_r[1] = (int16_t)r;
}

static void next_frame(struct opl3 *c, int32_t *l, int32_t *r)
{
    uint32_t f;
    c->frac += c->step;
    while (c->frac >= 0x10000) {
        advance(c);
        c->frac -= 0x10000;
    }
    f = c->frac;
    *l = c->s_l[0] + (((int32_t)(c->s_l[1] - c->s_l[0]) * (int32_t)f) >> 16);
    *r = c->s_r[0] + (((int32_t)(c->s_r[1] - c->s_r[0]) * (int32_t)f) >> 16);
}

void opl3_render(struct opl3 *c, int16_t *out, int frames)
{
    int i;
    int32_t l, r;
    for (i = 0; i < frames; i++) {
        next_frame(c, &l, &r);
        out[i * 2 + 0] = (int16_t)l;
        out[i * 2 + 1] = (int16_t)r;
    }
}

void opl3_render_mix(struct opl3 *c, int16_t *out, int frames, int vol)
{
    int i;
    int32_t l, r, a;
    for (i = 0; i < frames; i++) {
        next_frame(c, &l, &r);
        a = out[i * 2 + 0] + ((l * vol) >> 8);
        if (a > 32767) a = 32767; else if (a < -32768) a = -32768;
        out[i * 2 + 0] = (int16_t)a;
        a = out[i * 2 + 1] + ((r * vol) >> 8);
        if (a > 32767) a = 32767; else if (a < -32768) a = -32768;
        out[i * 2 + 1] = (int16_t)a;
    }
}
