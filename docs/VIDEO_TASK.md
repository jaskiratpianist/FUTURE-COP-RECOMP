# Task: video playback (DirectShow on pl_mpeg)

The game (FCopLAPD.exe, 1998) has been translated to C and runs natively on Linux with SDL2;
the Windows functions it imports are implemented in `recomp/platform/`. Read
`recomp/platform/host.h`, `win.h`, `sound.h` first, with your file-reading tool.

**Read only files inside this repository** (system headers are blocked and reading one ends
your session). **Run the test command in the foreground and wait for its output**, never as
a background task. After every test, append one line (script, OK/PROBLEM, what you saw) to
`recomp/platform/video_progress.txt`: your session may end without warning.

## The situation

The game plays MPEG-1 videos (video 256x144 or similar, MP2 audio 44.1 kHz) through
DirectShow: intro movies at startup (ea_logo.mpg, intro.mpg), HELP (cwrules.mpg), BRIEFING
(brief1..8.mpg), district fly-overs (po_*.mpg), credits. The files are found now
(`host_path` falls back to the CD's Video folder), then the game calls
`CoCreateInstance(CLSID_FilterGraph {e436ebb3-524f-11ce-9f53-0020af0ba770}, ...,
IID_IGraphBuilder or similar)`, implemented in `recomp/platform/gdi_misc.c`
(`imp_CoCreateInstance`), which returns E_NOINTERFACE, so the game skips the video.

## What to build

1. A new file `recomp/platform/video.c` with a DirectShow filter graph object built with
   `com_new` (host.h). Decode with **pl_mpeg** (`recomp/platform/third_party/pl_mpeg.h`,
   already in the repository: `#define PL_MPEG_IMPLEMENTATION` in video.c only; API:
   `plm_create_with_filename`, `plm_set_audio_enabled`, `plm_get_framerate`,
   `plm_decode_video`, `plm_decode_audio`, `plm_frame_to_rgb`/`plm_frame_to_bgr`,
   `plm_has_ended`, `plm_destroy`; read the header's comment block for details).
2. Find out with `--api` (and by logging every QueryInterface IID in your object) exactly
   which interfaces and methods the game uses. Expected: IGraphBuilder (IFilterGraph:
   AddFilter, RemoveFilter, EnumFilters, FindFilterByName, ConnectDirect, Reconnect,
   Disconnect, SetDefaultSyncSource; IGraphBuilder adds Connect, Render, RenderFile,
   AddSourceFilter, SetLogFile, Abort, ShouldOperationContinue), IMediaControl (IDispatch's
   4 methods, then Run, Pause, Stop, GetState, RenderFile, AddSourceFilter, get_FilterCollection,
   get_RegFilterCollection, StopWhenReady), IMediaEvent/IMediaEventEx (IDispatch's 4, then
   GetEventHandle, GetEvent, WaitForCompletion, CancelDefaultHandling,
   RestoreDefaultHandling, FreeEventParams; Ex: SetNotifyWindow, SetNotifyFlags,
   GetNotifyFlags), IVideoWindow, IBasicAudio (put_Volume), IMediaPosition/IMediaSeeking.
   The IDispatch methods are GetTypeInfoCount, GetTypeInfo, GetIDsOfNames, Invoke. Check
   every vtable order against the DirectX Media 5 / DirectShow SDK headers you know (strmif.h,
   control.h); a wrong slot crashes or silently misbehaves.
3. `RenderFile(wide-char guest path)`: map with `host_path` (convert the UTF-16 path to
   ASCII first). `Run` starts playback; play on a host thread or from `plat_pump`-driven
   polling, but never call guest code from a host thread. Show each frame with
   `win_show` (win.h): convert to 16-bit in the same layout as the game's last frame
   (`win_last_frame` tells 565 or 555; default 555 masks 0x7C00/0x03E0/0x001F), scaled to
   fill 640x480 keeping the aspect ratio (black bars). Audio: add a small streaming API to
   dsound.c (declare it in sound.h), e.g. `sound_stream_open(rate, channels)`,
   `sound_stream_write(const s16 *samples, int frames)`, `sound_stream_close()`, mixed into
   the existing SDL callback; keep video in sync with the audio clock (or the frame rate if
   there is no audio).
4. Events: when the video ends, `WaitForCompletion` returns with EC_COMPLETE (1), and
   `GetEvent` reports EC_COMPLETE once; signal the event handle from `GetEventHandle` if the
   game waits on it (it is a guest-visible handle: use the event functions of
   k32_sync.c through whatever the game calls, or create one with `handle_new` so
   WaitForSingleObject works; read k32_sync.c to see how events are stored). Respect
   SetNotifyWindow (post the given message to the window with `msg_post`, win.h) if used.
5. The player must be able to **skip** a video with Escape, Space, Enter or a mouse click
   (stop and report completion), and closing the window must still quit the game.
6. CoCreateInstance in gdi_misc.c: for CLSID_FilterGraph call a function exported from
   video.c (declare it in a new `recomp/platform/video.h`); leave every other CLSID as it is.

## Files

You may create `video.c`, `video.h`, `video_progress.txt` and edit `dsound.c`, `sound.h` and
`imp_CoCreateInstance` in `gdi_misc.c`. Nothing else.

## Testing (the only shell command you may run)

    .venv/bin/python -I recomp/plat_test.py --seconds 20 --input "9000:click@362,306 14000:click@10,10" --png build/shots/brief.png --audio build/shots/brief.raw

clicks BRIEFING at 9 s (brief1.mpg) and saves the last frame and the audio levels; take
the PNG earlier (e.g. `--seconds 13`, no skip click) to see a video frame. HELP is
`click@156,276` (cwrules.mpg). The intro videos play at startup: with videos working, the
menu appears later, so to reach the menu in other tests skip them with clicks/Escape.
The game must still reach the main menu and start a mission (`click@484,303` once in the
menu) afterwards.

## Rules

- Plain C99 + SDL2 + POSIX (+ pl_mpeg). Helpers `static`. No empty stubs.
- Every shell command except the test command is blocked; if an action is denied, continue.

## Final reply

What works, what is partial, and the tests you ran.
