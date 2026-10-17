/* dsound.dll - DirectSound implementation for recompiled Future Cop */
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <SDL.h>
#include "host.h"
#include "sound.h"

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

/* PlaySound flags */
#define SND_SYNC                        0x00000000u
#define SND_ASYNC                       0x00000001u
#define SND_NODEFAULT                   0x00000002u
#define SND_MEMORY                      0x00000004u
#define SND_LOOP                        0x00000008u
#define SND_NOSTOP                      0x00000010u
#define SND_PURGE                       0x00000040u
#define SND_NOWAIT                      0x00002000u
#define SND_ALIAS                       0x00010000u
#define SND_FILENAME                    0x00020000u
#define SND_RESOURCE                    0x00040004u

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
#define MAX_DS_INSTANCES 16

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
    struct DsbHost *primary_buf;
    struct DsbHost *secondary_bufs[MAX_DSOUND_BUFFERS];
    int num_secondary_bufs;
};

/* Global audio device and lock */
static SDL_AudioDeviceID g_audio_dev = 0;
static int g_audio_dev_opened = 0;
static int g_device_freq = 44100;
static int g_device_channels = 2;
static pthread_mutex_t g_sound_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct DsHost *g_ds_instances[MAX_DS_INSTANCES];
static int g_num_ds_instances = 0;

/* PlaySoundA state */
struct PlaySoundState {
    int active;
    int looping;
    const u8 *samples;
    u32 num_bytes;
    int channels;
    int bits;
    int block_align;
    int sample_rate;
    double play_cursor_bytes;
    u8 *allocated_buf;
};
static struct PlaySoundState g_playsound;

static inline void sound_lock(void)
{
    pthread_mutex_lock(&g_sound_mutex);
}

static inline void sound_unlock(void)
{
    pthread_mutex_unlock(&g_sound_mutex);
}

static inline void read_sample_frame(const u8 *base, u32 buf_size, u32 offset,
                                     int bits, int channels, float *out_l, float *out_r)
{
    if (buf_size == 0) {
        *out_l = 0.0f;
        *out_r = 0.0f;
        return;
    }
    if (offset >= buf_size) offset %= buf_size;
    const u8 *addr = base + offset;

    if (bits == 8) {
        if (channels == 1) {
            u8 val = *addr;
            float s = (float)((int)val - 128) * 256.0f;
            *out_l = s;
            *out_r = s;
        } else {
            u8 l = addr[0];
            u8 r = (offset + 1 < buf_size) ? addr[1] : base[0];
            *out_l = (float)((int)l - 128) * 256.0f;
            *out_r = (float)((int)r - 128) * 256.0f;
        }
    } else { /* 16-bit */
        if (channels == 1) {
            s16 val;
            memcpy(&val, addr, 2);
            float s = (float)val;
            *out_l = s;
            *out_r = s;
        } else {
            s16 l, r;
            memcpy(&l, addr, 2);
            if (offset + 2 < buf_size) {
                memcpy(&r, addr + 2, 2);
            } else {
                memcpy(&r, base, 2);
            }
            *out_l = (float)l;
            *out_r = (float)r;
        }
    }
}

