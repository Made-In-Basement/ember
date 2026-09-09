/* =============================================================================
   musopl.c - from a MUS score and a GENMIDI bank to OPL register writes
   ========================================================================== */

#include "musopl.h"

/* ---- GENMIDI --------------------------------------------------------------
   After the eight-byte "#OPL_II#" come 175 records of 36 bytes: two bytes of
   flags, a fine tuning byte, the note a percussion instrument always plays,
   and then two sixteen-byte voices.  A voice is simply the six operator
   registers for the modulator, the feedback byte, the same six for the
   carrier, a spare, and a signed offset in semitones. */
#define GEN_REC   36
#define GEN_COUNT 175

/* offsets within a voice */
enum { V_MOD_MISC = 0, V_MOD_ATT, V_MOD_SUS, V_MOD_WAVE, V_MOD_KSL, V_MOD_LEVEL,
       V_FEEDBACK, V_CAR_MISC, V_CAR_ATT, V_CAR_SUS, V_CAR_WAVE, V_CAR_KSL,
       V_CAR_LEVEL, V_UNUSED, V_OFF_LO, V_OFF_HI };

/* Where a channel's two operators live.  The chip does not lay them out
   consecutively: channel 3 starts at 0x08, not 0x03, because 0x03 is already
   channel 0's carrier.  Indexing the flat list of operator offsets by channel
   number gets the first three right by coincidence and then quietly writes
   one voice's registers over another's. */
static const uint8_t ch_op[9] = { 0x00, 0x01, 0x02, 0x08, 0x09, 0x0A,
                                  0x10, 0x11, 0x12 };

/* fnum for the twelve semitones of an octave, at block 0; the chip's own
   table, so a note lands where a real card would put it */
static const uint16_t note_fnum[12] = {
    345, 365, 387, 410, 435, 460, 488, 517, 547, 580, 615, 651
};

static void wr(struct mus_player *p, int bank, unsigned reg, unsigned val)
{
    opl3_write(p->chip, (uint16_t)((bank << 8) | reg), (uint8_t)val);
}

int mus_bank(struct mus_player *p, const void *genmidi, int len)
{
    const uint8_t *g = (const uint8_t *)genmidi;
    p->gen = 0;
    p->gen_ok = 0;
    if (!g || len < 8 + GEN_COUNT * GEN_REC)
        return -1;
    if (g[0] != '#' || g[1] != 'O' || g[2] != 'P' || g[3] != 'L')
        return -1;
    p->gen = g + 8;
    p->gen_ok = 1;
    return 0;
}

/* ---- voices --------------------------------------------------------------- */

static void voice_silence(struct mus_player *p, int v)
{
    int bank = v / 9, n = v % 9;
    wr(p, bank, 0xB0 + n, 0);           /* key off, and no pitch */
    opl3_channel_off(p->chip, v);       /* and truly off: some patches never release */
    (void)bank; (void)n;
}

void mus_all_off(struct mus_player *p)
{
    int i;
    for (i = 0; i < p->nvoices; i++) {
        voice_silence(p, i);
        p->voices[i].ch = -1;
        p->voices[i].on = 0;
    }
}

/* Volume comes in as 0-127 twice over - the channel's and the note's - and
   leaves as an attenuation to add to whatever the instrument already asked
   for.  The curve is deliberately not linear: halving a number halves its
   amplitude, which is nothing like halving its loudness. */
static int atten_of(int vol)
{
    static const uint8_t curve[16] = {
        63, 40, 32, 27, 23, 20, 18, 16, 14, 12, 10, 8, 6, 4, 2, 0
    };
    if (vol < 0) vol = 0;
    if (vol > 127) vol = 127;
    return curve[vol >> 3];
}

