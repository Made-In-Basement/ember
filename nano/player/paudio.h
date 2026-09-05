#ifndef PAUDIO_H
#define PAUDIO_H

int  audio_start(void);                 /* claim the sound stream */
void audio_stop(void);
int  audio_open(const char *path);      /* 0 if the file can be played */
void audio_close(void);
int  audio_pump(void);                  /* 0 when the track has ended */
void audio_silence(void);
void audio_decay_peaks(void);

extern int  audio_bitrate, audio_rate, audio_channels;
extern int  audio_peak_l, audio_peak_r;
extern int  audio_volume;               /* 0..10 */
extern long audio_seconds, audio_total_seconds;

#endif
