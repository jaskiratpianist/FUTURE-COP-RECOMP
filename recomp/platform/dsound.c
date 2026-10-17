/* dsound.dll - DirectSound implementation for recompiled Future Cop */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "host.h"

/* HRESULT return codes */
#define DS_OK                           0x00000000u
#define S_OK                            0x00000000u
#define S_FALSE                         0x00000001u
#define E_NOTIMPL                       0x80004001u
#define E_NOINTERFACE                   0x80004002u
#define E_POINTER                       0x80004003u
#define E_FAIL                          0x80004005u
#define E_OUTOFMEMORY                   0x8007000eu
#define E_INVALIDARG                    0x80070057u

#define DSERR_ALLOCATED                 0x8878000au
#define DSERR_CONTROLUNAVAIL            0x8878001eu
#define DSERR_INVALIDPARAM              0x80070057u
#define DSERR_INVALIDCALL               0x88780032u
#define DSERR_GENERIC                   0x80004005u
#define DSERR_PRIOLEVELNEEDED           0x88780046u
#define DSERR_OUTOFMEMORY               0x8007000eu
#define DSERR_BADFORMAT                 0x88780064u
#define DSERR_UNSUPPORTED               0x80004001u
#define DSERR_NODRIVER                  0x88780078u
#define DSERR_ALREADYINITIALIZED        0x88780082u
#define DSERR_NOAGGREGATION             0x8878008cu
#define DSERR_BUFFERLOST                0x88780096u
#define DSERR_OTHERAPPHASPRIO           0x887800a0u
#define DSERR_UNINITIALIZED             0x887800aau

/* DSCAPS flags */
#define DSCAPS_PRIMARYMONO              0x00000001u
#define DSCAPS_PRIMARYSTEREO            0x00000002u
#define DSCAPS_PRIMARY8BIT              0x00000004u
#define DSCAPS_PRIMARY16BIT             0x00000008u
#define DSCAPS_CONTINUOUSRATE           0x00000010u
#define DSCAPS_EMULATEDDRIVER           0x00000020u
#define DSCAPS_CERTIFIED                0x00000040u
#define DSCAPS_SECONDARYMONO            0x00000100u
#define DSCAPS_SECONDARYSTEREO          0x00000200u
#define DSCAPS_SECONDARY8BIT            0x00000400u
#define DSCAPS_SECONDARY16BIT           0x00000800u

/* DSBCAPS flags */
#define DSBCAPS_PRIMARYBUFFER           0x00000001u
#define DSBCAPS_STATIC                  0x00000002u
#define DSBCAPS_LOCHARDWARE             0x00000004u
#define DSBCAPS_LOCSOFTWARE             0x00000008u
#define DSBCAPS_CTRL3D                  0x00000010u
#define DSBCAPS_CTRLFREQUENCY           0x00000020u
#define DSBCAPS_CTRLPAN                 0x00000040u
#define DSBCAPS_CTRLVOLUME              0x00000080u
#define DSBCAPS_CTRLPOSITIONNOTIFY      0x00000100u
#define DSBCAPS_CTRLDEFAULT             0x000000e0u
#define DSBCAPS_CTRLALL                 0x000001f0u

/* Play / Status flags */
#define DSBPLAY_LOOPING                 0x00000001u
#define DSBSTATUS_PLAYING               0x00000001u
#define DSBSTATUS_BUFFERLOST            0x00000002u
#define DSBSTATUS_LOOPING               0x00000004u

/* Lock flags */
#define DSBLOCK_FROMWRITECURSOR         0x00000001u
#define DSBLOCK_ENTIREBUFFER            0x00000002u

/* GUID structure */
struct Guid {
    u32 data1;
    u16 data2;
    u16 data3;
    u8  data4[8];
};

static const struct Guid IID_IUnknown = {
    0x00000000, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 }
};
static const struct Guid IID_IDirectSound = {
    0x279afa83, 0x4981, 0x11ce, { 0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60 }
};
static const struct Guid IID_IDirectSoundBuffer = {
    0x279afa85, 0x4981, 0x11ce, { 0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60 }
};

static void read_guid(u32 p, struct Guid *g)
{
    g->data1 = rd32(p);
    g->data2 = rd16(p + 4);
    g->data3 = rd16(p + 6);
    for (int i = 0; i < 8; i++) g->data4[i] = rd8(p + 8 + i);
}

static int guid_equal(const struct Guid *a, const struct Guid *b)
{
    return memcmp(a, b, sizeof(struct Guid)) == 0;
}

/* Internal representation of WAVEFORMATEX */
struct WaveFormat {
    u16 wFormatTag;
    u16 nChannels;
    u32 nSamplesPerSec;
    u32 nAvgBytesPerSec;
    u16 nBlockAlign;
    u16 wBitsPerSample;
};

#define MAX_DSOUND_BUFFERS 256

struct DsHost;

struct DsbHost {
    struct DsHost *ds;
    u32 guest_obj;
    int is_primary;
    u32 ref_count;
    u32 flags;
    u32 size_bytes;
    u32 guest_buf;
    int *shared_mem_ref;
    struct WaveFormat format;
    u32 current_frequency;
    s32 volume;     /* hundredths of dB, 0 = full, -10000 = silent */
    s32 pan;        /* hundredths of dB, -10000 = full left, +10000 = full right */
    u32 status;     /* DSBSTATUS_* */
    double play_cursor_bytes;
};

struct DsHost {
    u32 ref_count;
    u32 coop_level;
    u32 hwnd;
    u32 speaker_config;
    SDL_AudioDeviceID audio_dev;
    pthread_mutex_t fallback_lock;
    struct DsbHost *primary_buf;
    struct DsbHost *secondary_bufs[MAX_DSOUND_BUFFERS];
    int num_secondary_bufs;
};

static void ds_lock(struct DsHost *ds)
{
    if (ds->audio_dev) SDL_LockAudioDevice(ds->audio_dev);
    else pthread_mutex_lock(&ds->fallback_lock);
}

