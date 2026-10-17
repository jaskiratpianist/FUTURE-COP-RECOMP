/* Sound interface between dsound.c and gdi_misc.c (PlaySoundA) */
#ifndef FCOP_SOUND_H
#define FCOP_SOUND_H

#include "host.h"

/* Win32 PlaySoundA implementation */
u32 sound_playsound(u32 pszSound, u32 hmod, u32 flags);

/* Streaming audio API for video playback */
int    sound_stream_open(int rate, int channels);
int    sound_stream_write(const s16 *samples, int frames);
void   sound_stream_close(void);
double sound_stream_get_time(void);
int    sound_stream_buffered_frames(void);
void   sound_stream_set_volume(int vol_db_hundredths);

#endif
