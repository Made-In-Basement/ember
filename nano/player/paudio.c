/* paudio.c - decoding and playback for the player.
 *
 * The kernel hands out a looping 44.1 kHz stereo ring buffer and tells us
 * where the controller has reached in it.  Everything here comes down to
 * keeping that ring a little ahead of the hardware: decode a frame, convert
 * it to the ring's rate, write it, repeat.
 */
#include <nanolibc.h>
#include "nano.h"
#include "paudio.h"

#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#define IN_BUF      (32 * 1024)         /* compressed bytes held at a time */
#define LEAD_GAP    1024                /* how close behind the DMA we may write */
static unsigned lead_frames;            /* how far ahead of the DMA we keep: the ring, nearly */

static struct pcm_info pcm;
static volatile int16_t *ring;
static volatile uint32_t *lpib;
static unsigned ring_frames, wpos;
static int audio_ready;

static int fd = -1;
static int is_mp3;
static long file_size, file_pos;

static uint8_t inbuf[IN_BUF];
static int in_len, in_pos;

static mp3dec_t mp3;
static int16_t decoded[MINIMP3_MAX_SAMPLES_PER_FRAME];
static int dec_frames, dec_pos;         /* frames (a left/right pair) */
static int dec_rate = 44100, dec_channels = 2;

static uint32_t rs_phase, rs_step;      /* 16.16 resampling position */
static int16_t last_l, last_r;

int audio_bitrate, audio_rate, audio_channels;
int audio_peak_l, audio_peak_r;
int audio_volume = 80;                  /* 0..100 */
long audio_seconds, audio_total_seconds;

/* the most recent samples, for whatever wants to draw the sound */
int16_t audio_scope[SCOPE_LEN];
volatile int audio_scope_pos;

/* ---------------------------------------------------------------- input */
static int refill(void)
{
    int keep = in_len - in_pos, got;
    if (keep > 0 && in_pos > 0) memmove(inbuf, inbuf + in_pos, keep);
    else if (keep < 0) keep = 0;
    in_pos = 0;
    in_len = keep;
    got = sys_read(fd, inbuf + in_len, IN_BUF - in_len);
    if (got > 0) {
        in_len += got;
        file_pos += got;
    }
    return in_len - in_pos;
}

/* ---------------------------------------------------------------- WAV */
static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static int wav_bits = 16;
static long wav_left, wav_data_start, wav_data_bytes;