static void ds_unlock(struct DsHost *ds)
{
    if (ds->audio_dev) SDL_UnlockAudioDevice(ds->audio_dev);
    else pthread_mutex_unlock(&ds->fallback_lock);
}

static inline void read_source_frame(u32 guest_buf, u32 buf_size, u32 offset,
                                     int bits, int channels, float *out_l, float *out_r)
{
    if (offset >= buf_size) offset %= buf_size;
    u32 addr = guest_buf + offset;

    if (bits == 8) {
        if (channels == 1) {
            u8 val = rd8(addr);
            float s = (float)((int)val - 128) * 256.0f;
            *out_l = s;
            *out_r = s;
        } else {
            u8 l = rd8(addr);
            u8 r = (offset + 1 < buf_size) ? rd8(addr + 1) : rd8(guest_buf);
            *out_l = (float)((int)l - 128) * 256.0f;
            *out_r = (float)((int)r - 128) * 256.0f;
        }
    } else { /* 16-bit */
        if (channels == 1) {
            s16 val = (s16)rd16(addr);
            float s = (float)val;
            *out_l = s;
            *out_r = s;
        } else {
            s16 l = (s16)rd16(addr);
            s16 r = (offset + 2 < buf_size) ? (s16)rd16(addr + 2) : (s16)rd16(guest_buf);
            *out_l = (float)l;
            *out_r = (float)r;
        }
    }
}

/* SDL Audio Device Callback: mixes all playing secondary buffers in software */
static void dsound_audio_callback(void *userdata, Uint8 *stream, int len)
{
    struct DsHost *ds = (struct DsHost *)userdata;
    memset(stream, 0, len);

    int num_samples = len / 4; /* 22050 Hz stereo S16: 4 bytes per sample frame */
    if (num_samples <= 0) return;

    float mix_l[512];
    float mix_r[512];
    float *buf_l = mix_l;
    float *buf_r = mix_r;
    if (num_samples > 512) {
        buf_l = (float *)malloc(num_samples * sizeof(float));
        buf_r = (float *)malloc(num_samples * sizeof(float));
        if (!buf_l || !buf_r) {
            free(buf_l);
            free(buf_r);
            return;
        }
    }
    memset(buf_l, 0, num_samples * sizeof(float));
    memset(buf_r, 0, num_samples * sizeof(float));

    /* SDL audio device is locked automatically during this callback */
    for (int b = 0; b < ds->num_secondary_bufs; b++) {
        struct DsbHost *buf = ds->secondary_bufs[b];
        if (!buf) continue;
        if (!(buf->status & DSBSTATUS_PLAYING)) continue;
        if (buf->size_bytes == 0 || buf->guest_buf == 0) continue;

        int channels = buf->format.nChannels;
        int bits = buf->format.wBitsPerSample;
        int block_align = buf->format.nBlockAlign;
        if (block_align <= 0) block_align = channels * (bits / 8);
        if (block_align <= 0) continue;

        u32 total_frames = buf->size_bytes / block_align;
        if (total_frames == 0) continue;

        /* Calculate volume & pan gains */
        float vol_gain = 0.0f;
        if (buf->volume > -10000) {
            vol_gain = powf(10.0f, (float)buf->volume / 2000.0f);
        }
        float pan_l = 1.0f, pan_r = 1.0f;
        if (buf->pan < 0) {
            if (buf->pan <= -10000) pan_r = 0.0f;
            else pan_r = powf(10.0f, (float)buf->pan / 2000.0f);
        } else if (buf->pan > 0) {
            if (buf->pan >= 10000) pan_l = 0.0f;
            else pan_l = powf(10.0f, -(float)buf->pan / 2000.0f);
        }
        float gain_l = vol_gain * pan_l;
        float gain_r = vol_gain * pan_r;

        double freq = (double)buf->current_frequency;
        if (freq <= 0.0) freq = 22050.0;
        double step_bytes = (freq * (double)block_align) / 22050.0;

        double cur_pos = buf->play_cursor_bytes;
        int is_looping = (buf->status & DSBSTATUS_LOOPING) != 0;

        /* If buffer is silent, simply advance position without decoding */
        if (gain_l <= 0.000001f && gain_r <= 0.000001f) {
            cur_pos += step_bytes * num_samples;
            if (is_looping) {
                cur_pos = fmod(cur_pos, (double)buf->size_bytes);
            } else if (cur_pos >= (double)buf->size_bytes) {
                buf->status = 0;
                cur_pos = 0.0;
            }
            buf->play_cursor_bytes = cur_pos;
            continue;
        }

        for (int i = 0; i < num_samples; i++) {
            u32 frame0 = (u32)(cur_pos / (double)block_align);
            double frac = (cur_pos / (double)block_align) - (double)frame0;
            u32 frame1 = frame0 + 1;

            if (is_looping) {
                frame0 %= total_frames;
                frame1 %= total_frames;
            } else {
                if (frame0 >= total_frames) {
                    buf->status = 0;
                    cur_pos = 0.0;
                    break;
                }
                if (frame1 >= total_frames) {
                    frame1 = frame0;
                }
            }

            u32 offset0 = frame0 * block_align;
            u32 offset1 = frame1 * block_align;

            float l0, r0, l1, r1;
            read_source_frame(buf->guest_buf, buf->size_bytes, offset0, bits, channels, &l0, &r0);
            read_source_frame(buf->guest_buf, buf->size_bytes, offset1, bits, channels, &l1, &r1);

            float s_l = l0 + (float)frac * (l1 - l0);
            float s_r = r0 + (float)frac * (r1 - r0);

            buf_l[i] += s_l * gain_l;
            buf_r[i] += s_r * gain_r;

            cur_pos += step_bytes;
            if (cur_pos >= (double)buf->size_bytes) {
                if (is_looping) {
                    cur_pos = fmod(cur_pos, (double)buf->size_bytes);
                } else {
                    buf->status = 0;
                    cur_pos = 0.0;
                    break;
                }
            }
        }
        buf->play_cursor_bytes = cur_pos;
    }

    int16_t *out = (int16_t *)stream;
    for (int i = 0; i < num_samples; i++) {
        float val_l = buf_l[i];
        float val_r = buf_r[i];
        if (val_l > 32767.0f) val_l = 32767.0f;
        else if (val_l < -32768.0f) val_l = -32768.0f;
        if (val_r > 32767.0f) val_r = 32767.0f;
        else if (val_r < -32768.0f) val_r = -32768.0f;

        out[i * 2 + 0] = (int16_t)val_l;
        out[i * 2 + 1] = (int16_t)val_r;
    }

    if (num_samples > 512) {
        free(buf_l);
        free(buf_r);
    }
}