/* SDL Audio Device Callback: mixes all playing secondary buffers and PlaySoundA */
static void audio_callback(void *userdata, Uint8 *stream, int len)
{
    (void)userdata;
    memset(stream, 0, len);

    int num_samples = len / 4; /* 44100 Hz stereo S16: 4 bytes per sample frame */
    if (num_samples <= 0) return;

    float mix_l[1024];
    float mix_r[1024];
    float *buf_l = mix_l;
    float *buf_r = mix_r;
    if (num_samples > 1024) {
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

    sound_lock();

    /* 1. Mix DirectSound secondary buffers */
    for (int d = 0; d < g_num_ds_instances; d++) {
        struct DsHost *ds = g_ds_instances[d];
        if (!ds) continue;

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

            /* Volume & Pan gains */
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
            double step_bytes = (freq * (double)block_align) / (double)g_device_freq;

            double cur_pos = buf->play_cursor_bytes;
            int is_looping = (buf->status & DSBSTATUS_LOOPING) != 0;

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

            const u8 *base = (const u8 *)G(buf->guest_buf);

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
                read_sample_frame(base, buf->size_bytes, offset0, bits, channels, &l0, &r0);
                read_sample_frame(base, buf->size_bytes, offset1, bits, channels, &l1, &r1);

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
    }

    /* 2. Mix PlaySoundA sound */
    if (g_playsound.active && g_playsound.samples && g_playsound.num_bytes > 0) {
        int channels = g_playsound.channels;
        int bits = g_playsound.bits;
        int block_align = g_playsound.block_align;
        if (block_align <= 0) block_align = channels * (bits / 8);
        if (block_align > 0) {
            u32 total_frames = g_playsound.num_bytes / block_align;
            if (total_frames > 0) {
                double freq = (double)g_playsound.sample_rate;
                if (freq <= 0.0) freq = 22050.0;
                double step_bytes = (freq * (double)block_align) / (double)g_device_freq;

                double cur_pos = g_playsound.play_cursor_bytes;
                int is_looping = g_playsound.looping;
                const u8 *base = g_playsound.samples;

                for (int i = 0; i < num_samples; i++) {
                    u32 frame0 = (u32)(cur_pos / (double)block_align);
                    double frac = (cur_pos / (double)block_align) - (double)frame0;
                    u32 frame1 = frame0 + 1;

                    if (is_looping) {
                        frame0 %= total_frames;
                        frame1 %= total_frames;
                    } else {
                        if (frame0 >= total_frames) {
                            g_playsound.active = 0;
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
                    read_sample_frame(base, g_playsound.num_bytes, offset0, bits, channels, &l0, &r0);
                    read_sample_frame(base, g_playsound.num_bytes, offset1, bits, channels, &l1, &r1);

                    float s_l = l0 + (float)frac * (l1 - l0);
                    float s_r = r0 + (float)frac * (r1 - r0);

                    buf_l[i] += s_l;
                    buf_r[i] += s_r;

                    cur_pos += step_bytes;
                    if (cur_pos >= (double)g_playsound.num_bytes) {
                        if (is_looping) {
                            cur_pos = fmod(cur_pos, (double)g_playsound.num_bytes);
                        } else {
                            g_playsound.active = 0;
                            cur_pos = 0.0;
                            break;
                        }
                    }
                }
                g_playsound.play_cursor_bytes = cur_pos;
            }
        }
    }

    sound_unlock();

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

    if (num_samples > 1024) {
        free(buf_l);
        free(buf_r);
    }
}

/* Open SDL audio device once */
static void ensure_audio_device_opened(void)
{
    if (g_audio_dev_opened) return;
    g_audio_dev_opened = 1;

    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
            plat_log("warn: SDL_InitSubSystem(AUDIO) failed: %s", SDL_GetError());
            return;
        }
    }

    SDL_AudioSpec desired, obtained;
    memset(&desired, 0, sizeof(desired));
    desired.freq = 44100;
    desired.format = AUDIO_S16SYS;
    desired.channels = 2;
    desired.samples = 1024;
    desired.callback = audio_callback;
    desired.userdata = NULL;

    g_audio_dev = SDL_OpenAudioDevice(NULL, 0, &desired, &obtained, 0);
    if (g_audio_dev == 0) {
        plat_log("warn: SDL_OpenAudioDevice failed: %s", SDL_GetError());
        return;
    }

    g_device_freq = obtained.freq;
    g_device_channels = obtained.channels;
    plat_log("audio device: %d Hz %d ch", obtained.freq, obtained.channels);
    SDL_PauseAudioDevice(g_audio_dev, 0);
}

/* RIFF/WAVE parser */
struct RiffWave {
    int format_tag;
    int channels;
    int sample_rate;
    int block_align;
    int bits_per_sample;
    const u8 *samples;
    u32 data_bytes;
};

static int parse_riff_wave(const u8 *data, u32 size, struct RiffWave *out)
{
    if (!data || size < 12) return 0;
    if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) return 0;

    memset(out, 0, sizeof(*out));
    int have_fmt = 0, have_data = 0;
    u32 offset = 12;

    while (offset + 8 <= size) {
        char chunk_id[4];
        memcpy(chunk_id, data + offset, 4);
        u32 chunk_sz;
        memcpy(&chunk_sz, data + offset + 4, 4);

        if (offset + 8 + chunk_sz > size) {
            chunk_sz = size - offset - 8;
        }

        if (memcmp(chunk_id, "fmt ", 4) == 0 && chunk_sz >= 14) {
            u16 tag, ch, ba, bits;
            u32 rate;
            memcpy(&tag, data + offset + 8, 2);
            memcpy(&ch, data + offset + 10, 2);
            memcpy(&rate, data + offset + 12, 4);
            memcpy(&ba, data + offset + 20, 2);
            bits = (chunk_sz >= 16) ? ((u16)data[offset + 22] | ((u16)data[offset + 23] << 8)) : 16;
            out->format_tag = tag;
            out->channels = ch;
            out->sample_rate = rate;
            out->block_align = ba > 0 ? ba : (ch * (bits / 8));
            out->bits_per_sample = bits;
            have_fmt = 1;
        } else if (memcmp(chunk_id, "data", 4) == 0) {
            out->samples = data + offset + 8;
            out->data_bytes = chunk_sz;
            have_data = 1;
        }

        offset += 8 + ((chunk_sz + 1) & ~1u);
    }

    if (!have_fmt || !have_data) return 0;
    if (out->block_align <= 0) out->block_align = out->channels * (out->bits_per_sample / 8);
    if (out->block_align <= 0) out->block_align = 1;
    if (out->sample_rate <= 0) out->sample_rate = 22050;
    return 1;
}

