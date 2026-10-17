#!/usr/bin/env python3
"""Hand one platform file to agy (Gemini): recomp/plat_agy.py k32_sync [--model M]
or any other brief: recomp/plat_agy.py --task docs/DIALOG_TASK.md"""
import argparse, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GROUPS = {
    "k32_proc": ("recomp/platform/k32_proc.c", "KERNEL32 process, environment, strings, locale, modules",
        "GetVersion GetCommandLineA GetStartupInfoA GetModuleFileNameA GetModuleHandleA GetEnvironmentStrings "
        "GetEnvironmentStringsW FreeEnvironmentStringsA FreeEnvironmentStringsW SetEnvironmentVariableA "
        "SetEnvironmentVariableW GetStdHandle SetStdHandle GetFileType SetHandleCount GetACP GetOEMCP GetCPInfo "
        "MultiByteToWideChar WideCharToMultiByte LCMapStringA LCMapStringW GetStringTypeA GetStringTypeW "
        "CompareStringA CompareStringW GetLocaleInfoA GetLocaleInfoW GetCurrentProcess GetCurrentThreadId "
        "GetCurrentThread TerminateProcess ExitProcess UnhandledExceptionFilter RaiseException RtlUnwind SetErrorMode "
        "OutputDebugStringA DebugBreak LoadLibraryA GetProcAddress FreeLibrary GetComputerNameA lstrlenA lstrcpyA "
        "Beep GetVolumeInformationA GetDriveTypeA GetLogicalDrives GetSystemInfo GetLastError SetLastError",
        "Report Windows 98 (GetVersion: 0xC0000A04). Command line: \"FCopLAPD.exe\". Module file name: "
        "C:\\\\FCOP\\\\FCopLAPD.exe; GetModuleHandleA(NULL) = 0x400000. Code page 1252, locale English (US). "
        "Strings/locale only need to be right for ASCII/Latin-1. The game directory is drive C: (fixed disk, "
        "a CD drive D: may be reported too: the game may check for its CD, report D: as DRIVE_CDROM with "
        "volume name \"FCOP\"). LoadLibraryA returns 0 for every DLL for now (no 3dfx Glide yet), "
        "GetProcAddress returns 0. GetCurrentThreadId must differ per thread (use a per-thread counter). "
        "RaiseException/UnhandledExceptionFilter/DebugBreak: log and abort()."),
    "k32_mem": ("recomp/platform/k32_mem.c", "KERNEL32 memory, TLS, interlocked, critical sections",
        "HeapCreate HeapDestroy HeapAlloc HeapFree HeapReAlloc HeapSize VirtualAlloc VirtualFree VirtualProtect "
        "GlobalAlloc GlobalFree GlobalLock GlobalUnlock GlobalHandle TlsAlloc TlsGetValue TlsSetValue "
        "InterlockedIncrement InterlockedDecrement InitializeCriticalSection EnterCriticalSection "
        "LeaveCriticalSection DeleteCriticalSection",
        "All memory comes from galloc/grealloc/gfree/gsize (one shared guest heap; heap handles can be dummy "
        "values from handle_new(H_HEAP, NULL)). HEAP_ZERO_MEMORY (8) and HEAP_REALLOC_IN_PLACE_ONLY (0x10) "
        "flags matter; HeapReAlloc must keep the contents. VirtualAlloc: page-aligned (galloc_aligned 4096), "
        "MEM_RESERVE+MEM_COMMIT both just allocate; VirtualProtect just succeeds and writes the old protection. "
        "GlobalAlloc: GMEM_FIXED returns the pointer itself; for GMEM_MOVEABLE return the pointer too and make "
        "GlobalLock/GlobalHandle identity. TLS: per-thread slots (__thread array of 64). Interlocked*: atomic "
        "on guest memory (__atomic builtins), return the new value. CRITICAL_SECTION is 24 bytes of guest memory: "
        "keep a host recursive pthread mutex per section (store an index into a static table in the first dword)."),
    "k32_file": ("recomp/platform/k32_file.c", "KERNEL32 files",
        "CreateFileA ReadFile WriteFile SetFilePointer SetEndOfFile FlushFileBuffers CloseHandle",
        "Files: open with host_path() relative to the game directory; honour GENERIC_READ/WRITE and "
        "CREATE_ALWAYS/CREATE_NEW/OPEN_EXISTING/OPEN_ALWAYS/TRUNCATE_EXISTING; return INVALID_HANDLE_VALUE "
        "(0xffffffff) and last_error 2 (FILE_NOT_FOUND) or 3 (PATH_NOT_FOUND) on failure. Keep a FILE* or fd "
        "in handle_new(H_FILE, ...). ReadFile/WriteFile write *lpNumberOfBytes (guest pointer, may be 0). "
        "WriteFile on HSTD_OUTPUT/HSTD_ERROR goes to stdout/stderr. SetFilePointer: 32-bit with optional high "
        "part pointer, FILE_BEGIN/CURRENT/END, returns the new position. CloseHandle must accept every handle "
        "kind: close files here; for any other kind (event, mutex, thread...) just handle_free() it and "
        "return 1."),
    "k32_sync": ("recomp/platform/k32_sync.c", "KERNEL32 time, threads, events, waits",
        "Sleep SleepEx QueryPerformanceFrequency QueryPerformanceCounter GetTickCount CreateThread ExitThread "
        "ResumeThread TerminateThread SetThreadPriority GetThreadPriority CreateEventA SetEvent ResetEvent "
        "PulseEvent CreateMutexA WaitForSingleObject WaitForMultipleObjects WaitForMultipleObjectsEx DuplicateHandle",
        "Threads: CreateThread makes a host pthread with its own Cpu, guest stack and thread information block "
        "(see the notes above) and runs guest_call(cpu, lpStartAddress, 1, &lpParameter); honour "
        "CREATE_SUSPENDED (4) with ResumeThread. Write *lpThreadId if non-zero. Keep a struct in "
        "handle_new(H_THREAD, ...) with a done flag + exit code + mutex/condvar so threads can be waited on. "
        "ExitThread: record the exit code, then pthread_exit. Events (manual/auto reset, initial state) and "
        "mutexes (recursive, owner thread) with pthread mutex+condvar; WaitForSingleObject/MultipleObjects work "
        "on events, mutexes and threads, with millisecond timeouts (INFINITE = 0xffffffff), return "
        "WAIT_OBJECT_0+i / WAIT_TIMEOUT (0x102). Use one global mutex+condvar for all waitable objects if that "
        "is simpler (broadcast on every state change). QueryPerformanceFrequency: 1000000 (microseconds, "
        "CLOCK_MONOTONIC). DuplicateHandle: return the same handle value in *lpTargetHandle."),
    "u32_win": ("recomp/platform/u32_win.c", "USER32 windows and messages, plus everything declared in win.h",
        "CreateWindowExA DestroyWindow RegisterClassA UnregisterClassA FindWindowA DefWindowProcA ShowWindow "
        "UpdateWindow SetWindowPos GetWindowRect GetClientRect AdjustWindowRect AdjustWindowRectEx ClientToScreen "
        "GetWindowLongA SetWindowLongA SetWindowTextA SetMenu GetMenu SetActiveWindow SetForegroundWindow SetFocus "
        "EnableWindow IsIconic PeekMessageA GetMessageA TranslateMessage DispatchMessageA PostMessageA SendMessageA "
        "SendNotifyMessageA PostQuitMessage SetTimer KillTimer ValidateRect InvalidateRect GetDC ReleaseDC "
        "BeginPaint EndPaint LoadIconA LoadCursorA SystemParametersInfoA GetSystemMetrics SetWindowsHookExA "
        "UnhookWindowsHookEx CallNextHookEx",
        "Also implement every function and variable declared in recomp/platform/win.h (win_get, main_win, "
        "msg_post, msg_peek, plat_pump, vk_down, dik_down, mouse_*, sdl_to_vk, sdl_to_dik, win_present); "
        "#include \"win.h\". Use SDL2: the first top-level window becomes an SDL window (SDL_Init(SDL_INIT_VIDEO) "
        "lazily) of the requested client size, titled \"Future Cop: L.A.P.D.\". win_present() uploads the frame "
        "to an SDL texture (create SDL_Renderer/SDL_Texture on demand; 16-bit frames are RGB565 or RGB555 from the "
        "masks) and presents it scaled to the window. Messages: SendMessageA calls the guest window procedure "
        "directly with guest_call(cur_cpu, wndproc, 4, args) (args = hwnd, msg, wparam, lparam); DispatchMessageA "
        "the same for a queued message (MSG is 28 bytes of guest memory: hwnd, message, wParam, lParam, time, pt.x, "
        "pt.y). CreateWindowExA sends WM_CREATE (with a guest CREATESTRUCT), ShowWindow sends WM_ACTIVATEAPP "
        "(wParam 1) and WM_ACTIVATE once. plat_pump maps SDL_KEYDOWN/UP to WM_KEYDOWN/WM_KEYUP/WM_SYSKEYDOWN "
        "(lParam with repeat count 1 and the scancode in bits 16-23, bit 30/31 for up), plus WM_CHAR for "
        "printable keys, mouse motion/buttons to WM_MOUSEMOVE/WM_LBUTTONDOWN..., SDL_QUIT to WM_CLOSE, and "
        "keeps vk_down/dik_down/mouse_* up to date. GetMessageA blocks (SDL_WaitEventTimeout + queue) and "
        "returns 0 for WM_QUIT. SetTimer: post WM_TIMER at the interval (check timers in plat_pump), or call "
        "the guest TIMERPROC. Hooks: store and return a dummy handle; CallNextHookEx returns 0. GetSystemMetrics: "
        "SM_CXSCREEN/SM_CYSCREEN 640x480. GetDC returns a dummy DC handle."),
    "u32_misc": ("recomp/platform/u32_misc.c", "USER32 dialogs, input, misc",
        "MessageBoxA DialogBoxParamA EndDialog GetDlgItem SetDlgItemTextA GetDlgItemTextA SendDlgItemMessageA "
        "CheckDlgButton IsDlgButtonChecked GetAsyncKeyState GetKeyState keybd_event ShowCursor SetCursor "
        "SetCursorPos wsprintfA SetRect InvertRect",
        "#include \"win.h\" for vk_down/mouse state (implemented by another worker; do not define them). "
        "MessageBoxA: log the text and caption with plat_log and return IDOK (1) (and IDYES for MB_YESNO). "
        "DialogBoxParamA: the game uses dialogs from its resources for setup options; for now log the dialog id, "
        "and run the guest dialog procedure with WM_INITDIALOG then return IDOK (1) as if the user pressed OK "
        "(guest_call with hwnd = a dummy handle). Dialog item functions work on a small table of fake items "
        "keyed by (dialog, id) storing text and check state. GetAsyncKeyState/GetKeyState from vk_down "
        "(0x8000 if down). wsprintfA is cdecl (IMPORT(wsprintfA, -1)): implement %d %i %u %x %X %s %c %ld %lu "
        "%lx with flags/width/precision by reading varargs from the guest stack (ARG(2), ARG(3), ...); return "
        "the length."),
    "gdi_misc": ("recomp/platform/gdi_misc.c", "GDI32, ADVAPI32 (registry), WINMM, ole32, COMCTL32",
        "TextOutA SetBkColor SetBkMode SetTextColor SelectObject CreateFontA GetTextMetricsA GetDeviceCaps "
        "GetStockObject DeleteObject RegCreateKeyExA RegOpenKeyExA RegCloseKey RegFlushKey RegQueryValueExA "
        "RegSetValueExA GetUserNameA PlaySoundA timeGetTime timeGetDevCaps timeBeginPeriod timeEndPeriod "
        "timeSetEvent timeKillEvent CoInitialize CoUninitialize CoCreateInstance InitCommonControls",
        "This file provides several DLLs: define FIVE tables imp_gdi32, imp_advapi32, imp_winmm, imp_ole32 and "
        "imp_comctl32, each followed by its IMPORT_TABLE(...). (Delete the placeholder files' tables? No: the "
        "placeholder files gdi32.c advapi32.c winmm.c ole32.c comctl32.c are deleted for you; just define the "
        "tables here.) COMCTL32 is imported by ordinal: its entry name is \"#17\" (InitCommonControls, no "
        "arguments). GDI: text output can be logged/ignored (return success); GetDeviceCaps: BITSPIXEL 16, "
        "HORZRES 640, VERTRES 480, PLANES 1. Registry: persist keys and values in a simple text file "
        "<game_dir>/registry.txt (load on first use, save on RegSetValueExA/RegCloseKey); HKEY_LOCAL_MACHINE etc. "
        "are the constants 0x80000000-0x80000005; return ERROR_FILE_NOT_FOUND (2) for missing keys/values in "
        "RegOpenKeyExA/RegQueryValueExA (the game then uses defaults); RegQueryValueExA with a NULL data pointer "
        "returns the size. timeGetTime in ms since start; timeSetEvent runs the guest callback "
        "(guest_call with its own Cpu on a host thread, see threads in the notes) periodically or once. "
        "PlaySoundA: log and return 1. CoCreateInstance: return E_NOINTERFACE (0x80004002) and log the CLSID."),
    "ddraw": ("recomp/platform/ddraw.c", "DirectDraw (DDRAW.dll)", "DirectDrawCreate DirectDrawEnumerateA",
        "COM objects: create them with com_new(methods, n, host_state) where methods[] lists EVERY method of the interface in exact vtable order from the DirectX 5 SDK headers (IUnknown's QueryInterface, AddRef, Release first), each as IMPORT(name, nargs) where nargs counts `this` too (stdcall). Put a comment with the vtable index on every entry. Methods the game never needs still get an entry that logs 'warn: <method>' and returns DD_OK/S_OK (0) or E_NOTIMPL as appropriate. QueryInterface for unknown interfaces returns E_NOINTERFACE (0x80004002) and logs the IID. Out-pointers are guest addresses: write the guest object address into them with wr32. Struct layouts (DDSURFACEDESC 108 bytes, DDPIXELFORMAT 32, DDCAPS, DSBUFFERDESC, WAVEFORMATEX, DIDEVICEINSTANCEA, DIJOYSTATE...) must match the SDK exactly: write them with wr32/wr16 at the documented offsets, never with host structs. " + "Interfaces: IDirectDraw and IDirectDraw2 (QueryInterface), IDirectDrawSurface/2/3, "
        "IDirectDrawPalette, IDirectDrawClipper. #include \"win.h\". The display is always 640x480 (also accept "
        "800x600/1024x768 if asked; report all three plus 320x240 from EnumDisplayModes) at 16 bits per pixel "
        "RGB565 (masks 0xF800 0x07E0 0x001F) or 8-bit palettized if the game sets 8 bpp. Surfaces keep their pixels "
        "in guest memory (galloc, pitch = width*bytes rounded up to 8); Lock fills the DDSURFACEDESC with lpSurface "
        "= that guest address and lPitch; Unlock of the primary presents it. The primary surface with "
        "DDSCAPS_FLIP+COMPLEX and dwBackBufferCount 1 gets an attached back buffer (GetAttachedSurface); Flip "
        "swaps their memory and calls win_present() with the new front buffer. Blt/BltFast implement copies "
        "(with source/dest rects, DDBLT_KEYSRC colour key, DDBLT_COLORFILL with ddbltfx.dwFillColor) between "
        "surfaces in software; a Blt or BltFast onto the primary presents it. 8-bit surfaces present through the "
        "palette (convert to RGB565 before win_present). GetCaps: report a HEL-only device with no 3D "
        "(dwCaps without DDCAPS_3D) so the game uses its software renderer; QueryInterface for IID_IDirect3D "
        "returns E_NOINTERFACE. DirectDrawEnumerateA calls the guest callback once (lpGUID NULL, "
        "\"Primary Display Driver\", \"display\", context) with guest_call (the strings in guest memory)."),
    "dinput": ("recomp/platform/dinput.c", "DirectInput (DINPUT.dll)", "DirectInputCreateA",
        "COM objects: create them with com_new(methods, n, host_state) where methods[] lists EVERY method of the interface in exact vtable order from the DirectX 5 SDK headers (IUnknown's QueryInterface, AddRef, Release first), each as IMPORT(name, nargs) where nargs counts `this` too (stdcall). Put a comment with the vtable index on every entry. Methods the game never needs still get an entry that logs 'warn: <method>' and returns DD_OK/S_OK (0) or E_NOTIMPL as appropriate. QueryInterface for unknown interfaces returns E_NOINTERFACE (0x80004002) and logs the IID. Out-pointers are guest addresses: write the guest object address into them with wr32. Struct layouts (DDSURFACEDESC 108 bytes, DDPIXELFORMAT 32, DDCAPS, DSBUFFERDESC, WAVEFORMATEX, DIDEVICEINSTANCEA, DIJOYSTATE...) must match the SDK exactly: write them with wr32/wr16 at the documented offsets, never with host structs. " + "Interfaces: IDirectInputA, IDirectInputDeviceA and IDirectInputDevice2A (QueryInterface; Poll). "
        "#include \"win.h\" (dik_down, mouse_* are kept up to date by another worker's plat_pump(); call "
        "plat_pump() at the start of GetDeviceState). Keyboard (GUID_SysKeyboard): GetDeviceState copies 256 "
        "bytes, 0x80 for keys in dik_down. Mouse (GUID_SysMouse): DIMOUSESTATE relative motion since the last call. "
        "Joysticks: use SDL2's game controller / joystick API (SDL_InitSubSystem(SDL_INIT_JOYSTICK|"
        "SDL_INIT_GAMECONTROLLER)); EnumDevices with DIDEVTYPE_JOYSTICK reports each connected pad via the guest "
        "callback (DIDEVICEINSTANCEA in guest memory, with a GUID derived from the index); GetDeviceState fills "
        "DIJOYSTATE (lX lY lZ lRx lRy lRz rglSlider[2] rgdwPOV[4] rgbButtons[32]) with axes scaled to the range "
        "set by SetProperty(DIPROP_RANGE) (default 0..65535). GetDeviceData with buffered keyboard events: keep a "
        "small queue of key changes since the last call (DIDEVICEOBJECTDATA 16 bytes each). SetDataFormat, "
        "SetCooperativeLevel, Acquire, Unacquire succeed."),
    "dsound": ("recomp/platform/dsound.c", "DirectSound (DSOUND.dll)", "DirectSoundCreate DirectSoundEnumerateA",
        "COM objects: create them with com_new(methods, n, host_state) where methods[] lists EVERY method of the interface in exact vtable order from the DirectX 5 SDK headers (IUnknown's QueryInterface, AddRef, Release first), each as IMPORT(name, nargs) where nargs counts `this` too (stdcall). Put a comment with the vtable index on every entry. Methods the game never needs still get an entry that logs 'warn: <method>' and returns DD_OK/S_OK (0) or E_NOTIMPL as appropriate. QueryInterface for unknown interfaces returns E_NOINTERFACE (0x80004002) and logs the IID. Out-pointers are guest addresses: write the guest object address into them with wr32. Struct layouts (DDSURFACEDESC 108 bytes, DDPIXELFORMAT 32, DDCAPS, DSBUFFERDESC, WAVEFORMATEX, DIDEVICEINSTANCEA, DIJOYSTATE...) must match the SDK exactly: write them with wr32/wr16 at the documented offsets, never with host structs. " + "Interfaces: IDirectSound and IDirectSoundBuffer. Mix in software with SDL2 audio "
        "(SDL_OpenAudioDevice, 22050 Hz stereo S16, small buffer of 512 samples) in the SDL audio callback: every "
        "playing secondary buffer is resampled from its own format (8/16-bit, mono/stereo, any rate; "
        "SetFrequency changes the rate), scaled by volume (hundredths of dB, 0 = full, -10000 = silent) and pan, "
        "looping if played with DSBPLAY_LOOPING, and stops at the end otherwise. Buffer memory lives in guest "
        "memory (galloc); Lock returns guest pointers to one or two regions (wrap-around) and their sizes; "
        "GetCurrentPosition returns play and write cursors in bytes that advance with real playback (the game "
        "streams music by writing behind the play cursor). Protect buffer state with SDL_LockAudioDevice. "
        "The primary buffer (DSBCAPS_PRIMARYBUFFER) only accepts SetFormat/Play/GetCaps. DuplicateSoundBuffer "
        "shares the memory. DirectSoundEnumerateA calls the guest callback once (NULL GUID, "
        "\"Primary Sound Driver\", \"\", context)."),
}