static void ds_cleanup(struct DsHost *ds)
{
    if (!ds) return;
    if (ds->audio_dev) {
        SDL_CloseAudioDevice(ds->audio_dev);
        ds->audio_dev = 0;
    }
    pthread_mutex_destroy(&ds->fallback_lock);
    free(ds);
}

/* Forward declarations of COM vtables */
static const struct ImpDef ds_methods[];
static const struct ImpDef dsb_methods[];

/* -------------------------------------------------------------------------
 * IDirectSoundBuffer implementation
 * ------------------------------------------------------------------------- */

/* vtable index 0: QueryInterface(this, riid, ppvObj) */
static u32 imp_dsb_QueryInterface(Cpu *c)
{
    u32 this_obj = ARG(0);
    u32 riid = ARG(1);
    u32 ppv = ARG(2);
    if (!ppv) return E_POINTER;

    struct Guid g;
    read_guid(riid, &g);

    if (guid_equal(&g, &IID_IUnknown) || guid_equal(&g, &IID_IDirectSoundBuffer)) {
        wr32(ppv, this_obj);
        struct DsbHost *buf = (struct DsbHost *)com_host(this_obj);
        if (buf) buf->ref_count++;
        return DS_OK;
    }

    plat_log("warn: IDirectSoundBuffer::QueryInterface unknown IID {%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
             g.data1, g.data2, g.data3,
             g.data4[0], g.data4[1], g.data4[2], g.data4[3],
             g.data4[4], g.data4[5], g.data4[6], g.data4[7]);
    wr32(ppv, 0);
    return E_NOINTERFACE;
}

/* vtable index 1: AddRef(this) */
static u32 imp_dsb_AddRef(Cpu *c)
{
    u32 this_obj = ARG(0);
    struct DsbHost *buf = (struct DsbHost *)com_host(this_obj);
    if (!buf) return 0;
    return ++buf->ref_count;
}

/* vtable index 2: Release(this) */
static u32 imp_dsb_Release(Cpu *c)
{
    u32 this_obj = ARG(0);
    struct DsbHost *buf = (struct DsbHost *)com_host(this_obj);
    if (!buf) return 0;
    if (--buf->ref_count > 0) return buf->ref_count;

    struct DsHost *ds = buf->ds;
    if (ds) {
        ds_lock(ds);
        buf->status = 0;
        for (int i = 0; i < ds->num_secondary_bufs; i++) {
            if (ds->secondary_bufs[i] == buf) {
                ds->secondary_bufs[i] = ds->secondary_bufs[--ds->num_secondary_bufs];
                break;
            }
        }
        if (ds->primary_buf == buf) {
            ds->primary_buf = NULL;
        }
        ds_unlock(ds);
    }

    if (!buf->is_primary && buf->guest_buf) {
        if (buf->shared_mem_ref) {
            if (--(*buf->shared_mem_ref) == 0) {
                gfree(buf->guest_buf);
                free(buf->shared_mem_ref);
            }
        } else {
            gfree(buf->guest_buf);
        }
    }

    if (ds) {
        if (--ds->ref_count == 0) {
            ds_cleanup(ds);
        }
    }

    free(buf);
    return 0;
}

/* vtable index 3: GetCaps(this, pDSBCaps) */
static u32 imp_dsb_GetCaps(Cpu *c)
{
    u32 this_obj = ARG(0);
    u32 pDSBCaps = ARG(1);
    if (!pDSBCaps) return DSERR_INVALIDPARAM;
    struct DsbHost *buf = (struct DsbHost *)com_host(this_obj);
    if (!buf) return E_FAIL;

    u32 size = rd32(pDSBCaps);
    if (size < 20) return DSERR_INVALIDPARAM;

    wr32(pDSBCaps + 0, 20);
    wr32(pDSBCaps + 4, buf->flags);
    wr32(pDSBCaps + 8, buf->size_bytes);
    wr32(pDSBCaps + 12, 0); /* dwUnlockTransferRate */
    wr32(pDSBCaps + 16, 0); /* dwPlayCpuOverhead */
    return DS_OK;
}

/* vtable index 4: GetCurrentPosition(this, pdwCurrentPlayCursor, pdwCurrentWriteCursor) */
static u32 imp_dsb_GetCurrentPosition(Cpu *c)
{
    u32 this_obj = ARG(0);
    u32 pdwPlay = ARG(1);
    u32 pdwWrite = ARG(2);
    struct DsbHost *buf = (struct DsbHost *)com_host(this_obj);
    if (!buf) return E_FAIL;
    if (buf->is_primary) {
        plat_log("warn: IDirectSoundBuffer::GetCurrentPosition called on primary buffer");
        return DSERR_INVALIDCALL;
    }

    ds_lock(buf->ds);
    int block_align = buf->format.nBlockAlign;
    if (block_align <= 0) block_align = 1;

    u32 play_pos = 0;
    if (buf->size_bytes > 0) {
        play_pos = (u32)buf->play_cursor_bytes;
        play_pos = (play_pos / block_align) * block_align;
        if (play_pos >= buf->size_bytes) play_pos %= buf->size_bytes;
    }

    u32 write_pos = play_pos;
    if (buf->size_bytes > 0) {
        double freq = (double)buf->current_frequency;
        if (freq <= 0.0) freq = 22050.0;
        u32 lead_bytes = (u32)((freq * (double)block_align * 512.0) / 22050.0);
        lead_bytes = ((lead_bytes + block_align - 1) / block_align) * block_align;
        if (lead_bytes < (u32)(block_align * 4)) lead_bytes = block_align * 4;
        write_pos = (play_pos + lead_bytes) % buf->size_bytes;
        write_pos = (write_pos / block_align) * block_align;
    }
    ds_unlock(buf->ds);

    if (pdwPlay) wr32(pdwPlay, play_pos);
    if (pdwWrite) wr32(pdwWrite, write_pos);
    return DS_OK;
}