/* Locate a WAVE resource from the loaded PE EXE at 0x400000 */
static const u8 *find_pe_wave_resource(u32 id, u32 *out_size)
{
    u32 pe_hdr = 0x400000 + rd32(0x40003c);
    u32 rsrc_rva = rd32(pe_hdr + 136);
    u32 rsrc_size = rd32(pe_hdr + 140);
    if (!rsrc_rva || !rsrc_size) return NULL;
    u32 rsrc_base = 0x400000 + rsrc_rva;

    /* Level 1: Resource Type "WAVE" */
    u16 num_named = rd16(rsrc_base + 12);
    u16 num_id = rd16(rsrc_base + 14);
    u32 wave_dir = 0;
    for (int i = 0; i < num_named + num_id; i++) {
        u32 entry = rsrc_base + 16 + i * 8;
        u32 name = rd32(entry);
        u32 offset = rd32(entry + 4);
        if (name & 0x80000000) {
            u32 str_addr = rsrc_base + (name & 0x7FFFFFFF);
            u16 slen = rd16(str_addr);
            if (slen == 4) {
                u16 c0 = rd16(str_addr + 2);
                u16 c1 = rd16(str_addr + 4);
                u16 c2 = rd16(str_addr + 6);
                u16 c3 = rd16(str_addr + 8);
                if ((c0 == 'W' || c0 == 'w') &&
                    (c1 == 'A' || c1 == 'a') &&
                    (c2 == 'V' || c2 == 'v') &&
                    (c3 == 'E' || c3 == 'e')) {
                    wave_dir = rsrc_base + (offset & 0x7FFFFFFF);
                    break;
                }
            }
        }
    }
    if (!wave_dir) return NULL;

    /* Level 2: Resource ID or Name */
    u16 wave_named = rd16(wave_dir + 12);
    u16 wave_id_count = rd16(wave_dir + 14);
    u32 lang_dir = 0;
    for (int i = 0; i < wave_named + wave_id_count; i++) {
        u32 entry = wave_dir + 16 + i * 8;
        u32 name = rd32(entry);
        u32 offset = rd32(entry + 4);
        if (id <= 0xFFFF) {
            if (!(name & 0x80000000) && name == id) {
                lang_dir = rsrc_base + (offset & 0x7FFFFFFF);
                break;
            }
        } else {
            if (name & 0x80000000) {
                u32 str_addr = rsrc_base + (name & 0x7FFFFFFF);
                u16 slen = rd16(str_addr);
                char res_name[128];
                int k;
                for (k = 0; k < slen && k < 127; k++) {
                    u16 wc = rd16(str_addr + 2 + k * 2);
                    res_name[k] = (wc < 256) ? (char)wc : '?';
                }
                res_name[k] = '\0';
                if (!strcasecmp(res_name, GSTR(id))) {
                    lang_dir = rsrc_base + (offset & 0x7FFFFFFF);
                    break;
                }
            }
        }
    }
    if (!lang_dir) return NULL;

    /* Level 3: First Language */
    u32 data_entry_offset = rd32(lang_dir + 16 + 4);
    if (data_entry_offset & 0x80000000) return NULL;
    u32 data_entry = rsrc_base + data_entry_offset;
    u32 rva = rd32(data_entry + 0);
    u32 sz = rd32(data_entry + 4);

    if (out_size) *out_size = sz;
    return (const u8 *)(MEM + 0x400000 + rva);
}