ap = argparse.ArgumentParser()
ap.add_argument("group", nargs="?", choices=sorted(GROUPS))
ap.add_argument("--task", help="send this brief (a Markdown file) instead of a platform group")
ap.add_argument("--model", default="gemini-3.8-flash-high")
a = ap.parse_args()
if a.task:
    prompt = open(os.path.join(ROOT, a.task)).read()
else:
    path, title, funcs, notes = GROUPS[a.group]
    assign = (f"**File:** `{path}` (the table must be named `imp_{a.group}` and end with "
              f"`IMPORT_TABLE({a.group});`)\n\n**{title}:** {funcs.replace(' ', ', ')}\n\n**Notes:** {notes}")
    prompt = open(os.path.join(ROOT, "docs", "PLATFORM_TASK.md")).read().replace("{ASSIGNMENT}", assign)
r = subprocess.run(["agy", "--model", a.model, "--output-format", "json", "-p", prompt], cwd=ROOT,
                   capture_output=True, text=True, timeout=7200)
name = os.path.splitext(os.path.basename(a.task))[0] if a.task else a.group
with open(os.path.join(ROOT, "build", f"agy_{name}.out"), "w") as f:  # the whole reply
    f.write(r.stdout + "\n" + r.stderr)
print(r.stdout[-3000:], r.stderr[-1000:])
