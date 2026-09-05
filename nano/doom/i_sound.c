/* Doom for Ember: sound effects mixed into the HD Audio DMA ring.
   The kernel starts a looping 44.1 kHz 16-bit stereo stream; this file keeps
   the ring filled a little ahead of the hardware's read position. */
#include <nanolibc.h>
#include "nano.h"
#include "z_zone.h"
#include "i_system.h"
#include "i_sound.h"
#include "m_argv.h"
#include "m_misc.h"
#include "w_wad.h"
#include "doomdef.h"
#include "doomstat.h"
#include "sounds.h"
#include "steptab.h"

#define NUM_CHANNELS 8
#define LEAD_FRAMES  2048               /* ~46 ms queued ahead of the DMA */

typedef struct {
    const unsigned char *data;
    unsigned len;                       /* samples */
    unsigned pos;                       /* 16.16 sample position */
    unsigned step;                      /* 16.16 samples per output frame */
    int lvol, rvol;
    int uniq, start;
} chan_t;

static struct pcm_info pcm;
static int sound_ok;
static volatile int16_t *ring;
static unsigned ring_frames;
static volatile uint32_t *lpib;
static unsigned wpos;
static chan_t ch[NUM_CHANNELS];
static int uniq_counter;
static const unsigned char *sfx_data[NUMSFX];
static unsigned sfx_len[NUMSFX], sfx_rate[NUMSFX];
static int snd_sfxvol = 15, snd_musvol = 0;

/* debug counters at a fixed low address (dumped with the QEMU harness) */
volatile uint32_t snd_dbg[16];
#define dbg snd_dbg
enum { DBG_MAGIC, DBG_UPDATES, DBG_STARTS, DBG_FRAMES, DBG_HW, DBG_WPOS, DBG_OK,
       DBG_NONZERO, DBG_LEN0, DBG_RATE0, DBG_ACTIVE, DBG_LASTVOL, DBG_LASTID };

static void load_sfx(int i)
{
    char name[16];
    int lump, len;
    const unsigned char *raw;
    sprintf(name, "ds%s", S_sfx[i].name);
    lump = W_CheckNumForName(name);
    if (lump < 0) lump = W_GetNumForName("dspistol");
    len = W_LumpLength(lump);
    raw = W_CacheLumpNum(lump, PU_STATIC);
    if (len < 16) { sfx_data[i] = 0; return; }
    sfx_rate[i] = raw[2] | (raw[3] << 8);
    if (sfx_rate[i] < 4000 || sfx_rate[i] > 48000) sfx_rate[i] = 11025;
    sfx_len[i] = (unsigned)len - 8;
    sfx_data[i] = raw + 8;
    if (i == 1) { dbg[DBG_LEN0] = sfx_len[i]; dbg[DBG_RATE0] = sfx_rate[i]; }
    S_sfx[i].data = (void *)sfx_data[i];
}

void I_InitSound(void)
{
    int i, rc;
    dbg[DBG_MAGIC] = 0x5EBD0000;                /* I_InitSound entered */
    rc = M_CheckParm("-nosound") ? 0 : sys_pcm_start(&pcm);
    if (rc < 0 || M_CheckParm("-nosound")) {
        dbg[DBG_MAGIC] = 0x5EBDFFFF;            /* stream start failed */
        sys_logf("NDOOM: HDA stream start failed (driver status %d)", -rc);
        printf("I_InitSound: no HD Audio stream, sound effects disabled\n");
        sound_ok = 0;
        return;
    }
    ring = (volatile int16_t *)pcm.ring_phys;
    ring_frames = pcm.ring_size / 4;
    lpib = (volatile uint32_t *)pcm.lpib_phys;
    wpos = LEAD_FRAMES;
    sound_ok = 1;
    dbg[DBG_MAGIC] = 0x5EBD0001;
    dbg[DBG_OK] = 1;
    printf("I_InitSound: HD Audio ring at %x, %u frames at %u Hz\n",
           pcm.ring_phys, ring_frames, pcm.rate);
    sys_logf("NDOOM: HDA ring %08X, %u frames, LPIB at %08X", pcm.ring_phys,
             ring_frames, pcm.lpib_phys);
    for (i = 1; i < NUMSFX; i++) {
        if (!S_sfx[i].link) load_sfx(i);
    }
    for (i = 1; i < NUMSFX; i++) {
        if (S_sfx[i].link) {
            int j = (int)(S_sfx[i].link - S_sfx);
            sfx_data[i] = sfx_data[j]; sfx_len[i] = sfx_len[j]; sfx_rate[i] = sfx_rate[j];
        }
    }
}

void I_ShutdownSound(void)
{
    if (sound_ok) sys_pcm_stop();
    sound_ok = 0;
}

void I_SetChannels(void)
{
}

void I_SetSfxVolume(int volume) { snd_sfxvol = volume; }
void I_SetMusicVolume(int volume) { snd_musvol = volume; }

int I_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char name[16];
    sprintf(name, "ds%s", sfx->name);
    return W_GetNumForName(name);
}