/* vtable index 5: GetFormat(this, pwfxFormat, dwSizeAllocated, pdwSizeWritten) */
static u32 imp_dsb_GetFormat(Cpu *c)
{
    u32 this_obj = ARG(0);
    u32 pwfxFormat = ARG(1);
    u32 dwSizeAllocated = ARG(2);
    u32 pdwSizeWritten = ARG(3);
    struct DsbHost *buf = (struct DsbHost *)com_host(this_obj);
    if (!buf) return E_FAIL;

    if (!pwfxFormat) {
        if (pdwSizeWritten) wr32(pdwSizeWritten, 18);
        return DS_OK;
    }

    if (dwSizeAllocated < 16) return DSERR_INVALIDPARAM;

    wr16(pwfxFormat + 0, buf->format.wFormatTag);
    wr16(pwfxFormat + 2, buf->format.nChannels);
    wr32(pwfxFormat + 4, buf->format.nSamplesPerSec);
    wr32(pwfxFormat + 8, buf->format.nAvgBytesPerSec);
    wr16(pwfxFormat + 12, buf->format.nBlockAlign);
    wr16(pwfxFormat + 14, buf->format.wBitsPerSample);

    u32 written = 16;
    if (dwSizeAllocated >= 18) {
        wr16(pwfxFormat + 16, 0); /* cbSize */
        written = 18;
    }
    if (pdwSizeWritten) wr32(pdwSizeWritten, written);
    return DS_OK;
}

/* vtable index 6: GetVolume(this, plVolume) */
static u32 imp_dsb_GetVolume(Cpu *c)
{
    u32 this_obj = ARG(0);
    u32 plVolume = ARG(1);
    if (!plVolume) return DSERR_INVALIDPARAM;
    struct DsbHost *buf = (struct DsbHost *)com_host(this_obj);
    if (!buf) return E_FAIL;

    wr32(plVolume, (u32)buf->volume);
    return DS_OK;
}

/* vtable index 7: GetPan(this, plPan) */
static u32 imp_dsb_GetPan(Cpu *c)
{
    u32 this_obj = ARG(0);
    u32 plPan = ARG(1);
    if (!plPan) return DSERR_INVALIDPARAM;
    struct DsbHost *buf = (struct DsbHost *)com_host(this_obj);
    if (!buf) return E_FAIL;

    wr32(plPan, (u32)buf->pan);
    return DS_OK;
}

/* vtable index 8: GetFrequency(this, pdwFrequency) */
static u32 imp_dsb_GetFrequency(Cpu *c)
{
    u32 this_obj = ARG(0);
    u32 pdwFrequency = ARG(1);
    if (!pdwFrequency) return DSERR_INVALIDPARAM;
    struct DsbHost *buf = (struct DsbHost *)com_host(this_obj);
    if (!buf) return E_FAIL;

    wr32(pdwFrequency, buf->current_frequency);
    return DS_OK;
}

/* vtable index 9: GetStatus(this, pdwStatus) */
static u32 imp_dsb_GetStatus(Cpu *c)
{
    u32 this_obj = ARG(0);
    u32 pdwStatus = ARG(1);
    if (!pdwStatus) return DSERR_INVALIDPARAM;
    struct DsbHost *buf = (struct DsbHost *)com_host(this_obj);
    if (!buf) return E_FAIL;

    ds_lock(buf->ds);
    u32 st = buf->status;
    ds_unlock(buf->ds);

    wr32(pdwStatus, st);
    return DS_OK;
}

/* vtable index 10: Initialize(this, pDirectSound, pcDSBufferDesc) */
static u32 imp_dsb_Initialize(Cpu *c)
{
    (void)c;
    plat_log("warn: IDirectSoundBuffer::Initialize");
    return DS_OK;
}

/* vtable index 11: Lock(this, dwOffset, dwBytes, ppvAudioPtr1, pdwAudioBytes1, ppvAudioPtr2, pdwAudioBytes2, dwFlags) */
static u32 imp_dsb_Lock(Cpu *c)
{
    u32 this_obj = ARG(0);
    u32 dwOffset = ARG(1);
    u32 dwBytes = ARG(2);
    u32 ppvPtr1 = ARG(3);
    u32 pdwBytes1 = ARG(4);
    u32 ppvPtr2 = ARG(5);
    u32 pdwBytes2 = ARG(6);
    u32 dwFlags = ARG(7);

    struct DsbHost *buf = (struct DsbHost *)com_host(this_obj);
    if (!buf) return E_FAIL;
    if (buf->is_primary) {
        plat_log("warn: IDirectSoundBuffer::Lock called on primary buffer");
        return DSERR_INVALIDCALL;
    }
    if (!ppvPtr1 || !pdwBytes1) return DSERR_INVALIDPARAM;

    ds_lock(buf->ds);
    int block_align = buf->format.nBlockAlign > 0 ? buf->format.nBlockAlign : 1;

    if (dwFlags & DSBLOCK_FROMWRITECURSOR) {
        u32 play_pos = (u32)buf->play_cursor_bytes;
        play_pos = (play_pos / block_align) * block_align;
        double freq = (double)buf->current_frequency > 0 ? (double)buf->current_frequency : 22050.0;
        u32 lead_bytes = (u32)((freq * (double)block_align * 512.0) / 22050.0);
        lead_bytes = ((lead_bytes + block_align - 1) / block_align) * block_align;
        if (lead_bytes < (u32)(block_align * 4)) lead_bytes = block_align * 4;
        dwOffset = (play_pos + lead_bytes) % buf->size_bytes;
    }

    if (dwFlags & DSBLOCK_ENTIREBUFFER) {
        dwBytes = buf->size_bytes;
    }

    if (buf->size_bytes > 0) dwOffset %= buf->size_bytes;
    if (dwBytes > buf->size_bytes) dwBytes = buf->size_bytes;

    u32 r1_ptr = 0, r1_len = 0;
    u32 r2_ptr = 0, r2_len = 0;

    if (dwOffset + dwBytes <= buf->size_bytes) {
        r1_ptr = buf->guest_buf + dwOffset;
        r1_len = dwBytes;
        r2_ptr = 0;
        r2_len = 0;
    } else {
        r1_ptr = buf->guest_buf + dwOffset;
        r1_len = buf->size_bytes - dwOffset;
        r2_ptr = buf->guest_buf;
        r2_len = dwBytes - r1_len;
    }
    ds_unlock(buf->ds);

    wr32(ppvPtr1, r1_ptr);
    wr32(pdwBytes1, r1_len);
    if (ppvPtr2) wr32(ppvPtr2, r2_ptr);
    if (pdwBytes2) wr32(pdwBytes2, r2_len);
    return DS_OK;
}