static int wav_open(void)
{
    uint8_t h[12], c[8];
    if (sys_read(fd, h, 12) != 12) return -1;
    if (memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return -1;
    for (;;) {
        uint32_t len;
        if (sys_read(fd, c, 8) != 8) return -1;
        len = rd32(c + 4);
        if (!memcmp(c, "fmt ", 4)) {
            uint8_t f[16];
            if (sys_read(fd, f, 16) != 16) return -1;
            audio_channels = dec_channels = rd16(f + 2);
            audio_rate = dec_rate = (int)rd32(f + 4);
            wav_bits = rd16(f + 14);
            if (len > 16) sys_lseek(fd, len - 16, SEEK_CUR);
        } else if (!memcmp(c, "data", 4)) {
            wav_left = (long)len;
            wav_data_bytes = (long)len;
            wav_data_start = sys_lseek(fd, 0, SEEK_CUR);
            break;
        } else {
            sys_lseek(fd, (long)len, SEEK_CUR);
        }
    }
    if (dec_channels < 1 || dec_channels > 2) return -1;
    if (wav_bits != 8 && wav_bits != 16) return -1;
    audio_bitrate = dec_rate * dec_channels * wav_bits / 1000;
    audio_total_seconds = wav_left / (dec_rate * dec_channels * (wav_bits / 8));
    in_len = in_pos = 0;
    return 0;
}

/* one buffer of WAV samples into `decoded` */
static int wav_next(void)
{
    int want = 1152 * dec_channels * (wav_bits / 8), got, i, n;
    if (wav_left <= 0) return 0;
    if (want > (int)sizeof inbuf) want = sizeof inbuf;
    if (want > wav_left) want = (int)wav_left;
    got = sys_read(fd, inbuf, want);
    if (got <= 0) return 0;
    wav_left -= got;
    file_pos += got;
    n = got / (dec_channels * (wav_bits / 8));
    for (i = 0; i < n * dec_channels; i++)
        decoded[i] = wav_bits == 8 ? (int16_t)((inbuf[i] - 128) << 8)
                                   : (int16_t)rd16(inbuf + i * 2);
    return n;
}

/* ---------------------------------------------------------------- MP3 */
static int mp3_next(void)
{
    mp3dec_frame_info_t info;
    int n;
    for (;;) {
        if (in_len - in_pos < 4 * 1024 && refill() <= 0 && in_len - in_pos <= 0)
            return 0;
        n = mp3dec_decode_frame(&mp3, inbuf + in_pos, in_len - in_pos, decoded, &info);
        in_pos += info.frame_bytes;
        if (info.frame_bytes == 0) {            /* needs more input */
            if (refill() <= 0) return 0;
            continue;
        }
        if (n > 0) {
            dec_rate = info.hz;
            dec_channels = info.channels;
            audio_rate = info.hz;
            audio_channels = info.channels;
            audio_bitrate = info.bitrate_kbps;
            if (audio_total_seconds <= 0 && info.bitrate_kbps > 0)
                audio_total_seconds = file_size / (info.bitrate_kbps * 125);
            return n;
        }
    }
}

static int decode_next(void)
{
    int n = is_mp3 ? mp3_next() : wav_next();
    dec_frames = n;
    dec_pos = 0;
    if (n > 0 && dec_rate > 0)
        rs_step = (uint32_t)(((uint64_t)dec_rate << 16) / pcm.rate);
    return n;
}

/* ---------------------------------------------------------------- output */
int audio_start(void)
{
    if (sys_pcm_start(&pcm) < 0) return -1;
    ring = (volatile int16_t *)pcm.ring_phys;
    ring_frames = pcm.ring_size / 4;
    lead_frames = ring_frames > LEAD_GAP * 2 ? ring_frames - LEAD_GAP : ring_frames / 2;
    lpib = (volatile uint32_t *)pcm.lpib_phys;
    wpos = lead_frames % ring_frames;
    audio_ready = 1;
    return 0;
}

void audio_stop(void)
{
    if (audio_ready) sys_pcm_stop();
    audio_ready = 0;
}

int audio_open(const char *path)
{
    const char *dot = strrchr(path, '.');
    audio_close();
    fd = sys_open(path);
    if (fd < 0) return -1;
    file_size = sys_lseek(fd, 0, SEEK_END);
    sys_lseek(fd, 0, SEEK_SET);
    file_pos = 0;
    in_len = in_pos = dec_frames = dec_pos = 0;
    rs_phase = 0;
    last_l = last_r = 0;
    audio_seconds = audio_total_seconds = 0;
    audio_bitrate = 0;
    is_mp3 = dot && (dot[1] == 'M' || dot[1] == 'm');
    if (is_mp3) {
        mp3dec_init(&mp3);
        audio_rate = 44100;
        audio_channels = 2;
    } else if (wav_open() != 0) {
        sys_close(fd);
        fd = -1;
        return -1;
    }
    rs_step = 0x10000;
    return 0;
}

void audio_close(void)
{
    if (fd >= 0) sys_close(fd);
    fd = -1;
}

/* silence, so a pause or the end of a track does not loop the last buffer */
void audio_silence(void)
{
    unsigned i;
    if (!audio_ready) return;
    for (i = 0; i < ring_frames * 2; i++) ring[i] = 0;
    cache_flush((const void *)ring, pcm.ring_size);
}

/* The end of a sound.  The pump keeps nearly a whole ring ahead of the
   hardware, so when the file runs out a third of a second of it has not
   been heard yet - and stopping the stream there both cut that off and
   left the last buffer in place, which is what the hardware (and QEMU's
   audio output) went on playing round and round until the next sound
   replaced it: the tail of the start-up chime, quietly, for ever.

   So the ring is topped up with silence instead, until the hardware has
   played everything that was real and gone a whole lap past it.  By then
   every sample in the ring is zero, and stopping it is silent. */
static unsigned drain_moved, drain_last_hw;

void audio_drain_begin(void)
{
    if (!audio_ready || !ring_frames) return;
    drain_last_hw = (*lpib / 4) % ring_frames;
    drain_moved = 0;
}

int audio_drain(void)
{
    unsigned hw, ahead, room, w;
    if (!audio_ready || !ring_frames) return 0;
    hw = (*lpib / 4) % ring_frames;
    drain_moved += (hw - drain_last_hw + ring_frames) % ring_frames;
    drain_last_hw = hw;
    if (drain_moved >= ring_frames + lead_frames) return 0;
    ahead = (wpos - hw + ring_frames) % ring_frames;
    if (ahead > ring_frames - 64) ahead = 0;    /* the hardware caught up */
    if (ahead >= lead_frames) return 1;
    room = lead_frames - ahead;
    w = wpos;
    while (room--) {
        ring[w * 2] = 0;
        ring[w * 2 + 1] = 0;
        if (++w >= ring_frames) w = 0;
    }
    if (w >= wpos) cache_flush((const void *)(ring + wpos * 2), (w - wpos) * 4);
    else {
        cache_flush((const void *)(ring + wpos * 2), (ring_frames - wpos) * 4);
        cache_flush((const void *)ring, w * 4);
    }
    wpos = w;
    return 1;
}

/* Keep the ring fed.  Returns 0 when the track has finished. */
/* how far ahead of the chip we are, as a percentage of the ring */
int audio_ring_fill(void)
{
    unsigned hw, ahead;
    if (!audio_ready || !ring_frames) return 0;
    hw = (*lpib / 4) % ring_frames;
    ahead = (wpos - hw + ring_frames) % ring_frames;
    return (int)(ahead * 100 / ring_frames);
}

int audio_pump(void)
{
    unsigned hw, ahead, room, w;
    int pl = 0, pr = 0, alive = 1;
    if (!audio_ready || fd < 0) return 0;
    hw = (*lpib / 4) % ring_frames;
    ahead = (wpos - hw + ring_frames) % ring_frames;
    if (ahead > ring_frames - 64) ahead = 0;    /* the hardware caught up */
    if (ahead >= lead_frames) return 1;
    room = lead_frames - ahead;
    w = wpos;
    while (room--) {
        int l, r;
        if (dec_pos >= dec_frames) {
            if (decode_next() <= 0) { alive = 0; break; }
        }
        {
            const int16_t *s = decoded + dec_pos * dec_channels;
            l = s[0];
            r = dec_channels > 1 ? s[1] : s[0];
        }
        last_l = (int16_t)l;
        last_r = (int16_t)r;
        audio_scope[audio_scope_pos] = (int16_t)((l + r) / 2);
        audio_scope_pos = (audio_scope_pos + 1) & (SCOPE_LEN - 1);
        l = l * audio_volume / 100;
        r = r * audio_volume / 100;
        ring[w * 2] = (int16_t)l;
        ring[w * 2 + 1] = (int16_t)r;
        if (l > pl) pl = l; else if (-l > pl) pl = -l;
        if (r > pr) pr = r; else if (-r > pr) pr = -r;
        if (++w >= ring_frames) w = 0;
        rs_phase += rs_step;                    /* advance the source */
        dec_pos += rs_phase >> 16;
        rs_phase &= 0xFFFF;
    }
    if (w >= wpos) cache_flush((const void *)(ring + wpos * 2), (w - wpos) * 4);
    else {
        cache_flush((const void *)(ring + wpos * 2), (ring_frames - wpos) * 4);
        cache_flush((const void *)ring, w * 4);
    }
    wpos = w;
    if (pl > audio_peak_l) audio_peak_l = pl;
    if (pr > audio_peak_r) audio_peak_r = pr;
    audio_seconds = file_size > 0 && audio_total_seconds > 0
                  ? file_pos * audio_total_seconds / file_size : 0;
    return alive;
}

/* Jump to a position, given in thousandths of the file.  An MP3 is found
   again by letting the decoder resynchronise on the next frame header,
   which is what makes seeking in one cheap; a WAV is exact. */
void audio_seek_permille(int p)
{
    long target;
    if (fd < 0 || file_size <= 0) return;
    if (p < 0) p = 0;
    if (p > 1000) p = 1000;
    if (is_mp3) {
        target = file_size / 1000 * p;
        sys_lseek(fd, target, SEEK_SET);
        file_pos = target;
        mp3dec_init(&mp3);
    } else {
        long frame = dec_channels * (wav_bits / 8);
        target = wav_data_bytes / 1000 * p;
        target -= target % frame;
        wav_left = wav_data_bytes - target;
        sys_lseek(fd, wav_data_start + target, SEEK_SET);
        file_pos = wav_data_start + target;
    }
    in_len = in_pos = 0;
    dec_frames = dec_pos = 0;
    rs_phase = 0;
}

void audio_decay_peaks(void)
{
    audio_peak_l -= audio_peak_l >> 2;
    audio_peak_r -= audio_peak_r >> 2;
}
