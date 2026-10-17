# Task: sound (DirectSound and PlaySoundA on SDL audio)

The game (FCopLAPD.exe, 1998) has been translated to C and runs natively on Linux with SDL2;
the Windows functions it imports are implemented in `recomp/platform/`. Read
`recomp/platform/host.h` (guest memory: `MEM`, `rd32`, `G(p)`, `galloc`; `ARG(n)`, `IMPORT`)
first, with your file-reading tool. Right now **the game is completely silent**.

## Files

- `recomp/platform/dsound.c` (DirectSound): written earlier by another worker and never
  tested. Review it carefully; fix it or rewrite parts as needed. Keep its import table.
- `recomp/platform/gdi_misc.c`: only the function `imp_PlaySoundA` (WINMM). Nothing else in it.
- You may add `recomp/platform/sound.h` for functions dsound.c exports to gdi_misc.c.

## What the game does

- Menu sounds: `PlaySoundA(id, hmod, flags)` with flags 0x40005 (SND_RESOURCE 0x40004 |
  SND_ASYNC 1) or 0: `id` <= 0xffff is a resource id of type **"WAVE"** (a named resource
  type, not numeric) in the EXE, each a complete RIFF/WAVE file. Find it in the loaded image
  in guest memory: base 0x400000, PE header at `0x400000 + rd32(0x40003c)`, resource
  directory = optional-header data directory 2 (RVA). Level 1 entries with a name (high
  bit of the name offset set: a length-prefixed UTF-16 string) "WAVE", level 2 the id,
  level 3 the first language. SND_ASYNC plays in the background; without it, return when
  done. A new PlaySoundA stops the previous one; `PlaySoundA(NULL, ...)` stops. SND_LOOP 8
  loops; SND_NOSTOP 0x10 does not interrupt. Also support SND_FILENAME 0x20000 (a file via
  `host_path`) and SND_MEMORY 4 (a guest pointer to a RIFF file).
- In missions and for music: DirectSound. `DirectSoundCreate`, `SetCooperativeLevel`,
  `CreateSoundBuffer` (a primary buffer with DSBCAPS_PRIMARYBUFFER 1, and secondary buffers
  with a WAVEFORMATEX: 8- or 16-bit PCM, mono or stereo, any rate), `Lock` (two parts when
  it wraps; pointers into guest memory), `Unlock`, `Play` (DSBPLAY_LOOPING 1), `Stop`,
  `SetCurrentPosition`, `GetCurrentPosition` (play cursor and write cursor: **the music is
  streamed**, the game refills the part behind the play cursor, so the cursors must advance
  in real time at the buffer's frequency, wrap correctly, and the write cursor be a little
  ahead of the play cursor), `GetStatus` (DSBSTATUS_PLAYING 1, LOOPING 4), `SetVolume`
  (hundredths of a dB, -10000..0: gain = 10^(v/2000)), `SetPan` (-10000 left .. 10000
  right), `SetFrequency` (0 = original), `GetVolume/GetPan/GetFrequency`, `GetCaps`,
  `GetFormat`, `DuplicateSoundBuffer` (shares the sample data, own position/volume/state),
  `Release` (free when the count reaches 0; stop playing first), `Restore`.
  Check the vtable order of every interface against the DirectX 5 SDK (IDirectSound:
  QueryInterface, AddRef, Release, CreateSoundBuffer, GetCaps, DuplicateSoundBuffer,
  SetCooperativeLevel, Compact, GetSpeakerConfig, SetSpeakerConfig, Initialize;
  IDirectSoundBuffer: QueryInterface, AddRef, Release, GetCaps, GetCurrentPosition,
  GetFormat, GetVolume, GetPan, GetFrequency, GetStatus, Initialize, Lock, Play,
  SetCurrentPosition, SetFormat, SetVolume, SetPan, SetFrequency, Stop, Unlock, Restore)
  and every structure offset (DSBUFFERDESC: dwSize 0, dwFlags 4, dwBufferBytes 8,
  dwReserved 12, lpwfxFormat 16; WAVEFORMATEX: wFormatTag 0, nChannels 2, nSamplesPerSec 4,
  nAvgBytesPerSec 8, nBlockAlign 12, wBitsPerSample 14; DSBCAPS: dwSize, dwFlags,
  dwBufferBytes, dwUnlockTransferRate, dwPlayCpuOverhead).

## How to build it

- One SDL audio device, opened once, 44100 Hz, stereo, signed 16-bit, ~1024-sample
  buffer, with a callback that mixes every playing sound (DirectSound buffers and the
  PlaySoundA sound): resample from each sound's own rate (linear interpolation is fine),
  8-bit samples are unsigned (128 = silence), apply volume and pan, clamp the sum. Log once:
  `audio device: <rate> Hz <channels> ch` (exactly this text; the test tool reads it).
- The callback runs on SDL's audio thread: it reads sample data straight from guest memory
  (`G(ptr)`), never calls guest code, and shares state with the import functions under one
  mutex (keep the locked parts short). Play cursors advance from the callback, in bytes of
  the buffer's own format, so GetCurrentPosition follows real time.
- Never block the game: Lock/Unlock/GetCurrentPosition must be fast.

## Testing (the only shell command you may run)

    .venv/bin/python -I recomp/plat_test.py --seconds 14 --input "9000:click@484,303" --audio build/shots/a.raw

records what the game plays (headless, nothing appears on screen) and prints the peak level
of every second (0-32767). Expect: clicks in the menu (PlaySoundA) give short peaks; a
mission (`--seconds 32 --input "9000:click@484,303 26000:Keypad_8*3000 28000:space*2000"`:
walk and fire) has music and effects, so most seconds are clearly above 0. Use
`--api` to see which DirectSound calls the game makes. The game must keep running at the
same speed (compare the frame numbers in `--png` runs with and without your changes).

## Rules

- Plain C99 + SDL2 + POSIX. Helpers `static`. No empty stubs.
- Read files with your file-reading tool, never with shell commands. Every shell command
  except the test command is blocked; if an action is denied, continue.

## Final reply

What works, what is partial, and the audio levels you measured (menu and mission).