/* PlaySoundA implementation */
u32 sound_playsound(u32 pszSound, u32 hmod, u32 flags)
{
    (void)hmod;

    /* pszSound == NULL stops any playing sound */
    if (!pszSound) {
        sound_lock();
        g_playsound.active = 0;
        if (g_playsound.allocated_buf) {
            free(g_playsound.allocated_buf);
            g_playsound.allocated_buf = NULL;
        }
        sound_unlock();
        return 1;
    }

    if (flags & SND_NOSTOP) {
        sound_lock();
        int playing = g_playsound.active;
        sound_unlock();
        if (playing) return 0;
    }

    const u8 *wave_data = NULL;
    u32 wave_size = 0;
    u8 *allocated_buf = NULL;

    if ((flags & 0x40000) || (pszSound <= 0xffff && !(flags & (SND_FILENAME | SND_MEMORY)))) {
        /* Resource lookup */
        wave_data = find_pe_wave_resource(pszSound, &wave_size);
        if (!wave_data) {
            plat_log("warn: PlaySoundA resource %u not found", pszSound);
            return 0;
        }
    } else if (flags & SND_FILENAME) {
        char hpath[1024];
        host_path(GSTR(pszSound), hpath, sizeof(hpath));
        FILE *f = fopen(hpath, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz > 0) {
                allocated_buf = (u8 *)malloc((size_t)sz);
                if (allocated_buf && fread(allocated_buf, 1, (size_t)sz, f) == (size_t)sz) {
                    wave_data = allocated_buf;
                    wave_size = (u32)sz;
                } else {
                    free(allocated_buf);
                    allocated_buf = NULL;
                }
            }
            fclose(f);
        }
        if (!wave_data) return 0;
    } else if (flags & SND_MEMORY) {
        wave_data = (const u8 *)G(pszSound);
        if (wave_data && memcmp(wave_data, "RIFF", 4) == 0) {
            wave_size = rd32(pszSound + 4) + 8;
        } else {
            return 0;
        }
    } else {
        /* Fallback: try resource first, then filename */
        wave_data = find_pe_wave_resource(pszSound, &wave_size);
        if (!wave_data) {
            char hpath[1024];
            host_path(GSTR(pszSound), hpath, sizeof(hpath));
            FILE *f = fopen(hpath, "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (sz > 0) {
                    allocated_buf = (u8 *)malloc((size_t)sz);
                    if (allocated_buf && fread(allocated_buf, 1, (size_t)sz, f) == (size_t)sz) {
                        wave_data = allocated_buf;
                        wave_size = (u32)sz;
                    } else {
                        free(allocated_buf);
                        allocated_buf = NULL;
                    }
                }
                fclose(f);
            }
        }
        if (!wave_data) return 0;
    }

    struct RiffWave wave;
    if (!parse_riff_wave(wave_data, wave_size, &wave)) {
        plat_log("warn: PlaySoundA failed to parse RIFF/WAVE header");
        if (allocated_buf) free(allocated_buf);
        return 0;
    }

    ensure_audio_device_opened();

    sound_lock();
    if (g_playsound.allocated_buf) {
        free(g_playsound.allocated_buf);
        g_playsound.allocated_buf = NULL;
    }
    g_playsound.samples = wave.samples;
    g_playsound.num_bytes = wave.data_bytes;
    g_playsound.channels = wave.channels;
    g_playsound.bits = wave.bits_per_sample;
    g_playsound.block_align = wave.block_align;
    g_playsound.sample_rate = wave.sample_rate;
    g_playsound.looping = (flags & SND_LOOP) != 0;
    g_playsound.play_cursor_bytes = 0.0;
    g_playsound.allocated_buf = allocated_buf;
    g_playsound.active = 1;
    sound_unlock();

    if (!(flags & SND_ASYNC)) {
        u32 duration_ms = 0;
        if (wave.block_align > 0 && wave.sample_rate > 0) {
            duration_ms = (u32)(((u64)wave.data_bytes * 1000ull) / ((u64)wave.sample_rate * (u64)wave.block_align));
        }
        u32 start_t = SDL_GetTicks();
        while (1) {
            sound_lock();
            int active = g_playsound.active;
            sound_unlock();
            if (!active) break;
            if (SDL_GetTicks() - start_t > duration_ms + 1000) {
                sound_lock();
                g_playsound.active = 0;
                sound_unlock();
                break;
            }
            SDL_Delay(5);
        }
    }

    return 1;
}