/* vtable index 12: Play(this, dwReserved1, dwPriority, dwFlags) */
static u32 imp_dsb_Play(Cpu *c)
{
    u32 this_obj = ARG(0);
    u32 dwFlags = ARG(3);
    struct DsbHost *buf = (struct DsbHost *)com_host(this_obj);
    if (!buf) return E_FAIL;

    if (buf->is_primary) {
        return DS_OK;
    }

    ds_lock(buf->ds);
    buf->status = DSBSTATUS_PLAYING;
    if (dwFlags & DSBPLAY_LOOPING) {
        buf->status |= DSBSTATUS_LOOPING;
    }
    ds_unlock(buf->ds);
    return DS_OK;
}

/* vtable index 13: SetCurrentPosition(this, dwNewPosition) */
static u32 imp_dsb_SetCurrentPosition(Cpu *c)
{
    u32 this_obj = ARG(0);
    u32 dwNewPosition = ARG(1);
    struct DsbHost *buf = (struct DsbHost *)com_host(this_obj);
    if (!buf) return E_FAIL;
    if (buf->is_primary) return DSERR_INVALIDCALL;

    ds_lock(buf->ds);
    if (buf->size_bytes > 0) dwNewPosition %= buf->size_bytes;
    int block_align = buf->format.nBlockAlign > 0 ? buf->format.nBlockAlign : 1;
    dwNewPosition = (dwNewPosition / block_align) * block_align;
    buf->play_cursor_bytes = (double)dwNewPosition;
    ds_unlock(buf->ds);
    return DS_OK;
}

/* vtable index 14: SetFormat(this, pcfxFormat) */
static u32 imp_dsb_SetFormat(Cpu *c)
{
    u32 this_obj = ARG(0);
    u32 pcfx = ARG(1);
    struct DsbHost *buf = (struct DsbHost *)com_host(this_obj);
    if (!buf) return E_FAIL;
    if (!buf->is_primary) {
        plat_log("warn: IDirectSoundBuffer::SetFormat called on secondary buffer");
        return DSERR_INVALIDCALL;
    }
    if (!pcfx) return DSERR_INVALIDPARAM;

    buf->format.wFormatTag = rd16(pcfx + 0);
    buf->format.nChannels = rd16(pcfx + 2);
    buf->format.nSamplesPerSec = rd32(pcfx + 4);
    buf->format.nAvgBytesPerSec = rd32(pcfx + 8);
    buf->format.nBlockAlign = rd16(pcfx + 12);
    buf->format.wBitsPerSample = rd16(pcfx + 14);
    return DS_OK;
}

/* vtable index 15: SetVolume(this, lVolume) */
static u32 imp_dsb_SetVolume(Cpu *c)
{
    u32 this_obj = ARG(0);
    s32 lVolume = (s32)ARG(1);
    struct DsbHost *buf = (struct DsbHost *)com_host(this_obj);
    if (!buf) return E_FAIL;
    if (buf->is_primary) return DSERR_INVALIDCALL;

    if (lVolume > 0) lVolume = 0;
    if (lVolume < -10000) lVolume = -10000;

    ds_lock(buf->ds);
    buf->volume = lVolume;
    ds_unlock(buf->ds);
    return DS_OK;
}

/* vtable index 16: SetPan(this, lPan) */
static u32 imp_dsb_SetPan(Cpu *c)
{
    u32 this_obj = ARG(0);
    s32 lPan = (s32)ARG(1);
    struct DsbHost *buf = (struct DsbHost *)com_host(this_obj);
    if (!buf) return E_FAIL;
    if (buf->is_primary) return DSERR_INVALIDCALL;

    if (lPan > 10000) lPan = 10000;
    if (lPan < -10000) lPan = -10000;

    ds_lock(buf->ds);
    buf->pan = lPan;
    ds_unlock(buf->ds);
    return DS_OK;
}

/* vtable index 17: SetFrequency(this, dwFrequency) */
static u32 imp_dsb_SetFrequency(Cpu *c)
{
    u32 this_obj = ARG(0);
    u32 dwFrequency = ARG(1);
    struct DsbHost *buf = (struct DsbHost *)com_host(this_obj);
    if (!buf) return E_FAIL;
    if (buf->is_primary) return DSERR_INVALIDCALL;

    if (dwFrequency == 0) {
        dwFrequency = buf->format.nSamplesPerSec;
    }
    if (dwFrequency < 100 || dwFrequency > 100000) {
        return DSERR_INVALIDPARAM;
    }

    ds_lock(buf->ds);
    buf->current_frequency = dwFrequency;
    ds_unlock(buf->ds);
    return DS_OK;
}

/* vtable index 18: Stop(this) */
static u32 imp_dsb_Stop(Cpu *c)
{
    u32 this_obj = ARG(0);
    struct DsbHost *buf = (struct DsbHost *)com_host(this_obj);
    if (!buf) return E_FAIL;

    ds_lock(buf->ds);
    buf->status = 0;
    ds_unlock(buf->ds);
    return DS_OK;
}

