#ifndef PAUDIO_H
#define PAUDIO_H
#include <stdint.h>

#define SCOPE_LEN 1024                  /* a power of two: the ring wraps */

int  audio_start(void);                 /* claim the sound stream */
void audio_stop(void);
int  audio_open(const char *path);      /* 0 if the file can be played */
void audio_close(void);
int  audio_pump(void);                  /* 0 when the track has ended */
void audio_silence(void);
void audio_drain_begin(void);           /* a sound has ended: play its tail out */
int  audio_drain(void);                 /* silence behind it; 0 once it is safe to stop */
void audio_decay_peaks(void);
void audio_seek_permille(int p);        /* 0..1000 through the file */

extern int16_t audio_scope[SCOPE_LEN];  /* the sound as it goes out */
extern volatile int audio_scope_pos;

extern int  audio_bitrate, audio_rate, audio_channels;
extern int  audio_peak_l, audio_peak_r;
int  audio_ring_fill(void);            /* percent of the ring ahead of the chip */
extern int  audio_volume;               /* 0..10 */
extern long audio_seconds, audio_total_seconds;

#endif