static void set_params(chan_t *c, int vol, int sep, int pitch, int id)
{
    int l, r;
    vol *= 8;                               /* Doom's 0..15 -> 0..120 */
    if (vol > 127) vol = 127;
    if (vol < 0) vol = 0;
    if (pitch < 0) pitch = 0;
    if (pitch > 255) pitch = 255;
    sep += 1;
    l = vol - ((vol * sep * sep) >> 16);
    sep -= 257;
    r = vol - ((vol * sep * sep) >> 16);
    c->lvol = l < 0 ? 0 : l;
    c->rvol = r < 0 ? 0 : r;
    c->step = (unsigned)(((uint64_t)sfx_rate[id] * 65536 / pcm.rate) * steptable[pitch] >> 16);
}

int I_StartSound(int id, int vol, int sep, int pitch, int priority)
{
    int i, slot = -1, oldest = 0x7fffffff;
    chan_t *c;
    (void)priority;
    dbg[DBG_STARTS]++;
    dbg[DBG_LASTVOL] = vol;
    dbg[DBG_LASTID] = id;
    if (!sound_ok || id <= 0 || id >= NUMSFX || !sfx_data[id]) return 0;
    for (i = 0; i < NUM_CHANNELS; i++) {
        if (!ch[i].data) { slot = i; break; }
        if (ch[i].start < oldest) { oldest = ch[i].start; slot = i; }
    }
    c = &ch[slot];
    c->data = 0;
    set_params(c, vol, sep, pitch, id);
    c->len = sfx_len[id];
    c->pos = 0;
    c->uniq = ++uniq_counter;
    c->start = I_GetTime();
    c->data = sfx_data[id];
    return (c->uniq << 4) | slot;
}

static chan_t *lookup(int handle)
{
    chan_t *c = &ch[handle & 15];
    return (c->uniq == (handle >> 4) && c->data) ? c : 0;
}

void I_StopSound(int handle)
{
    chan_t *c = lookup(handle);
    if (c) c->data = 0;
}

int I_SoundIsPlaying(int handle)
{
    return lookup(handle) != 0;
}

void I_UpdateSoundParams(int handle, int vol, int sep, int pitch)
{
    chan_t *c = lookup(handle);
    unsigned step;
    if (!c) return;
    step = c->step;
    set_params(c, vol, sep, pitch, 0);
    /* keep the sample-rate part of the step: only pitch may change */
    c->step = (unsigned)((uint64_t)step * steptable[pitch < 0 ? 0 : pitch > 255 ? 255 : pitch] >> 16);
    (void)step;
}

void I_UpdateSound(void)
{
    unsigned hw, ahead, n, w;
    dbg[DBG_UPDATES]++;
    if (!sound_ok) return;
    hw = (*lpib / 4) % ring_frames;
    dbg[DBG_HW] = hw;
    dbg[DBG_WPOS] = wpos;
    { int i, a = 0; for (i = 0; i < NUM_CHANNELS; i++) if (ch[i].data) a++; dbg[DBG_ACTIVE] = a; }
    ahead = (wpos - hw + ring_frames) % ring_frames;
    if (ahead > ring_frames / 2) {              /* the hardware overtook us */
        wpos = (hw + LEAD_FRAMES / 2) % ring_frames;
        ahead = LEAD_FRAMES / 2;
    }
    if (ahead >= LEAD_FRAMES) return;
    n = LEAD_FRAMES - ahead;
    w = wpos;
    while (n--) {
        int l = 0, r = 0, i;
        for (i = 0; i < NUM_CHANNELS; i++) {
            chan_t *c = &ch[i];
            int s;
            if (!c->data) continue;
            s = (int)c->data[c->pos >> 16] - 128;
            l += s * c->lvol * 2;
            r += s * c->rvol * 2;
            c->pos += c->step;
            if ((c->pos >> 16) >= c->len) c->data = 0;
        }
        if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
        if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
        ring[w * 2] = (int16_t)l;
        ring[w * 2 + 1] = (int16_t)r;
        if (l | r) dbg[DBG_NONZERO]++;
        dbg[DBG_FRAMES]++;
        if (++w >= ring_frames) w = 0;
    }
    if (w >= wpos) cache_flush((const void *)(ring + wpos * 2), (w - wpos) * 4);
    else {
        cache_flush((const void *)(ring + wpos * 2), (ring_frames - wpos) * 4);
        cache_flush((const void *)ring, w * 4);
    }
    wpos = w;
}

void I_SubmitSound(void)
{
}

/* ---- music: not available (no OPL synthesizer yet) ---- */
void I_InitMusic(void) {}
void I_ShutdownMusic(void) {}
void I_PauseSong(int handle) { (void)handle; }
void I_ResumeSong(int handle) { (void)handle; }
int  I_RegisterSong(void *data) { (void)data; return 1; }
void I_PlaySong(int handle, int looping) { (void)handle; (void)looping; }
void I_StopSong(int handle) { (void)handle; }
void I_UnRegisterSong(int handle) { (void)handle; }
int  I_QrySongPlaying(int handle) { (void)handle; return 0; }