/* Helper to compute write cursor ahead of play cursor */
static inline u32 calc_write_cursor(struct DsbHost *buf, u32 play_pos)
{
    int block_align = buf->format.nBlockAlign > 0 ? buf->format.nBlockAlign : 1;
    double freq = (double)buf->current_frequency > 0 ? (double)buf->current_frequency : 22050.0;
    u32 lead_frames = (u32)((freq * 1024.0) / (double)g_device_freq);
    u32 lead_bytes = lead_frames * block_align;
    if (lead_bytes < (u32)(block_align * 4)) lead_bytes = block_align * 4;
    if (buf->size_bytes > 0 && lead_bytes > buf->size_bytes / 2) lead_bytes = buf->size_bytes / 2;
    u32 write_pos = play_pos + lead_bytes;
    if (buf->size_bytes > 0) write_pos %= buf->size_bytes;
    return (write_pos / block_align) * block_align;
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
        sound_lock();
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
        sound_unlock();
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

    free(buf);

    if (ds) {
        if (--ds->ref_count == 0) {
            sound_lock();
            for (int i = 0; i < g_num_ds_instances; i++) {
                if (g_ds_instances[i] == ds) {
                    g_ds_instances[i] = g_ds_instances[--g_num_ds_instances];
                    break;
                }
            }
            sound_unlock();
            free(ds);
        }
    }

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

    sound_lock();
    int block_align = buf->format.nBlockAlign > 0 ? buf->format.nBlockAlign : 1;

    u32 play_pos = 0;
    if (buf->size_bytes > 0) {
        play_pos = (u32)buf->play_cursor_bytes;
        play_pos = (play_pos / block_align) * block_align;
        play_pos %= buf->size_bytes;
    }

    u32 write_pos = calc_write_cursor(buf, play_pos);
    sound_unlock();

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

    sound_lock();
    u32 st = buf->status;
    sound_unlock();

    wr32(pdwStatus, st);
    return DS_OK;
}