static void voice_program(struct mus_player *p, int v, int instr, int vol, int which)
{
    const uint8_t *rec, *vo;
    int bank = v / 9, n = v % 9, mo, ca, lvl;

    if (!p->gen_ok || instr < 0 || instr >= GEN_COUNT)
        return;
    rec = p->gen + instr * GEN_REC;
    vo = rec + 4 + which * 16;
    mo = ch_op[n];                      /* the operator pair, within its bank */
    ca = mo + 3;

    wr(p, bank, 0x20 + mo, vo[V_MOD_MISC]);
    wr(p, bank, 0x60 + mo, vo[V_MOD_ATT]);
    wr(p, bank, 0x80 + mo, vo[V_MOD_SUS]);
    wr(p, bank, 0xE0 + mo, vo[V_MOD_WAVE]);
    wr(p, bank, 0x20 + ca, vo[V_CAR_MISC]);
    wr(p, bank, 0x60 + ca, vo[V_CAR_ATT]);
    wr(p, bank, 0x80 + ca, vo[V_CAR_SUS]);
    wr(p, bank, 0xE0 + ca, vo[V_CAR_WAVE]);
    wr(p, bank, 0xC0 + n, (vo[V_FEEDBACK] & 0x0F) | 0x30);   /* both speakers */

    /* The carrier's level is the note's loudness.  The modulator's is part of
       the timbre and only follows the volume when the two are added rather
       than one modulating the other. */
    lvl = (vo[V_CAR_LEVEL] & 0x3F) + atten_of(vol);
    if (lvl > 63) lvl = 63;
    wr(p, bank, 0x40 + ca, (vo[V_CAR_KSL] & 0xC0) | lvl);

    if (vo[V_FEEDBACK] & 1) {
        lvl = (vo[V_MOD_LEVEL] & 0x3F) + atten_of(vol);
        if (lvl > 63) lvl = 63;
    } else {
        lvl = vo[V_MOD_LEVEL] & 0x3F;
    }
    wr(p, bank, 0x40 + mo, (vo[V_MOD_KSL] & 0xC0) | lvl);
}

static void voice_note(struct mus_player *p, int v, int instr, int note, int bend, int which)
{
    int bank = v / 9, n = v % 9;
    const uint8_t *rec;
    int semis, block, f, cents;

    if (!p->gen_ok)
        return;
    rec = p->gen + instr * GEN_REC;
    if (rec[0] & 1)                     /* a fixed-pitch instrument: a drum */
        note = rec[3];
    semis = note;
    {   /* The bank's own transposition, in whole semitones: the values in
           Doom's GENMIDI are -24, -12, -7 and 0, and losing them puts a
           quarter of the instruments an octave or two too high. */
        const uint8_t *vo = rec + 4 + which * 16;
        int16_t off = (int16_t)(vo[V_OFF_LO] | (vo[V_OFF_HI] << 8));
        semis += off;
    }
    if (semis < 0) semis = 0;
    if (semis > 95) semis = 95;

    block = semis / 12;
    f = note_fnum[semis % 12];
    /* the pitch wheel, 128 is centre, +-2 semitones over its range */
    cents = bend - 128;
    if (cents)
        f += (f * cents) / 1600;
    /* the second voice sits a hair off the first: that beating is the point */
    if (which)
        f += (f * ((int)rec[2] - 128)) / 2048;
    if (f > 1023) f = 1023;
    if (block > 7) block = 7;

    wr(p, bank, 0xA0 + n, f & 0xFF);
    wr(p, bank, 0xB0 + n, 0x20 | (block << 2) | ((f >> 8) & 3));
    (void)instr;
}

static int voice_take(struct mus_player *p, int ch)
{
    int i, best = 0;
    uint32_t oldest = 0xFFFFFFFFu;
    for (i = 0; i < p->nvoices; i++)
        if (!p->voices[i].on) {
            p->voices[i].age = p->clock;
            p->voices[i].ch = ch;
            return i;
        }
    for (i = 0; i < p->nvoices; i++)    /* nothing free: the one sounding longest */
        if (p->voices[i].age < oldest) {
            oldest = p->voices[i].age;
            best = i;
        }
    voice_silence(p, best);
    p->voices[best].age = p->clock;
    p->voices[best].ch = ch;
    return best;
}

/* ---- the score ------------------------------------------------------------- */

int mus_load(struct mus_player *p, struct opl3 *chip, const void *mus, int len)
{
    const uint8_t *m = (const uint8_t *)mus;
    int i, score_len, score_start;

    p->chip = chip;
    p->done = 1;
    if (!m || len < 16 || m[0] != 'M' || m[1] != 'U' || m[2] != 'S' || m[3] != 0x1A)
        return -1;
    score_len = m[4] | (m[5] << 8);
    score_start = m[6] | (m[7] << 8);
    p->channels = m[8] | (m[9] << 8);
    if (score_start + score_len > len)
        return -1;
    p->score = m + score_start;
    p->pos = p->score;
    p->end = p->score + score_len;

    /* OPL3's second bank doubles the voices; without it there are nine. */
    opl3_write(chip, 0x105, 1);
    opl3_write(chip, 0x104, 0);
    opl3_write(chip, 0x001, 0x20);
    opl3_write(chip, 0x0BD, 0);
    p->nvoices = MUS_VOICES;

    for (i = 0; i < 16; i++) {
        p->ch_instr[i] = 0;
        p->ch_vol[i] = 100;
        p->ch_bend[i] = 128;
    }
    p->ch_instr[15] = 128;              /* the percussion channel */
    p->ch_mask = 0xFFFF;
    mus_all_off(p);
    p->clock = 0;
    p->wait = 0;
    p->done = 0;
    return 0;
}