/* vtable index 19: Unlock(this, pvAudioPtr1, dwAudioBytes1, pvAudioPtr2, dwAudioBytes2) */
static u32 imp_dsb_Unlock(Cpu *c)
{
    (void)c;
    return DS_OK;
}

/* vtable index 20: Restore(this) */
static u32 imp_dsb_Restore(Cpu *c)
{
    (void)c;
    plat_log("warn: IDirectSoundBuffer::Restore");
    return DS_OK;
}

static const struct ImpDef dsb_methods[] = {
    /* 0 */ IMPORT(dsb_QueryInterface, 3),
    /* 1 */ IMPORT(dsb_AddRef, 1),
    /* 2 */ IMPORT(dsb_Release, 1),
    /* 3 */ IMPORT(dsb_GetCaps, 2),
    /* 4 */ IMPORT(dsb_GetCurrentPosition, 3),
    /* 5 */ IMPORT(dsb_GetFormat, 4),
    /* 6 */ IMPORT(dsb_GetVolume, 2),
    /* 7 */ IMPORT(dsb_GetPan, 2),
    /* 8 */ IMPORT(dsb_GetFrequency, 2),
    /* 9 */ IMPORT(dsb_GetStatus, 2),
    /* 10 */ IMPORT(dsb_Initialize, 3),
    /* 11 */ IMPORT(dsb_Lock, 8),
    /* 12 */ IMPORT(dsb_Play, 4),
    /* 13 */ IMPORT(dsb_SetCurrentPosition, 2),
    /* 14 */ IMPORT(dsb_SetFormat, 2),
    /* 15 */ IMPORT(dsb_SetVolume, 2),
    /* 16 */ IMPORT(dsb_SetPan, 2),
    /* 17 */ IMPORT(dsb_SetFrequency, 2),
    /* 18 */ IMPORT(dsb_Stop, 1),
    /* 19 */ IMPORT(dsb_Unlock, 5),
    /* 20 */ IMPORT(dsb_Restore, 1),
};

/* -------------------------------------------------------------------------
 * IDirectSound implementation
 * ------------------------------------------------------------------------- */

/* vtable index 0: QueryInterface(this, riid, ppvObj) */
static u32 imp_ds_QueryInterface(Cpu *c)
{
    u32 this_obj = ARG(0);
    u32 riid = ARG(1);
    u32 ppv = ARG(2);
    if (!ppv) return E_POINTER;

    struct Guid g;
    read_guid(riid, &g);

    if (guid_equal(&g, &IID_IUnknown) || guid_equal(&g, &IID_IDirectSound)) {
        wr32(ppv, this_obj);
        struct DsHost *ds = (struct DsHost *)com_host(this_obj);
        if (ds) ds->ref_count++;
        return DS_OK;
    }

    plat_log("warn: IDirectSound::QueryInterface unknown IID {%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
             g.data1, g.data2, g.data3,
             g.data4[0], g.data4[1], g.data4[2], g.data4[3],
             g.data4[4], g.data4[5], g.data4[6], g.data4[7]);
    wr32(ppv, 0);
    return E_NOINTERFACE;
}

/* vtable index 1: AddRef(this) */
static u32 imp_ds_AddRef(Cpu *c)
{
    u32 this_obj = ARG(0);
    struct DsHost *ds = (struct DsHost *)com_host(this_obj);
    if (!ds) return 0;
    return ++ds->ref_count;
}

/* vtable index 2: Release(this) */
static u32 imp_ds_Release(Cpu *c)
{
    u32 this_obj = ARG(0);
    struct DsHost *ds = (struct DsHost *)com_host(this_obj);
    if (!ds) return 0;
    if (--ds->ref_count == 0) {
        ds_cleanup(ds);
        return 0;
    }
    return ds->ref_count;
}

/* vtable index 3: CreateSoundBuffer(this, lpDSBufferDesc, lplpDirectSoundBuffer, pUnkOuter) */
static u32 imp_ds_CreateSoundBuffer(Cpu *c)
{
    u32 this_obj = ARG(0);
    u32 lpDesc = ARG(1);
    u32 ppDSB = ARG(2);
    u32 pUnkOuter = ARG(3);

    if (pUnkOuter != 0) return DSERR_NOAGGREGATION;
    if (!lpDesc || !ppDSB) return DSERR_INVALIDPARAM;

    struct DsHost *ds = (struct DsHost *)com_host(this_obj);
    if (!ds) return E_FAIL;

    u32 dwFlags = rd32(lpDesc + 4);
    u32 dwBufferBytes = rd32(lpDesc + 8);
    u32 lpwfxFormat = rd32(lpDesc + 16);

    struct DsbHost *buf = (struct DsbHost *)calloc(1, sizeof(*buf));
    if (!buf) return DSERR_OUTOFMEMORY;

    buf->ds = ds;
    buf->ref_count = 1;
    buf->flags = dwFlags;
    buf->volume = 0;
    buf->pan = 0;
    buf->status = 0;
    buf->play_cursor_bytes = 0.0;

    if (dwFlags & DSBCAPS_PRIMARYBUFFER) {
        buf->is_primary = 1;
        buf->size_bytes = 0;
        buf->guest_buf = 0;
        buf->format.wFormatTag = 1;
        buf->format.nChannels = 2;
        buf->format.nSamplesPerSec = 22050;
        buf->format.wBitsPerSample = 16;
        buf->format.nBlockAlign = 4;
        buf->format.nAvgBytesPerSec = 22050 * 4;
        buf->current_frequency = 22050;

        ds_lock(ds);
        ds->primary_buf = buf;
        ds_unlock(ds);
    } else {
        buf->is_primary = 0;
        if (!lpwfxFormat || dwBufferBytes == 0) {
            free(buf);
            return DSERR_INVALIDPARAM;
        }

        buf->size_bytes = dwBufferBytes;
        buf->guest_buf = galloc(dwBufferBytes);
        if (!buf->guest_buf) {
            free(buf);
            return DSERR_OUTOFMEMORY;
        }

        buf->shared_mem_ref = (int *)malloc(sizeof(int));
        if (buf->shared_mem_ref) *buf->shared_mem_ref = 1;

        buf->format.wFormatTag = rd16(lpwfxFormat + 0);
        buf->format.nChannels = rd16(lpwfxFormat + 2);
        buf->format.nSamplesPerSec = rd32(lpwfxFormat + 4);
        buf->format.nAvgBytesPerSec = rd32(lpwfxFormat + 8);
        buf->format.nBlockAlign = rd16(lpwfxFormat + 12);
        buf->format.wBitsPerSample = rd16(lpwfxFormat + 14);

        if (buf->format.nBlockAlign <= 0) {
            buf->format.nBlockAlign = buf->format.nChannels * (buf->format.wBitsPerSample / 8);
        }
        if (buf->format.nBlockAlign <= 0) buf->format.nBlockAlign = 1;

        buf->current_frequency = buf->format.nSamplesPerSec;

        ds_lock(ds);
        if (ds->num_secondary_bufs < MAX_DSOUND_BUFFERS) {
            ds->secondary_bufs[ds->num_secondary_bufs++] = buf;
        } else {
            plat_log("warn: IDirectSound::CreateSoundBuffer max secondary buffers exceeded");
        }
        ds_unlock(ds);
    }

    u32 obj = com_new(dsb_methods, 21, buf);
    buf->guest_obj = obj;

    ds->ref_count++; /* each buffer retains the DirectSound device */

    wr32(ppDSB, obj);
    return DS_OK;
}

