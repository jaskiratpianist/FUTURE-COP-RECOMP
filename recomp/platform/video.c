#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <SDL.h>

#define PL_MPEG_IMPLEMENTATION
#include "third_party/pl_mpeg.h"

#include "host.h"
#include "win.h"
#include "sound.h"
#include "video.h"

/* GUID definition */
struct Guid {
    u32 data1;
    u16 data2;
    u16 data3;
    u8  data4[8];
};

static const struct Guid IID_IUnknown = {
    0x00000000, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 }
};
static const struct Guid IID_IDispatch = {
    0x00020400, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 }
};
static const struct Guid IID_IFilterGraph = {
    0x56a868a1, 0x0ad4, 0x11ce, { 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 }
};
static const struct Guid IID_IGraphBuilder = {
    0x56a868a9, 0x0ad4, 0x11ce, { 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 }
};
static const struct Guid IID_IMediaControl = {
    0x56a868b1, 0x0ad4, 0x11ce, { 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 }
};
static const struct Guid IID_IMediaEvent = {
    0x56a868b6, 0x0ad4, 0x11ce, { 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 }
};
static const struct Guid IID_IMediaEventEx = {
    0x56a868c0, 0x0ad4, 0x11ce, { 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 }
};
static const struct Guid IID_IVideoWindow = {
    0x56a868b4, 0x0ad4, 0x11ce, { 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 }
};
static const struct Guid IID_IBasicAudio = {
    0x56a868b3, 0x0ad4, 0x11ce, { 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 }
};
static const struct Guid IID_IBasicVideo = {
    0x56a868b5, 0x0ad4, 0x11ce, { 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 }
};
static const struct Guid IID_IMediaPosition = {
    0x56a868b2, 0x0ad4, 0x11ce, { 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 }
};
static const struct Guid IID_IMediaSeeking = {
    0x363930b0, 0x62d3, 0x11cf, { 0x88, 0x4d, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24 }
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


typedef struct FilterGraph {
    u32 ref_count;

    u32 obj_graph_builder;
    u32 obj_media_control;
    u32 obj_media_event;
    u32 obj_video_window;
    u32 obj_basic_audio;
    u32 obj_media_seeking;
    u32 obj_media_position;

    plm_t *plm;
    char path[512];
    volatile int state; /* 0 = stopped, 1 = paused, 2 = running */
    volatile int ended;
    volatile int skipped;
    int ec_complete_delivered;

    int has_audio;
    int audio_rate;
    int audio_channels;

    double framerate;
    double duration;
    int video_w;
    int video_h;
    uint8_t *rgb_buf;

    s32 volume;
    s32 balance;

    u32 window_style;
    u32 window_style_ex;
    u32 owner_hwnd;
    u32 drain_hwnd;
    int auto_show;
    int video_visible;
    int win_x, win_y, win_w, win_h;

    pthread_t thread;
    int thread_active;

    u32 guest_event_handle;
    u32 notify_hwnd;
    u32 notify_msg;
    u32 notify_inst_data;
} FilterGraph;

static FilterGraph *g_active_graph = NULL;
static u16 g_out_frame[640 * 480];

static u32 graph_QueryInterface(FilterGraph *fg, u32 riid, u32 ppv);
static int video_event_hook(const SDL_Event *e);

static void present_video_frame(FilterGraph *fg, plm_frame_t *frame)
{
    if (!frame || !fg->rgb_buf) return;

    int vw = frame->width;
    int vh = frame->height;
    if (vw <= 0 || vh <= 0) return;

    plm_frame_to_rgb(frame, fg->rgb_buf, vw * 3);

    const u16 *dummy_px = NULL;
    int dummy_w = 0, dummy_h = 0, is565 = 0;
    if (!win_last_frame(&dummy_px, &dummy_w, &dummy_h, &is565)) {
        is565 = 0; /* default 555 */
    }
    u32 rmask = is565 ? 0xf800 : 0x7c00;
    u32 gmask = is565 ? 0x07e0 : 0x03e0;
    u32 bmask = 0x001f;

    memset(g_out_frame, 0, sizeof(g_out_frame));

    int scaled_w = 640;
    int scaled_h = 480;
    if (vw * 480 <= 640 * vh) {
        scaled_h = 480;
        scaled_w = (vw * 480) / vh;
    } else {
        scaled_w = 640;
        scaled_h = (vh * 640) / vw;
    }

    int offset_x = (640 - scaled_w) / 2;
    int offset_y = (480 - scaled_h) / 2;

    for (int y = 0; y < scaled_h; y++) {
        int sy = (y * vh) / scaled_h;
        const uint8_t *src_row = fg->rgb_buf + sy * (vw * 3);
        u16 *dst_row = g_out_frame + (offset_y + y) * 640 + offset_x;
        for (int x = 0; x < scaled_w; x++) {
            int sx = (x * vw) / scaled_w;
            uint8_t r = src_row[sx * 3 + 0];
            uint8_t g = src_row[sx * 3 + 1];
            uint8_t b = src_row[sx * 3 + 2];
            if (is565) {
                dst_row[x] = (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            } else {
                dst_row[x] = (u16)(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
            }
        }
    }

    win_show(g_out_frame, 640, 480, 640 * sizeof(u16), 16, rmask, gmask, bmask);
}

static void *video_worker(void *arg)
{
    FilterGraph *fg = (FilterGraph *)arg;
    plat_log("DirectShow: video_worker started for '%s'", fg->path);

    if (fg->has_audio) {
        sound_stream_open(fg->audio_rate, fg->audio_channels);
        sound_stream_set_volume(fg->volume);
    }

    struct timespec start_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);

    plm_frame_t *frame = plm_decode_video(fg->plm);
    if (!frame) {
        plat_log("DirectShow: no video frames in '%s'", fg->path);
        fg->ended = 1;
        fg->state = 0;
        if (fg->has_audio) sound_stream_close();
        if (fg->guest_event_handle) sync_event_set(fg->guest_event_handle, 1);
        if (fg->notify_hwnd && fg->notify_msg) {
            msg_post(fg->notify_hwnd, fg->notify_msg, 0, fg->notify_inst_data);
        }
        return NULL;
    }

    double start_pts = -1.0;
    int frame_count = 0;

    while (fg->state == 2 && !fg->skipped) {
        if (fg->has_audio) {
            while (sound_stream_buffered_frames() < 22050) {
                plm_samples_t *samples = plm_decode_audio(fg->plm);
                if (!samples) break;
                s16 s16_buf[PLM_AUDIO_SAMPLES_PER_FRAME * 2];
                for (int i = 0; i < (int)samples->count * 2; i++) {
                    float f = samples->interleaved[i] * 32767.0f;
                    if (f > 32767.0f) f = 32767.0f;
                    else if (f < -32768.0f) f = -32768.0f;
                    s16_buf[i] = (s16)f;
                }
                sound_stream_write(s16_buf, samples->count);
            }
        }

        if (frame && start_pts < 0.0) {
            start_pts = frame->time;
            plat_log("DirectShow: first frame pts=%.3f", start_pts);
        }

        /* follow the audio clock, but never more than 0.25 s behind real time: an audio
           device that does not consume (no sound card, SDL's dummy driver) must not stall
           the video, or the game waits forever for the end of the intro */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double cur_time = (now.tv_sec - start_ts.tv_sec) + (now.tv_nsec - start_ts.tv_nsec) * 1e-9;
        if (fg->has_audio) {
            double audio_time = sound_stream_get_time();
            cur_time = audio_time > cur_time - 0.25 ? audio_time : cur_time - 0.25;
        }

        if (frame) {
            double target_time = frame->time - start_pts;
            if (cur_time < target_time) {
                double wait_sec = target_time - cur_time;
                if (wait_sec > 0.001) {
                    if (wait_sec > 0.02) wait_sec = 0.02;
                    usleep((useconds_t)(wait_sec * 1000000.0));
                    continue;
                }
            }

            present_video_frame(fg, frame);
            frame_count++;
            frame = plm_decode_video(fg->plm);
        }

        if (!frame) {
            static int drain_waits;  /* let the audio play out, at most 1 s */
            if (fg->has_audio && sound_stream_buffered_frames() > 0 && drain_waits++ < 100) {
                usleep(10000);
                continue;
            }
            drain_waits = 0;
            break;
        }
    }

    fg->ended = 1;
    fg->state = 0;
    if (fg->has_audio) {
        sound_stream_close();
    }

    if (fg->guest_event_handle) sync_event_set(fg->guest_event_handle, 1);
    if (fg->notify_hwnd && fg->notify_msg) {
        msg_post(fg->notify_hwnd, fg->notify_msg, 0, fg->notify_inst_data);
    }

    if (win_event_hook == video_event_hook) {
        win_event_hook = NULL;
    }

    plat_log("DirectShow: video_worker finished for '%s' (ended=%d, skipped=%d, frames=%d)",
             fg->path, fg->ended, fg->skipped, frame_count);
    return NULL;
}

static int video_event_hook(const SDL_Event *e)
{
    if (!g_active_graph || g_active_graph->state != 2) return 0;
    if (e->type == SDL_KEYDOWN) {
        SDL_Keycode k = e->key.keysym.sym;
        if (k == SDLK_ESCAPE || k == SDLK_SPACE || k == SDLK_RETURN || k == SDLK_KP_ENTER) {
            plat_log("DirectShow: video skipped via key %d", (int)k);
            g_active_graph->skipped = 1;
            g_active_graph->ended = 1;
            return 1;
        }
    } else if (e->type == SDL_MOUSEBUTTONDOWN) {
        plat_log("DirectShow: video skipped via mouse click");
        g_active_graph->skipped = 1;
        g_active_graph->ended = 1;
        return 1;
    }
    return 0;
}

/* Generic IUnknown methods */
static u32 imp_com_QueryInterface(Cpu *c)
{
    u32 this_obj = ARG(0);
    u32 riid = ARG(1);
    u32 ppv = ARG(2);
    FilterGraph *fg = (FilterGraph *)com_host(this_obj);
    if (!fg) return 0x80004005u;
    return graph_QueryInterface(fg, riid, ppv);
}

static u32 imp_com_AddRef(Cpu *c)
{
    u32 this_obj = ARG(0);
    FilterGraph *fg = (FilterGraph *)com_host(this_obj);
    if (!fg) return 0;
    fg->ref_count++;
    return fg->ref_count;
}

static u32 imp_com_Release(Cpu *c)
{
    u32 this_obj = ARG(0);
    FilterGraph *fg = (FilterGraph *)com_host(this_obj);
    if (!fg) return 0;
    if (fg->ref_count > 0) fg->ref_count--;
    u32 rc = fg->ref_count;
    if (rc == 0) {
        plat_log("DirectShow: releasing FilterGraph");
        if (win_event_hook == video_event_hook) {
            win_event_hook = NULL;
        }
        if (fg->thread_active) {
            fg->state = 0;
            pthread_join(fg->thread, NULL);
            fg->thread_active = 0;
        }
        if (fg->plm) {
            plm_destroy(fg->plm);
            fg->plm = NULL;
        }
        if (fg->rgb_buf) {
            free(fg->rgb_buf);
            fg->rgb_buf = NULL;
        }
        sound_stream_close();
        if (fg->guest_event_handle) sync_event_set(fg->guest_event_handle, 0);
        if (g_active_graph == fg) g_active_graph = NULL;
        free(fg);
    }
    return rc;
}

/* Generic IDispatch methods */
static u32 imp_disp_GetTypeInfoCount(Cpu *c)
{
    u32 pct = ARG(1);
    if (pct) wr32(pct, 0);
    return 0; /* S_OK */
}

static u32 imp_disp_GetTypeInfo(Cpu *c) { (void)c; return 0x80004001u; /* E_NOTIMPL */ }
static u32 imp_disp_GetIDsOfNames(Cpu *c) { (void)c; return 0x80004001u; /* E_NOTIMPL */ }
static u32 imp_disp_Invoke(Cpu *c) { (void)c; return 0x80004001u; /* E_NOTIMPL */ }

/* -------------------------------------------------------------------------
 * IGraphBuilder / IFilterGraph methods
 * ------------------------------------------------------------------------- */
static u32 imp_fg_AddFilter(Cpu *c) { (void)c; return 0; }
static u32 imp_fg_RemoveFilter(Cpu *c) { (void)c; return 0; }
static u32 imp_fg_EnumFilters(Cpu *c) { (void)c; return 0x80004001u; }
static u32 imp_fg_FindFilterByName(Cpu *c) { (void)c; return 0x80004005u; }
static u32 imp_fg_ConnectDirect(Cpu *c) { (void)c; return 0; }
static u32 imp_fg_Reconnect(Cpu *c) { (void)c; return 0; }
static u32 imp_fg_Disconnect(Cpu *c) { (void)c; return 0; }
static u32 imp_fg_SetDefaultSyncSource(Cpu *c) { (void)c; return 0; }
static u32 imp_fg_Connect(Cpu *c) { (void)c; return 0; }
static u32 imp_fg_Render(Cpu *c) { (void)c; return 0; }

static u32 imp_fg_RenderFile(Cpu *c)
{
    u32 this_obj = ARG(0);
    u32 lpcwstrFile = ARG(1);
    FilterGraph *fg = (FilterGraph *)com_host(this_obj);
    if (!fg || !lpcwstrFile) return 0x80070057u; /* E_INVALIDARG */

    char path_ascii[512] = {0};
    int i = 0;
    while (i < 511) {
        u16 c16 = rd16(lpcwstrFile + i * 2);
        if (!c16) break;
        path_ascii[i] = (char)(c16 & 0x7f);
        i++;
    }
    path_ascii[i] = '\0';

    char full_path[512] = {0};
    const char *hp = host_path(path_ascii, full_path, sizeof(full_path));
    plat_log("IGraphBuilder::RenderFile: '%s' -> '%s'", path_ascii, hp ? hp : "NULL");
    if (!hp || !*hp) return 0x80004005u; /* E_FAIL */

    if (fg->thread_active) {
        fg->state = 0;
        pthread_join(fg->thread, NULL);
        fg->thread_active = 0;
    }
    if (fg->plm) {
        plm_destroy(fg->plm);
        fg->plm = NULL;
    }
    if (fg->rgb_buf) {
        free(fg->rgb_buf);
        fg->rgb_buf = NULL;
    }

    fg->plm = plm_create_with_filename(hp);
    if (!fg->plm) {
        plat_log("IGraphBuilder::RenderFile: plm_create_with_filename failed for '%s'", hp);
        return 0x80004005u;
    }

    strncpy(fg->path, hp, sizeof(fg->path) - 1);
    plm_probe(fg->plm, 1024 * 1024);
    plm_set_audio_enabled(fg->plm, TRUE);
    fg->framerate = plm_get_framerate(fg->plm);
    if (fg->framerate <= 0.0) fg->framerate = 15.0;
    fg->video_w = plm_get_width(fg->plm);
    fg->video_h = plm_get_height(fg->plm);
    fg->has_audio = (plm_get_num_audio_streams(fg->plm) > 0);
    fg->audio_rate = plm_get_samplerate(fg->plm);
    if (fg->audio_rate <= 0) fg->audio_rate = 44100;
    fg->audio_channels = 2;
    fg->duration = plm_get_duration(fg->plm);

    if (fg->video_w > 0 && fg->video_h > 0) {
        fg->rgb_buf = (uint8_t *)malloc(fg->video_w * fg->video_h * 3);
    }

    fg->state = 0;
    fg->ended = 0;
    fg->skipped = 0;
    fg->ec_complete_delivered = 0;

    plat_log("DirectShow: loaded '%s' (%dx%d, %.2f fps, duration %.2f s, audio: %d @ %d Hz)",
             hp, fg->video_w, fg->video_h, fg->framerate, fg->duration, fg->has_audio, fg->audio_rate);

    return 0; /* S_OK */
}

static u32 imp_fg_AddSourceFilter(Cpu *c) { (void)c; return 0x80004001u; }
static u32 imp_fg_SetLogFile(Cpu *c) { (void)c; return 0; }
static u32 imp_fg_Abort(Cpu *c) { (void)c; return 0; }
static u32 imp_fg_ShouldOperationContinue(Cpu *c) { (void)c; return 0; /* S_OK */ }

static const struct ImpDef graph_builder_methods[] = {
    /* 0 */ IMPORT(com_QueryInterface, 3),
    /* 1 */ IMPORT(com_AddRef, 1),
    /* 2 */ IMPORT(com_Release, 1),
    /* 3 */ IMPORT(fg_AddFilter, 3),
    /* 4 */ IMPORT(fg_RemoveFilter, 2),
    /* 5 */ IMPORT(fg_EnumFilters, 2),
    /* 6 */ IMPORT(fg_FindFilterByName, 3),
    /* 7 */ IMPORT(fg_ConnectDirect, 4),
    /* 8 */ IMPORT(fg_Reconnect, 2),
    /* 9 */ IMPORT(fg_Disconnect, 2),
    /* 10 */ IMPORT(fg_SetDefaultSyncSource, 1),
    /* 11 */ IMPORT(fg_Connect, 3),
    /* 12 */ IMPORT(fg_Render, 2),
    /* 13 */ IMPORT(fg_RenderFile, 3),
    /* 14 */ IMPORT(fg_AddSourceFilter, 4),
    /* 15 */ IMPORT(fg_SetLogFile, 2),
    /* 16 */ IMPORT(fg_Abort, 1),
    /* 17 */ IMPORT(fg_ShouldOperationContinue, 1),
};

/* -------------------------------------------------------------------------
 * IMediaControl methods
 * ------------------------------------------------------------------------- */
static u32 imp_mc_Run(Cpu *c)
{
    u32 this_obj = ARG(0);
    FilterGraph *fg = (FilterGraph *)com_host(this_obj);
    plat_log("IMediaControl::Run()");
    if (!fg) return 0x80004005u;
    if (fg->state == 2) return 0;

    fg->state = 2; /* running */
    fg->ended = 0;
    fg->skipped = 0;
    fg->ec_complete_delivered = 0;

    win_event_hook = video_event_hook;

    if (pthread_create(&fg->thread, NULL, video_worker, fg) == 0) {
        fg->thread_active = 1;
    } else {
        plat_log("DirectShow: pthread_create failed");
        fg->state = 0;
        return 0x80004005u;
    }
    return 0; /* S_OK */
}

static u32 imp_mc_Pause(Cpu *c)
{
    u32 this_obj = ARG(0);
    FilterGraph *fg = (FilterGraph *)com_host(this_obj);
    plat_log("IMediaControl::Pause()");
    if (!fg) return 0x80004005u;
    fg->state = 1;
    return 0;
}

static u32 imp_mc_Stop(Cpu *c)
{
    u32 this_obj = ARG(0);
    FilterGraph *fg = (FilterGraph *)com_host(this_obj);
    plat_log("IMediaControl::Stop()");
    if (!fg) return 0x80004005u;
    fg->state = 0;
    if (win_event_hook == video_event_hook) {
        win_event_hook = NULL;
    }
    if (fg->thread_active) {
        pthread_join(fg->thread, NULL);
        fg->thread_active = 0;
    }
    sound_stream_close();
    return 0;
}

static u32 imp_mc_GetState(Cpu *c)
{
    u32 this_obj = ARG(0);
    u32 pfs = ARG(2);
    FilterGraph *fg = (FilterGraph *)com_host(this_obj);
    plat_log("IMediaControl::GetState()");
    if (pfs) wr32(pfs, fg ? fg->state : 0);
    return 0;
}

static u32 imp_mc_RenderFile(Cpu *c) { return imp_fg_RenderFile(c); }
static u32 imp_mc_AddSourceFilter(Cpu *c) { (void)c; return 0x80004001u; }
static u32 imp_mc_get_FilterCollection(Cpu *c) { (void)c; return 0x80004001u; }
static u32 imp_mc_get_RegFilterCollection(Cpu *c) { (void)c; return 0x80004001u; }
static u32 imp_mc_StopWhenReady(Cpu *c) { (void)c; return 0; }

static const struct ImpDef media_control_methods[] = {
    /* 0 */ IMPORT(com_QueryInterface, 3),
    /* 1 */ IMPORT(com_AddRef, 1),
    /* 2 */ IMPORT(com_Release, 1),
    /* 3 */ IMPORT(disp_GetTypeInfoCount, 2),
    /* 4 */ IMPORT(disp_GetTypeInfo, 4),
    /* 5 */ IMPORT(disp_GetIDsOfNames, 6),
    /* 6 */ IMPORT(disp_Invoke, 9),
    /* 7 */ IMPORT(mc_Run, 1),
    /* 8 */ IMPORT(mc_Pause, 1),
    /* 9 */ IMPORT(mc_Stop, 1),
    /* 10 */ IMPORT(mc_GetState, 3),
    /* 11 */ IMPORT(mc_RenderFile, 2),
    /* 12 */ IMPORT(mc_AddSourceFilter, 3),
    /* 13 */ IMPORT(mc_get_FilterCollection, 2),
    /* 14 */ IMPORT(mc_get_RegFilterCollection, 2),
    /* 15 */ IMPORT(mc_StopWhenReady, 1),
};

/* -------------------------------------------------------------------------
 * IMediaEvent / IMediaEventEx methods
 * ------------------------------------------------------------------------- */
static u32 imp_me_GetEventHandle(Cpu *c)
{
    u32 this_obj = ARG(0);
    u32 phEvent = ARG(1);
    FilterGraph *fg = (FilterGraph *)com_host(this_obj);
    plat_log("IMediaEvent::GetEventHandle(phEvent=0x%x) -> handle 0x%x",
             phEvent, fg ? fg->guest_event_handle : 0);
    if (!fg || !phEvent) return 0x80070057u;
    wr32(phEvent, fg->guest_event_handle);
    return 0;
}

static u32 imp_me_GetEvent(Cpu *c)
{
    u32 this_obj = ARG(0);
    u32 plEventCode = ARG(1);
    u32 plParam1 = ARG(2);
    u32 plParam2 = ARG(3);
    u32 msTimeout = ARG(4);
    FilterGraph *fg = (FilterGraph *)com_host(this_obj);
    plat_log("IMediaEvent::GetEvent(timeout=%u, ended=%d, delivered=%d)",
             msTimeout, fg ? fg->ended : 0, fg ? fg->ec_complete_delivered : 0);
    if (!fg) return 0x80004005u;

    if (fg->ended && !fg->ec_complete_delivered) {
        if (plEventCode) wr32(plEventCode, 1); /* EC_COMPLETE = 1 */
        if (plParam1) wr32(plParam1, 0);       /* S_OK */
        if (plParam2) wr32(plParam2, 0);
        fg->ec_complete_delivered = 1;
        if (fg->guest_event_handle) sync_event_set(fg->guest_event_handle, 0);
        return 0; /* S_OK */
    }

    if (msTimeout > 0 && !fg->ended) {
        u32 start_tick = SDL_GetTicks();
        while (fg->thread_active && fg->state == 2 && !fg->ended) {
            plat_pump();
            if (SDL_GetTicks() - start_tick >= msTimeout) break;
            usleep(2000);
        }
        if (fg->ended && !fg->ec_complete_delivered) {
            if (plEventCode) wr32(plEventCode, 1);
            if (plParam1) wr32(plParam1, 0);
            if (plParam2) wr32(plParam2, 0);
            fg->ec_complete_delivered = 1;
            return 0;
        }
    }

    return 0x80040227u; /* VFW_E_TIMEOUT */
}

static u32 imp_me_WaitForCompletion(Cpu *c)
{
    u32 this_obj = ARG(0);
    u32 msTimeout = ARG(1);
    u32 pEvCode = ARG(2);
    FilterGraph *fg = (FilterGraph *)com_host(this_obj);
    plat_log("IMediaEvent::WaitForCompletion(timeout=%u)", msTimeout);
    if (!fg) return 0x80004005u;

    u32 start_tick = SDL_GetTicks();
    while (fg->thread_active && fg->state == 2 && !fg->ended) {
        plat_pump();
        if (msTimeout != (u32)-1 && msTimeout != 0xffffffffu) {
            if (SDL_GetTicks() - start_tick >= msTimeout) {
                if (pEvCode) wr32(pEvCode, 0);
                return 0x80040227u; /* VFW_E_TIMEOUT */
            }
        }
        usleep(2000);
    }
    if (fg->thread_active) {
        pthread_join(fg->thread, NULL);
        fg->thread_active = 0;
    }
    if (pEvCode) wr32(pEvCode, 1); /* EC_COMPLETE */
    return 0;
}

static u32 imp_me_CancelDefaultHandling(Cpu *c) { (void)c; return 0; }
static u32 imp_me_RestoreDefaultHandling(Cpu *c) { (void)c; return 0; }
static u32 imp_me_FreeEventParams(Cpu *c) { (void)c; return 0; }

static u32 imp_me_SetNotifyWindow(Cpu *c)
{
    u32 this_obj = ARG(0);
    u32 hwnd = ARG(1);
    u32 lMsg = ARG(2);
    u32 lInstanceData = ARG(3);
    FilterGraph *fg = (FilterGraph *)com_host(this_obj);
    plat_log("IMediaEventEx::SetNotifyWindow(hwnd=0x%x, msg=0x%x, inst=0x%x)", hwnd, lMsg, lInstanceData);
    if (fg) {
        fg->notify_hwnd = hwnd;
        fg->notify_msg = lMsg;
        fg->notify_inst_data = lInstanceData;
    }
    return 0;
}

static u32 imp_me_SetNotifyFlags(Cpu *c) { (void)c; return 0; }
static u32 imp_me_GetNotifyFlags(Cpu *c) { (void)c; return 0; }

static const struct ImpDef media_event_methods[] = {
    /* 0 */ IMPORT(com_QueryInterface, 3),
    /* 1 */ IMPORT(com_AddRef, 1),
    /* 2 */ IMPORT(com_Release, 1),
    /* 3 */ IMPORT(disp_GetTypeInfoCount, 2),
    /* 4 */ IMPORT(disp_GetTypeInfo, 4),
    /* 5 */ IMPORT(disp_GetIDsOfNames, 6),
    /* 6 */ IMPORT(disp_Invoke, 9),
    /* 7 */ IMPORT(me_GetEventHandle, 2),
    /* 8 */ IMPORT(me_GetEvent, 5),
    /* 9 */ IMPORT(me_WaitForCompletion, 3),
    /* 10 */ IMPORT(me_CancelDefaultHandling, 2),
    /* 11 */ IMPORT(me_RestoreDefaultHandling, 2),
    /* 12 */ IMPORT(me_FreeEventParams, 4),
    /* 13 */ IMPORT(me_SetNotifyWindow, 4),
    /* 14 */ IMPORT(me_SetNotifyFlags, 2),
    /* 15 */ IMPORT(me_GetNotifyFlags, 2),
};

/* -------------------------------------------------------------------------
 * IVideoWindow methods
 * ------------------------------------------------------------------------- */
static u32 imp_vw_put_Caption(Cpu *c) { (void)c; return 0; }
static u32 imp_vw_get_Caption(Cpu *c) { if (ARG(1)) wr32(ARG(1), 0); return 0; }

static u32 imp_vw_put_WindowStyle(Cpu *c)
{
    FilterGraph *fg = (FilterGraph *)com_host(ARG(0));
    if (fg) fg->window_style = ARG(1);
    return 0;
}

static u32 imp_vw_get_WindowStyle(Cpu *c)
{
    FilterGraph *fg = (FilterGraph *)com_host(ARG(0));
    if (ARG(1)) wr32(ARG(1), fg ? fg->window_style : 0);
    return 0;
}

static u32 imp_vw_put_WindowStyleEx(Cpu *c)
{
    FilterGraph *fg = (FilterGraph *)com_host(ARG(0));
    if (fg) fg->window_style_ex = ARG(1);
    return 0;
}

static u32 imp_vw_get_WindowStyleEx(Cpu *c)
{
    FilterGraph *fg = (FilterGraph *)com_host(ARG(0));
    if (ARG(1)) wr32(ARG(1), fg ? fg->window_style_ex : 0);
    return 0;
}

static u32 imp_vw_put_AutoShow(Cpu *c)
{
    FilterGraph *fg = (FilterGraph *)com_host(ARG(0));
    if (fg) fg->auto_show = (int)ARG(1);
    return 0;
}

static u32 imp_vw_get_AutoShow(Cpu *c)
{
    FilterGraph *fg = (FilterGraph *)com_host(ARG(0));
    if (ARG(1)) wr32(ARG(1), fg ? (u32)fg->auto_show : 0);
    return 0;
}

static u32 imp_vw_put_WindowState(Cpu *c) { (void)c; return 0; }
static u32 imp_vw_get_WindowState(Cpu *c) { if (ARG(1)) wr32(ARG(1), 1); return 0; }
static u32 imp_vw_put_BackgroundPalette(Cpu *c) { (void)c; return 0; }
static u32 imp_vw_get_BackgroundPalette(Cpu *c) { if (ARG(1)) wr32(ARG(1), 0); return 0; }

static u32 imp_vw_put_Visible(Cpu *c)
{
    FilterGraph *fg = (FilterGraph *)com_host(ARG(0));
    if (fg) fg->video_visible = (int)ARG(1);
    return 0;
}

static u32 imp_vw_get_Visible(Cpu *c)
{
    FilterGraph *fg = (FilterGraph *)com_host(ARG(0));
    if (ARG(1)) wr32(ARG(1), fg ? (u32)fg->video_visible : 0);
    return 0;
}

static u32 imp_vw_put_Left(Cpu *c)
{
    FilterGraph *fg = (FilterGraph *)com_host(ARG(0));
    if (fg) fg->win_x = (int)ARG(1);
    return 0;
}

static u32 imp_vw_get_Left(Cpu *c)
{
    FilterGraph *fg = (FilterGraph *)com_host(ARG(0));
    if (ARG(1)) wr32(ARG(1), fg ? (u32)fg->win_x : 0);
    return 0;
}

static u32 imp_vw_put_Width(Cpu *c)
{
    FilterGraph *fg = (FilterGraph *)com_host(ARG(0));
    if (fg) fg->win_w = (int)ARG(1);
    return 0;
}

static u32 imp_vw_get_Width(Cpu *c)
{
    FilterGraph *fg = (FilterGraph *)com_host(ARG(0));
    if (ARG(1)) wr32(ARG(1), fg ? (u32)fg->win_w : 0);
    return 0;
}

static u32 imp_vw_put_Top(Cpu *c)
{
    FilterGraph *fg = (FilterGraph *)com_host(ARG(0));
    if (fg) fg->win_y = (int)ARG(1);
    return 0;
}

static u32 imp_vw_get_Top(Cpu *c)
{
    FilterGraph *fg = (FilterGraph *)com_host(ARG(0));
    if (ARG(1)) wr32(ARG(1), fg ? (u32)fg->win_y : 0);
    return 0;
}

static u32 imp_vw_put_Height(Cpu *c)
{
    FilterGraph *fg = (FilterGraph *)com_host(ARG(0));
    if (fg) fg->win_h = (int)ARG(1);
    return 0;
}

static u32 imp_vw_get_Height(Cpu *c)
{
    FilterGraph *fg = (FilterGraph *)com_host(ARG(0));
    if (ARG(1)) wr32(ARG(1), fg ? (u32)fg->win_h : 0);
    return 0;
}

static u32 imp_vw_put_Owner(Cpu *c)
{
    FilterGraph *fg = (FilterGraph *)com_host(ARG(0));
    if (fg) fg->owner_hwnd = ARG(1);
    return 0;
}

static u32 imp_vw_get_Owner(Cpu *c)
{
    FilterGraph *fg = (FilterGraph *)com_host(ARG(0));
    if (ARG(1)) wr32(ARG(1), fg ? fg->owner_hwnd : 0);
    return 0;
}

static u32 imp_vw_put_MessageDrain(Cpu *c)
{
    FilterGraph *fg = (FilterGraph *)com_host(ARG(0));
    if (fg) fg->drain_hwnd = ARG(1);
    return 0;
}

static u32 imp_vw_get_MessageDrain(Cpu *c)
{
    FilterGraph *fg = (FilterGraph *)com_host(ARG(0));
    if (ARG(1)) wr32(ARG(1), fg ? fg->drain_hwnd : 0);
    return 0;
}

static u32 imp_vw_get_BorderColor(Cpu *c) { if (ARG(1)) wr32(ARG(1), 0); return 0; }
static u32 imp_vw_put_BorderColor(Cpu *c) { (void)c; return 0; }
static u32 imp_vw_get_FullScreenMode(Cpu *c) { if (ARG(1)) wr32(ARG(1), 0); return 0; }
static u32 imp_vw_put_FullScreenMode(Cpu *c) { (void)c; return 0; }
static u32 imp_vw_SetWindowForeground(Cpu *c) { (void)c; return 0; }
static u32 imp_vw_NotifyOwnerMessage(Cpu *c) { (void)c; return 0; }

static u32 imp_vw_SetWindowPosition(Cpu *c)
{
    FilterGraph *fg = (FilterGraph *)com_host(ARG(0));
    if (fg) {
        fg->win_x = (int)ARG(1);
        fg->win_y = (int)ARG(2);
        fg->win_w = (int)ARG(3);
        fg->win_h = (int)ARG(4);
    }
    return 0;
}

static u32 imp_vw_GetWindowPosition(Cpu *c)
{
    FilterGraph *fg = (FilterGraph *)com_host(ARG(0));
    if (ARG(1)) wr32(ARG(1), fg ? (u32)fg->win_x : 0);
    if (ARG(2)) wr32(ARG(2), fg ? (u32)fg->win_y : 0);
    if (ARG(3)) wr32(ARG(3), fg ? (u32)fg->win_w : 640);
    if (ARG(4)) wr32(ARG(4), fg ? (u32)fg->win_h : 480);
    return 0;
}

static u32 imp_vw_GetMinIdealImageSize(Cpu *c)
{
    FilterGraph *fg = (FilterGraph *)com_host(ARG(0));
    if (ARG(1)) wr32(ARG(1), fg ? (u32)fg->video_w : 320);
    if (ARG(2)) wr32(ARG(2), fg ? (u32)fg->video_h : 240);
    return 0;
}

static u32 imp_vw_GetMaxIdealImageSize(Cpu *c)
{
    if (ARG(1)) wr32(ARG(1), 640);
    if (ARG(2)) wr32(ARG(2), 480);
    return 0;
}

static u32 imp_vw_GetRestorePosition(Cpu *c)
{
    if (ARG(1)) wr32(ARG(1), 0);
    if (ARG(2)) wr32(ARG(2), 0);
    if (ARG(3)) wr32(ARG(3), 640);
    if (ARG(4)) wr32(ARG(4), 480);
    return 0;
}

static u32 imp_vw_HideCursor(Cpu *c) { (void)c; return 0; }
static u32 imp_vw_IsCursorHidden(Cpu *c) { if (ARG(1)) wr32(ARG(1), 0); return 0; }

static const struct ImpDef video_window_methods[] = {
    /* 0 */ IMPORT(com_QueryInterface, 3),
    /* 1 */ IMPORT(com_AddRef, 1),
    /* 2 */ IMPORT(com_Release, 1),
    /* 3 */ IMPORT(disp_GetTypeInfoCount, 2),
    /* 4 */ IMPORT(disp_GetTypeInfo, 4),
    /* 5 */ IMPORT(disp_GetIDsOfNames, 6),
    /* 6 */ IMPORT(disp_Invoke, 9),
    /* 7 */ IMPORT(vw_put_Caption, 2),
    /* 8 */ IMPORT(vw_get_Caption, 2),
    /* 9 */ IMPORT(vw_put_WindowStyle, 2),
    /* 10 */ IMPORT(vw_get_WindowStyle, 2),
    /* 11 */ IMPORT(vw_put_WindowStyleEx, 2),
    /* 12 */ IMPORT(vw_get_WindowStyleEx, 2),
    /* 13 */ IMPORT(vw_put_AutoShow, 2),
    /* 14 */ IMPORT(vw_get_AutoShow, 2),
    /* 15 */ IMPORT(vw_put_WindowState, 2),
    /* 16 */ IMPORT(vw_get_WindowState, 2),
    /* 17 */ IMPORT(vw_put_BackgroundPalette, 2),
    /* 18 */ IMPORT(vw_get_BackgroundPalette, 2),
    /* 19 */ IMPORT(vw_put_Visible, 2),
    /* 20 */ IMPORT(vw_get_Visible, 2),
    /* 21 */ IMPORT(vw_put_Left, 2),
    /* 22 */ IMPORT(vw_get_Left, 2),
    /* 23 */ IMPORT(vw_put_Width, 2),
    /* 24 */ IMPORT(vw_get_Width, 2),
    /* 25 */ IMPORT(vw_put_Top, 2),
    /* 26 */ IMPORT(vw_get_Top, 2),
    /* 27 */ IMPORT(vw_put_Height, 2),
    /* 28 */ IMPORT(vw_get_Height, 2),
    /* 29 */ IMPORT(vw_put_Owner, 2),
    /* 30 */ IMPORT(vw_get_Owner, 2),
    /* 31 */ IMPORT(vw_put_MessageDrain, 2),
    /* 32 */ IMPORT(vw_get_MessageDrain, 2),
    /* 33 */ IMPORT(vw_get_BorderColor, 2),
    /* 34 */ IMPORT(vw_put_BorderColor, 2),
    /* 35 */ IMPORT(vw_get_FullScreenMode, 2),
    /* 36 */ IMPORT(vw_put_FullScreenMode, 2),
    /* 37 */ IMPORT(vw_SetWindowForeground, 2),
    /* 38 */ IMPORT(vw_NotifyOwnerMessage, 5),
    /* 39 */ IMPORT(vw_SetWindowPosition, 5),
    /* 40 */ IMPORT(vw_GetWindowPosition, 5),
    /* 41 */ IMPORT(vw_GetMinIdealImageSize, 3),
    /* 42 */ IMPORT(vw_GetMaxIdealImageSize, 3),
    /* 43 */ IMPORT(vw_GetRestorePosition, 5),
    /* 44 */ IMPORT(vw_HideCursor, 2),
    /* 45 */ IMPORT(vw_IsCursorHidden, 2),
};

/* -------------------------------------------------------------------------
 * IBasicAudio methods
 * ------------------------------------------------------------------------- */
static u32 imp_ba_put_Volume(Cpu *c)
{
    u32 this_obj = ARG(0);
    s32 vol = (s32)ARG(1);
    FilterGraph *fg = (FilterGraph *)com_host(this_obj);
    if (fg) fg->volume = vol;
    plat_log("IBasicAudio::put_Volume(%d)", (int)vol);
    sound_stream_set_volume(vol);
    return 0;
}

static u32 imp_ba_get_Volume(Cpu *c)
{
    FilterGraph *fg = (FilterGraph *)com_host(ARG(0));
    if (ARG(1)) wr32(ARG(1), fg ? (u32)fg->volume : 0);
    return 0;
}

static u32 imp_ba_put_Balance(Cpu *c)
{
    FilterGraph *fg = (FilterGraph *)com_host(ARG(0));
    if (fg) fg->balance = (s32)ARG(1);
    return 0;
}

static u32 imp_ba_get_Balance(Cpu *c)
{
    FilterGraph *fg = (FilterGraph *)com_host(ARG(0));
    if (ARG(1)) wr32(ARG(1), fg ? (u32)fg->balance : 0);
    return 0;
}

static const struct ImpDef basic_audio_methods[] = {
    /* 0 */ IMPORT(com_QueryInterface, 3),
    /* 1 */ IMPORT(com_AddRef, 1),
    /* 2 */ IMPORT(com_Release, 1),
    /* 3 */ IMPORT(disp_GetTypeInfoCount, 2),
    /* 4 */ IMPORT(disp_GetTypeInfo, 4),
    /* 5 */ IMPORT(disp_GetIDsOfNames, 6),
    /* 6 */ IMPORT(disp_Invoke, 9),
    /* 7 */ IMPORT(ba_put_Volume, 2),
    /* 8 */ IMPORT(ba_get_Volume, 2),
    /* 9 */ IMPORT(ba_put_Balance, 2),
    /* 10 */ IMPORT(ba_get_Balance, 2),
};

/* -------------------------------------------------------------------------
 * IMediaSeeking methods
 * ------------------------------------------------------------------------- */
static u32 imp_ms_GetCapabilities(Cpu *c)
{
    if (ARG(1)) wr32(ARG(1), 0x3fu); /* CanSeekAbsolute | CanSeekForwards | CanSeekBackwards | CanGetDuration etc */
    return 0;
}

static u32 imp_ms_CheckCapabilities(Cpu *c) { (void)c; return 0; }
static u32 imp_ms_IsFormatSupported(Cpu *c) { (void)c; return 0; }
static u32 imp_ms_QueryPreferredFormat(Cpu *c) { (void)c; return 0; }
static u32 imp_ms_GetTimeFormat(Cpu *c) { (void)c; return 0; }
static u32 imp_ms_IsUsingTimeFormat(Cpu *c) { (void)c; return 0; }
static u32 imp_ms_SetTimeFormat(Cpu *c) { (void)c; return 0; }

static u32 imp_ms_GetDuration(Cpu *c)
{
    FilterGraph *fg = (FilterGraph *)com_host(ARG(0));
    u32 pDur = ARG(1);
    if (pDur) {
        u64 units = fg ? (u64)(fg->duration * 10000000.0) : 0;
        wr32(pDur, (u32)units);
        wr32(pDur + 4, (u32)(units >> 32));
    }
    return 0;
}

static u32 imp_ms_GetStopPosition(Cpu *c) { return imp_ms_GetDuration(c); }

static u32 imp_ms_GetCurrentPosition(Cpu *c)
{
    FilterGraph *fg = (FilterGraph *)com_host(ARG(0));
    u32 pCur = ARG(1);
    if (pCur) {
        double cur = fg ? (fg->has_audio ? sound_stream_get_time() : 0.0) : 0.0;
        u64 units = (u64)(cur * 10000000.0);
        wr32(pCur, (u32)units);
        wr32(pCur + 4, (u32)(units >> 32));
    }
    return 0;
}

static u32 imp_ms_ConvertTimeFormat(Cpu *c) { (void)c; return 0x80004001u; }
static u32 imp_ms_SetPositions(Cpu *c) { (void)c; return 0; }
static u32 imp_ms_GetPositions(Cpu *c) { (void)c; return 0; }
static u32 imp_ms_GetAvailable(Cpu *c) { (void)c; return 0; }
static u32 imp_ms_SetRate(Cpu *c) { (void)c; return 0; }

static u32 imp_ms_GetRate(Cpu *c)
{
    u32 pRate = ARG(1);
    if (pRate) {
        double r = 1.0;
        memcpy(G(pRate), &r, sizeof(double));
    }
    return 0;
}

static u32 imp_ms_GetPreroll(Cpu *c)
{
    u32 pPre = ARG(1);
    if (pPre) {
        wr32(pPre, 0);
        wr32(pPre + 4, 0);
    }
    return 0;
}

static const struct ImpDef media_seeking_methods[] = {
    /* 0 */ IMPORT(com_QueryInterface, 3),
    /* 1 */ IMPORT(com_AddRef, 1),
    /* 2 */ IMPORT(com_Release, 1),
    /* 3 */ IMPORT(ms_GetCapabilities, 2),
    /* 4 */ IMPORT(ms_CheckCapabilities, 2),
    /* 5 */ IMPORT(ms_IsFormatSupported, 2),
    /* 6 */ IMPORT(ms_QueryPreferredFormat, 2),
    /* 7 */ IMPORT(ms_GetTimeFormat, 2),
    /* 8 */ IMPORT(ms_IsUsingTimeFormat, 2),
    /* 9 */ IMPORT(ms_SetTimeFormat, 2),
    /* 10 */ IMPORT(ms_GetDuration, 2),
    /* 11 */ IMPORT(ms_GetStopPosition, 2),
    /* 12 */ IMPORT(ms_GetCurrentPosition, 2),
    /* 13 */ IMPORT(ms_ConvertTimeFormat, 6),
    /* 14 */ IMPORT(ms_SetPositions, 5),
    /* 15 */ IMPORT(ms_GetPositions, 3),
    /* 16 */ IMPORT(ms_GetAvailable, 3),
    /* 17 */ IMPORT(ms_SetRate, 3),
    /* 18 */ IMPORT(ms_GetRate, 2),
    /* 19 */ IMPORT(ms_GetPreroll, 2),
};

/* -------------------------------------------------------------------------
 * IMediaPosition methods
 * ------------------------------------------------------------------------- */
static u32 imp_mp_get_Duration(Cpu *c)
{
    FilterGraph *fg = (FilterGraph *)com_host(ARG(0));
    u32 pLen = ARG(1);
    if (pLen) {
        double d = fg ? fg->duration : 0.0;
        memcpy(G(pLen), &d, sizeof(double));
    }
    return 0;
}

static u32 imp_mp_put_CurrentPosition(Cpu *c) { (void)c; return 0; }

static u32 imp_mp_get_CurrentPosition(Cpu *c)
{
    FilterGraph *fg = (FilterGraph *)com_host(ARG(0));
    u32 pTime = ARG(1);
    if (pTime) {
        double cur = fg ? (fg->has_audio ? sound_stream_get_time() : 0.0) : 0.0;
        memcpy(G(pTime), &cur, sizeof(double));
    }
    return 0;
}

static u32 imp_mp_get_StopTime(Cpu *c) { return imp_mp_get_Duration(c); }
static u32 imp_mp_put_StopTime(Cpu *c) { (void)c; return 0; }

static u32 imp_mp_get_PrerollTime(Cpu *c)
{
    u32 pTime = ARG(1);
    if (pTime) {
        double zero = 0.0;
        memcpy(G(pTime), &zero, sizeof(double));
    }
    return 0;
}

static u32 imp_mp_put_PrerollTime(Cpu *c) { (void)c; return 0; }
static u32 imp_mp_put_Rate(Cpu *c) { (void)c; return 0; }

static u32 imp_mp_get_Rate(Cpu *c)
{
    u32 pdRate = ARG(1);
    if (pdRate) {
        double one = 1.0;
        memcpy(G(pdRate), &one, sizeof(double));
    }
    return 0;
}

static u32 imp_mp_CanSeekForward(Cpu *c)
{
    if (ARG(1)) wr32(ARG(1), 1);
    return 0;
}

static u32 imp_mp_CanSeekBackward(Cpu *c)
{
    if (ARG(1)) wr32(ARG(1), 1);
    return 0;
}

static const struct ImpDef media_position_methods[] = {
    /* 0 */ IMPORT(com_QueryInterface, 3),
    /* 1 */ IMPORT(com_AddRef, 1),
    /* 2 */ IMPORT(com_Release, 1),
    /* 3 */ IMPORT(disp_GetTypeInfoCount, 2),
    /* 4 */ IMPORT(disp_GetTypeInfo, 4),
    /* 5 */ IMPORT(disp_GetIDsOfNames, 6),
    /* 6 */ IMPORT(disp_Invoke, 9),
    /* 7 */ IMPORT(mp_get_Duration, 2),
    /* 8 */ IMPORT(mp_put_CurrentPosition, 3),
    /* 9 */ IMPORT(mp_get_CurrentPosition, 2),
    /* 10 */ IMPORT(mp_get_StopTime, 2),
    /* 11 */ IMPORT(mp_put_StopTime, 3),
    /* 12 */ IMPORT(mp_get_PrerollTime, 2),
    /* 13 */ IMPORT(mp_put_PrerollTime, 3),
    /* 14 */ IMPORT(mp_put_Rate, 3),
    /* 15 */ IMPORT(mp_get_Rate, 2),
    /* 16 */ IMPORT(mp_CanSeekForward, 2),
    /* 17 */ IMPORT(mp_CanSeekBackward, 2),
};

/* -------------------------------------------------------------------------
 * QueryInterface
 * ------------------------------------------------------------------------- */
static u32 graph_QueryInterface(FilterGraph *fg, u32 riid, u32 ppv)
{
    if (!ppv) return 0x80070057u; /* E_INVALIDARG */
    struct Guid g;
    read_guid(riid, &g);
    plat_log("FilterGraph::QueryInterface: {%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
             g.data1, g.data2, g.data3,
             g.data4[0], g.data4[1], g.data4[2], g.data4[3],
             g.data4[4], g.data4[5], g.data4[6], g.data4[7]);

    if (guid_equal(&g, &IID_IUnknown) ||
        guid_equal(&g, &IID_IFilterGraph) ||
        guid_equal(&g, &IID_IGraphBuilder)) {
        wr32(ppv, fg->obj_graph_builder);
        fg->ref_count++;
        return 0;
    }
    if (guid_equal(&g, &IID_IDispatch) ||
        guid_equal(&g, &IID_IMediaControl)) {
        wr32(ppv, fg->obj_media_control);
        fg->ref_count++;
        return 0;
    }
    if (guid_equal(&g, &IID_IMediaEvent) ||
        guid_equal(&g, &IID_IMediaEventEx)) {
        wr32(ppv, fg->obj_media_event);
        fg->ref_count++;
        return 0;
    }
    if (guid_equal(&g, &IID_IVideoWindow)) {
        wr32(ppv, fg->obj_video_window);
        fg->ref_count++;
        return 0;
    }
    if (guid_equal(&g, &IID_IBasicAudio)) {
        wr32(ppv, fg->obj_basic_audio);
        fg->ref_count++;
        return 0;
    }
    if (guid_equal(&g, &IID_IMediaSeeking)) {
        wr32(ppv, fg->obj_media_seeking);
        fg->ref_count++;
        return 0;
    }
    if (guid_equal(&g, &IID_IMediaPosition)) {
        wr32(ppv, fg->obj_media_position);
        fg->ref_count++;
        return 0;
    }

    plat_log("FilterGraph::QueryInterface: unsupported IID");
    wr32(ppv, 0);
    return 0x80004002u; /* E_NOINTERFACE */
}

u32 video_create_filter_graph(u32 riid, u32 ppv)
{
    if (getenv("FCOP_SKIP_VIDEOS")) {  /* no videos: the game skips them, as before */
        if (ppv) wr32(ppv, 0);
        return 0x80004002u;  /* E_NOINTERFACE */
    }
    plat_log("DirectShow: creating FilterGraph object");
    FilterGraph *fg = (FilterGraph *)calloc(1, sizeof(*fg));
    if (!fg) return 0x8007000eu; /* E_OUTOFMEMORY */

    fg->obj_graph_builder = com_new(graph_builder_methods, 18, fg);
    fg->obj_media_control = com_new(media_control_methods, 16, fg);
    fg->obj_media_event   = com_new(media_event_methods, 16, fg);
    fg->obj_video_window  = com_new(video_window_methods, 46, fg);
    fg->obj_basic_audio   = com_new(basic_audio_methods, 11, fg);
    fg->obj_media_seeking = com_new(media_seeking_methods, 20, fg);
    fg->obj_media_position = com_new(media_position_methods, 18, fg);

    fg->guest_event_handle = sync_event_new(1, 0);  /* set while an event is pending */

    fg->ref_count = 0;
    u32 hr = graph_QueryInterface(fg, riid, ppv);
    if (hr != 0) {
        if (fg->guest_event_handle) sync_event_set(fg->guest_event_handle, 0);
        free(fg);
        return hr;
    }

    g_active_graph = fg;
    return 0; /* S_OK */
}
