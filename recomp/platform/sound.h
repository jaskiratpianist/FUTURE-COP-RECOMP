/* Sound interface between dsound.c and gdi_misc.c (PlaySoundA) */
#ifndef FCOP_SOUND_H
#define FCOP_SOUND_H

#include "host.h"

/* Win32 PlaySoundA implementation */
u32 sound_playsound(u32 pszSound, u32 hmod, u32 flags);

#endif