/* vtable index 10: Initialize(this, pDirectSound, pcDSBufferDesc) */
static u32 imp_dsb_Initialize(Cpu *c)
{
    (void)c;
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

    sound_lock();
    int block_align = buf->format.nBlockAlign > 0 ? buf->format.nBlockAlign : 1;

    if (dwFlags & DSBLOCK_FROMWRITECURSOR) {
        u32 play_pos = 0;
        if (buf->size_bytes > 0) {
            play_pos = (u32)buf->play_cursor_bytes;
            play_pos = (play_pos / block_align) * block_align;
            play_pos %= buf->size_bytes;
        }
        dwOffset = calc_write_cursor(buf, play_pos);
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
    sound_unlock();

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

    sound_lock();
    buf->status = DSBSTATUS_PLAYING;
    if (dwFlags & DSBPLAY_LOOPING) {
        buf->status |= DSBSTATUS_LOOPING;
    }
    sound_unlock();
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

    sound_lock();
    if (buf->size_bytes > 0) dwNewPosition %= buf->size_bytes;
    int block_align = buf->format.nBlockAlign > 0 ? buf->format.nBlockAlign : 1;
    dwNewPosition = (dwNewPosition / block_align) * block_align;
    buf->play_cursor_bytes = (double)dwNewPosition;
    sound_unlock();
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

    if (lVolume > 0) lVolume = 0;
    if (lVolume < -10000) lVolume = -10000;

    sound_lock();
    buf->volume = lVolume;
    sound_unlock();
    return DS_OK;
}

/* vtable index 16: SetPan(this, lPan) */
static u32 imp_dsb_SetPan(Cpu *c)
{
    u32 this_obj = ARG(0);
    s32 lPan = (s32)ARG(1);
    struct DsbHost *buf = (struct DsbHost *)com_host(this_obj);
    if (!buf) return E_FAIL;

    if (lPan > 10000) lPan = 10000;
    if (lPan < -10000) lPan = -10000;

    sound_lock();
    buf->pan = lPan;
    sound_unlock();
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

    sound_lock();
    buf->current_frequency = dwFrequency;
    sound_unlock();
    return DS_OK;
}

/* vtable index 18: Stop(this) */
static u32 imp_dsb_Stop(Cpu *c)
{
    u32 this_obj = ARG(0);
    struct DsbHost *buf = (struct DsbHost *)com_host(this_obj);
    if (!buf) return E_FAIL;

    sound_lock();
    buf->status = 0;
    sound_unlock();
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
        sound_lock();
        for (int i = 0; i < g_num_ds_instances; i++) {
            if (g_ds_instances[i] == ds) {
                g_ds_instances[i] = g_ds_instances[--g_num_ds_instances];
                break;
            }
        }
        sound_unlock();
        free(ds);
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
        buf->format.nSamplesPerSec = 44100;
        buf->format.wBitsPerSample = 16;
        buf->format.nBlockAlign = 4;
        buf->format.nAvgBytesPerSec = 44100 * 4;
        buf->current_frequency = 44100;

        sound_lock();
        ds->primary_buf = buf;
        sound_unlock();
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

        sound_lock();
        if (ds->num_secondary_bufs < MAX_DSOUND_BUFFERS) {
            ds->secondary_bufs[ds->num_secondary_bufs++] = buf;
        } else {
            plat_log("warn: IDirectSound::CreateSoundBuffer max secondary buffers exceeded");
        }
        sound_unlock();
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

    sound_lock();
    if (ds->num_secondary_bufs < MAX_DSOUND_BUFFERS) {
        ds->secondary_bufs[ds->num_secondary_bufs++] = dup;
    } else {
        plat_log("warn: IDirectSound::DuplicateSoundBuffer max secondary buffers reached");
    }
    sound_unlock();

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

    ensure_audio_device_opened();

    struct DsHost *ds = (struct DsHost *)calloc(1, sizeof(*ds));
    if (!ds) return DSERR_OUTOFMEMORY;

    ds->ref_count = 1;
    ds->speaker_config = 4; /* DSSPEAKER_STEREO */
    ds->coop_level = 1;     /* DSSCL_NORMAL */

    sound_lock();
    if (g_num_ds_instances < MAX_DS_INSTANCES) {
        g_ds_instances[g_num_ds_instances++] = ds;
    }
    sound_unlock();

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