/* vtable index 4: GetCaps(this, lpDSCaps) */
static u32 imp_ds_GetCaps(Cpu *c)
{
    u32 lpDSCaps = ARG(1);
    if (!lpDSCaps) return DSERR_INVALIDPARAM;

    u32 size = rd32(lpDSCaps);
    if (size < 96) return DSERR_INVALIDPARAM;

    wr32(lpDSCaps + 0, 96);
    wr32(lpDSCaps + 4, DSCAPS_PRIMARYSTEREO | DSCAPS_PRIMARY16BIT |
                       DSCAPS_SECONDARYSTEREO | DSCAPS_SECONDARY16BIT |
                       DSCAPS_CONTINUOUSRATE | DSCAPS_CERTIFIED);
    wr32(lpDSCaps + 8, 100);       /* dwMinSecondarySampleRate */
    wr32(lpDSCaps + 12, 100000);   /* dwMaxSecondarySampleRate */
    wr32(lpDSCaps + 16, 1);        /* dwPrimaryBuffers */
    wr32(lpDSCaps + 20, 0);        /* dwMaxHwMixingAllBuffers */
    wr32(lpDSCaps + 24, 0);        /* dwMaxHwMixingStaticBuffers */
    wr32(lpDSCaps + 28, 0);        /* dwMaxHwMixingStreamingBuffers */
    wr32(lpDSCaps + 32, 0);        /* dwFreeHwMixingAllBuffers */
    wr32(lpDSCaps + 36, 0);        /* dwFreeHwMixingStaticBuffers */
    wr32(lpDSCaps + 40, 0);        /* dwFreeHwMixingStreamingBuffers */
    wr32(lpDSCaps + 44, 0);        /* dwMaxHw3DAllBuffers */
    wr32(lpDSCaps + 48, 0);        /* dwMaxHw3DStaticBuffers */
    wr32(lpDSCaps + 52, 0);        /* dwMaxHw3DStreamingBuffers */
    wr32(lpDSCaps + 56, 0);        /* dwFreeHw3DAllBuffers */
    wr32(lpDSCaps + 60, 0);        /* dwFreeHw3DStaticBuffers */
    wr32(lpDSCaps + 64, 0);        /* dwFreeHw3DStreamingBuffers */
    wr32(lpDSCaps + 68, 0);        /* dwTotalHwMemBytes */
    wr32(lpDSCaps + 72, 0);        /* dwFreeHwMemBytes */
    wr32(lpDSCaps + 76, 0);        /* dwMaxContigFreeHwMemBytes */
    wr32(lpDSCaps + 80, 0);        /* dwUnlockTransferRateHwBuffers */
    wr32(lpDSCaps + 84, 0);        /* dwPlayCpuOverheadSwBuffers */
    wr32(lpDSCaps + 88, 0);        /* dwReserved1 */
    wr32(lpDSCaps + 92, 0);        /* dwReserved2 */
    return DS_OK;
}

/* vtable index 5: DuplicateSoundBuffer(this, lpDsbOriginal, lplpDsbDuplicate) */
static u32 imp_ds_DuplicateSoundBuffer(Cpu *c)
{
    u32 this_obj = ARG(0);
    u32 orig_obj = ARG(1);
    u32 out_dup = ARG(2);
    struct DsHost *ds = (struct DsHost *)com_host(this_obj);
    if (!ds || !orig_obj || !out_dup) return DSERR_INVALIDPARAM;

    struct DsbHost *orig = (struct DsbHost *)com_host(orig_obj);
    if (!orig || orig->is_primary) return DSERR_INVALIDCALL;

    struct DsbHost *dup = (struct DsbHost *)calloc(1, sizeof(*dup));
    if (!dup) return DSERR_OUTOFMEMORY;

    dup->ds = ds;
    dup->is_primary = 0;
    dup->flags = orig->flags;
    dup->size_bytes = orig->size_bytes;
    dup->guest_buf = orig->guest_buf; /* Shares audio buffer memory */
    dup->shared_mem_ref = orig->shared_mem_ref;
    if (dup->shared_mem_ref) {
        (*dup->shared_mem_ref)++;
    }
    dup->format = orig->format;
    dup->current_frequency = orig->current_frequency;
    dup->volume = orig->volume;
    dup->pan = orig->pan;
    dup->status = 0;
    dup->play_cursor_bytes = 0.0;
    dup->ref_count = 1;

    u32 dup_obj = com_new(dsb_methods, 21, dup);
    dup->guest_obj = dup_obj;

    ds_lock(ds);
    if (ds->num_secondary_bufs < MAX_DSOUND_BUFFERS) {
        ds->secondary_bufs[ds->num_secondary_bufs++] = dup;
    } else {
        plat_log("warn: IDirectSound::DuplicateSoundBuffer max secondary buffers reached");
    }
    ds_unlock(ds);

    ds->ref_count++;

    wr32(out_dup, dup_obj);
    return DS_OK;
}