static void note_off(struct mus_player *p, int ch, int note)
{
    int i;
    /* A double-voice instrument took two: release both, not the first found. */
    for (i = 0; i < p->nvoices; i++)
        if (p->voices[i].on && p->voices[i].ch == ch && p->voices[i].note == note) {
            int bank = i / 9, n = i % 9;
            /* keep the pitch, drop the key: the release still sounds */
            wr(p, bank, 0xB0 + n, p->chip->regs[(bank << 8) | (0xB0 + n)] & ~0x20);
            p->voices[i].on = 0;
        }
}

static void note_on(struct mus_player *p, int ch, int note, int vol)
{
    int instr = ch == 15 ? 128 + (note - 35) : p->ch_instr[ch];
    int level = (vol * p->ch_vol[ch]) / 127;
    int voices, k;
    if (ch == 15) {
        if (note < 35 || note > 81)
            return;
    }
    if (instr < 0 || instr >= GEN_COUNT)
        return;
    if (!((p->ch_mask >> ch) & 1))
        return;
    /* Flag bit 2 means the instrument is two voices layered a little apart;
       a third of Doom's bank is built that way, and one of them alone is a
       recognisably different sound. */
    voices = (p->gen[instr * GEN_REC] & 4) ? 2 : 1;
    for (k = 0; k < voices; k++) {
        int v = voice_take(p, ch);
        p->voices[v].note = note;
        p->voices[v].instr = instr;
        p->voices[v].on = 1;
        voice_program(p, v, instr, level, k);
        voice_note(p, v, instr, note, p->ch_bend[ch], k);
    }
}

int mus_tick(struct mus_player *p)
{
    if (p->done)
        return 0;
    p->clock++;
    if (p->wait > 0) {
        p->wait--;
        return 1;
    }
    for (;;) {
        int ev, type, ch, last;
        if (p->pos >= p->end) {
            if (!p->loop) { p->done = 1; mus_all_off(p); return 0; }
            p->pos = p->score;
            continue;
        }
        ev = *p->pos++;
        last = ev & 0x80;
        type = (ev >> 4) & 7;
        ch = ev & 0x0F;

        switch (type) {
        case 0:                                     /* let a note go */
            note_off(p, ch, *p->pos++ & 0x7F);
            break;
        case 1: {                                   /* strike one */
            int n = *p->pos++;
            int vol = p->ch_vol[ch];
            if (n & 0x80) { vol = *p->pos++ & 0x7F; p->ch_vol[ch] = (uint8_t)vol; }
            note_on(p, ch, n & 0x7F, vol);
            break;
        }
        case 2:                                     /* bend the pitch */
            p->ch_bend[ch] = *p->pos++;
            break;
        case 3:                                     /* all notes off, and friends */
            if ((*p->pos++ & 0x7F) >= 10)
                mus_all_off(p);
            break;
        case 4: {                                   /* a controller */
            int num = *p->pos++ & 0x7F;
            int val = *p->pos++ & 0x7F;
            if (num == 0)      p->ch_instr[ch] = (uint8_t)val;
            else if (num == 3) p->ch_vol[ch] = (uint8_t)val;
            break;
        }
        case 6:                                     /* the end */
            if (!p->loop) { p->done = 1; mus_all_off(p); return 0; }
            p->pos = p->score;
            break;
        case 5:                                     /* a bar line: nothing to do */
            break;
        default:
            p->pos += 2;
            break;
        }

        if (last) {                                 /* a delay in ticks follows */
            uint32_t d = 0;
            uint8_t b;
            do {
                if (p->pos >= p->end) { p->done = 1; mus_all_off(p); return 0; }
                b = *p->pos++;
                d = (d << 7) | (b & 0x7F);
            } while (b & 0x80);
            p->wait = (int)d;
            return 1;
        }
    }
}