/* vtable index 6: SetCooperativeLevel(this, hwnd, dwLevel) */
static u32 imp_ds_SetCooperativeLevel(Cpu *c)
{
    u32 this_obj = ARG(0);
    u32 hwnd = ARG(1);
    u32 dwLevel = ARG(2);
    struct DsHost *ds = (struct DsHost *)com_host(this_obj);
    if (!ds) return E_FAIL;

    ds->hwnd = hwnd;
    ds->coop_level = dwLevel;
    return DS_OK;
}

/* vtable index 7: Compact(this) */
static u32 imp_ds_Compact(Cpu *c)
{
    (void)c;
    plat_log("warn: IDirectSound::Compact");
    return DS_OK;
}

/* vtable index 8: GetSpeakerConfig(this, pdwSpeakerConfig) */
static u32 imp_ds_GetSpeakerConfig(Cpu *c)
{
    u32 this_obj = ARG(0);
    u32 pdw = ARG(1);
    if (!pdw) return DSERR_INVALIDPARAM;
    struct DsHost *ds = (struct DsHost *)com_host(this_obj);
    if (!ds) return E_FAIL;

    wr32(pdw, ds->speaker_config);
    return DS_OK;
}

/* vtable index 9: SetSpeakerConfig(this, dwSpeakerConfig) */
static u32 imp_ds_SetSpeakerConfig(Cpu *c)
{
    u32 this_obj = ARG(0);
    u32 cfg = ARG(1);
    struct DsHost *ds = (struct DsHost *)com_host(this_obj);
    if (!ds) return E_FAIL;

    ds->speaker_config = cfg;
    return DS_OK;
}

/* vtable index 10: Initialize(this, pcGuidDevice) */
static u32 imp_ds_Initialize(Cpu *c)
{
    (void)c;
    plat_log("warn: IDirectSound::Initialize");
    return DS_OK;
}

static const struct ImpDef ds_methods[] = {
    /* 0 */ IMPORT(ds_QueryInterface, 3),
    /* 1 */ IMPORT(ds_AddRef, 1),
    /* 2 */ IMPORT(ds_Release, 1),
    /* 3 */ IMPORT(ds_CreateSoundBuffer, 4),
    /* 4 */ IMPORT(ds_GetCaps, 2),
    /* 5 */ IMPORT(ds_DuplicateSoundBuffer, 3),
    /* 6 */ IMPORT(ds_SetCooperativeLevel, 3),
    /* 7 */ IMPORT(ds_Compact, 1),
    /* 8 */ IMPORT(ds_GetSpeakerConfig, 2),
    /* 9 */ IMPORT(ds_SetSpeakerConfig, 2),
    /* 10 */ IMPORT(ds_Initialize, 2),
};

/* -------------------------------------------------------------------------
 * Exported functions (DSOUND.dll)
 * ------------------------------------------------------------------------- */

/* HRESULT WINAPI DirectSoundCreate(LPCGUID pcGuidDevice, LPDIRECTSOUND *ppDS, LPUNKNOWN pUnkOuter) */
static u32 imp_DirectSoundCreate(Cpu *c)
{
    u32 ppDS = ARG(1);
    u32 pUnkOuter = ARG(2);

    if (pUnkOuter != 0) return DSERR_NOAGGREGATION;
    if (!ppDS) return DSERR_INVALIDPARAM;

    struct DsHost *ds = (struct DsHost *)calloc(1, sizeof(*ds));
    if (!ds) return DSERR_OUTOFMEMORY;

    ds->ref_count = 1;
    ds->speaker_config = 4; /* DSSPEAKER_STEREO */
    ds->coop_level = 1;     /* DSSCL_NORMAL */
    pthread_mutex_init(&ds->fallback_lock, NULL);

    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
            plat_log("warn: SDL_InitSubSystem(AUDIO) failed: %s", SDL_GetError());
        }
    }

    SDL_AudioSpec desired, obtained;
    memset(&desired, 0, sizeof(desired));
    desired.freq = 22050;
    desired.format = AUDIO_S16SYS;
    desired.channels = 2;
    desired.samples = 512;
    desired.callback = dsound_audio_callback;
    desired.userdata = ds;

    ds->audio_dev = SDL_OpenAudioDevice(NULL, 0, &desired, &obtained, 0);
    if (ds->audio_dev == 0) {
        plat_log("warn: SDL_OpenAudioDevice failed: %s", SDL_GetError());
    } else {
        SDL_PauseAudioDevice(ds->audio_dev, 0);
    }

    u32 obj = com_new(ds_methods, 11, ds);
    wr32(ppDS, obj);
    return DS_OK;
}

/* HRESULT WINAPI DirectSoundEnumerateA(LPDSENUMCALLBACKA lpDSEnumCallback, LPVOID lpContext) */
static u32 imp_DirectSoundEnumerateA(Cpu *c)
{
    u32 lpCallback = ARG(0);
    u32 lpContext = ARG(1);
    if (!lpCallback) return DSERR_INVALIDPARAM;

    static u32 guest_desc = 0;
    static u32 guest_mod = 0;
    if (!guest_desc) {
        guest_desc = gstrdup("Primary Sound Driver");
        guest_mod = gstrdup("");
    }

    u32 args[4];
    args[0] = 0; /* NULL GUID */
    args[1] = guest_desc;
    args[2] = guest_mod;
    args[3] = lpContext;

    guest_call(c, lpCallback, 4, args);
    return DS_OK;
}

const struct ImpDef imp_dsound[] = {
    IMPORT(DirectSoundCreate, 3),
    IMPORT(DirectSoundEnumerateA, 2),
};
IMPORT_TABLE(dsound);
