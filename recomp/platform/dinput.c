/* DirectInput (DINPUT.dll) implementation for Future Cop (FCopLAPD.exe)
 * Interfaces: IDirectInputA, IDirectInputDeviceA, IDirectInputDevice2A
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "host.h"
#include "win.h"

/* HRESULT return codes */
#define DI_OK                       0x00000000u
#define S_OK                        0x00000000u
#define S_FALSE                     0x00000001u
#define DI_BUFFEROVERFLOW           0x00000001u
#define E_NOTIMPL                   0x80004001u
#define E_NOINTERFACE               0x80004002u
#define E_POINTER                   0x80004003u
#define E_FAIL                      0x80004005u
#define E_OUTOFMEMORY               0x8007000Eu
#define E_INVALIDARG                0x80070057u
#define CLASS_E_NOAGGREGATION       0x80040110u

#define DIERR_NOTFOUND              0x800401E4u
#define DIERR_DEVICENOTREG          0x80040154u
#define DIERR_NOTINITIALIZED        0x80070015u
#define DIERR_INPUTLOST             0x8007001Eu
#define DIERR_NOTACQUIRED           0x8007001Eu

/* DirectInput Device Types */
#define DIDEVTYPE_DEVICE            1
#define DIDEVTYPE_MOUSE             2
#define DIDEVTYPE_KEYBOARD          3
#define DIDEVTYPE_JOYSTICK          4

/* Subtypes */
#define DIDEVTYPEKEYBOARD_PCENH     4
#define DIDEVTYPEMOUSE_TRADITIONAL  1
#define DIDEVTYPEJOYSTICK_GAMEPAD   3

/* DIDC flags */
#define DIDC_ATTACHED               0x00000001u
#define DIDC_POLLEDDATAGEN          0x00000002u
#define DIDC_EMULATED               0x00000004u
#define DIDC_POLLEDDEVICE           0x00000020u
#define DIDC_FORCEFEEDBACK          0x00000100u

/* Property IDs */
#define DIPROP_BUFFERSIZE_ID        1
#define DIPROP_AXISMODE_ID          2
#define DIPROP_GRANULARITY_ID       3
#define DIPROP_RANGE_ID             4
#define DIPROP_DEADZONE_ID          5
#define DIPROP_SATURATION_ID        6

/* Header / How flags */
#define DIPH_DEVICE                 0
#define DIPH_BYOFFSET               1
#define DIPH_BYID                   2

/* GetDeviceData flags */
#define DIGDD_PEEK                  0x00000001u

/* GUID definitions */
static const u8 guid_d4_unknown[8] = { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 };
static const u8 guid_d4_dinput[8]  = { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };

/* Device category enum */
enum DevKind {
    DEV_KEYBOARD = 1,
    DEV_MOUSE,
    DEV_JOYSTICK
};

#define KEY_QUEUE_CAP 256

struct KeyEvent {
    u32 dwOfs;
    u32 dwData;
    u32 dwTimeStamp;
    u32 dwSequence;
};

/* Host states */
struct DiHost {
    u32 ref_count;
};

struct DiDevHost {
    u32 ref_count;
    enum DevKind kind;
    int joy_index;
    int acquired;
    u32 hwnd;
    u32 coop_flags;
    u32 event_handle;

    /* Guest COM object addresses */
    u32 dev1_obj;
    u32 dev2_obj;

    /* Mouse relative movement tracking */
    int last_mouse_x;
    int last_mouse_y;

    /* Keyboard buffered events */
    u8 prev_dik_down[256];
    struct KeyEvent queue[KEY_QUEUE_CAP];
    int q_head;
    int q_count;
    u32 seq;
    u32 buffer_capacity;

    /* Joystick axis ranges (8 axes: X, Y, Z, Rx, Ry, Rz, Slider0, Slider1) */
    s32 axis_min[8];
    s32 axis_max[8];
    u32 deadzone[8];

    /* SDL Joystick / Controller handles */
    SDL_GameController *gamecontroller;
    SDL_Joystick *joystick;
};

/* Forward declarations of COM vtables */
static const struct ImpDef di_methods[];
static const struct ImpDef dev1_methods[];
static const struct ImpDef dev2_methods[];

/* GUID comparison helpers */
static int match_guid(u32 riid, u32 d1, u16 d2, u16 d3, const u8 d4[8])
{
    if (!riid) return 0;
    if (rd32(riid + 0) != d1) return 0;
    if (rd16(riid + 4) != d2) return 0;
    if (rd16(riid + 6) != d3) return 0;
    for (int i = 0; i < 8; i++) {
        if (rd8(riid + 8 + i) != d4[i]) return 0;
    }
    return 1;
}

static void log_unknown_iid(const char *prefix, u32 riid)
{
    if (!riid) {
        plat_log("warn: %s: NULL riid", prefix);
        return;
    }
    u32 d1 = rd32(riid + 0);
    u16 d2 = rd16(riid + 4);
    u16 d3 = rd16(riid + 6);
    u8 d4[8];
    for (int i = 0; i < 8; i++) d4[i] = rd8(riid + 8 + i);
    plat_log("warn: %s: unknown IID {%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
             prefix, d1, d2, d3, d4[0], d4[1], d4[2], d4[3], d4[4], d4[5], d4[6], d4[7]);
}

/* Ensure SDL joystick subsystem is initialized */
static void ensure_sdl_joystick(void)
{
    plat_pump();
    if (!SDL_WasInit(SDL_INIT_JOYSTICK)) {
        if (SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) < 0) {
            plat_log("warn: SDL_InitSubSystem(JOYSTICK|GAMECONTROLLER) failed: %s", SDL_GetError());
        } else {
            SDL_JoystickEventState(SDL_ENABLE);
            SDL_GameControllerEventState(SDL_ENABLE);
        }
    }
}

/* Helper to fill a DIDEVICEINSTANCEA at guest address p */
static void fill_device_instance(u32 p, u32 d1, u16 d2, u16 d3, const u8 d4[8],
                                 u32 prod_d1, u16 prod_d2, u16 prod_d3, const u8 prod_d4[8],
                                 u32 dev_type, const char *inst_name, const char *prod_name)
{
    memset(G(p), 0, 580);
    wr32(p + 0x00, 580); /* dwSize */

    /* guidInstance */
    wr32(p + 0x04, d1);
    wr16(p + 0x08, d2);
    wr16(p + 0x0A, d3);
    for (int i = 0; i < 8; i++) wr8(p + 0x0C + i, d4[i]);

    /* guidProduct */
    wr32(p + 0x14, prod_d1);
    wr16(p + 0x18, prod_d2);
    wr16(p + 0x1A, prod_d3);
    for (int i = 0; i < 8; i++) wr8(p + 0x1C + i, prod_d4[i]);

    /* dwDevType */
    wr32(p + 0x24, dev_type);

    /* tszInstanceName (260 bytes) */
    gstrcpy(p + 0x28, inst_name, 260);

    /* tszProductName (260 bytes) */
    gstrcpy(p + 0x12C, prod_name, 260);
}

/* Scale raw SDL axis (-32768..32767) to [min_val, max_val] with deadzone */
static s32 scale_axis(s16 raw, s32 min_val, s32 max_val, u32 deadzone)
{
    s32 val = raw;
    if (deadzone > 0 && deadzone < 10000) {
        s32 threshold = (s32)(((double)deadzone / 10000.0) * 32768.0);
        if (raw > threshold) {
            val = (s32)(((double)(raw - threshold) / (32767.0 - threshold)) * 32767.0);
        } else if (raw < -threshold) {
            val = (s32)(((double)(raw + threshold) / (32768.0 - threshold)) * -32768.0);
        } else {
            val = 0;
        }
    }
    s32 center = (min_val + max_val) / 2;
    if (val == 0) return center;
    if (val > 0) {
        return (s32)(center + ((double)val / 32767.0) * (max_val - center) + 0.5);
    } else {
        return (s32)(center + ((double)val / 32768.0) * (center - min_val) - 0.5);
    }
}

/* Axis range configuration */
static void dev_set_range(struct DiDevHost *dev, u32 dwHow, u32 dwObj, s32 min_val, s32 max_val)
{
    if (dwHow == DIPH_DEVICE) {
        for (int i = 0; i < 8; i++) {
            dev->axis_min[i] = min_val;
            dev->axis_max[i] = max_val;
        }
    } else if (dwHow == DIPH_BYOFFSET) {
        int idx = (int)(dwObj / 4);
        if (idx >= 0 && idx < 8) {
            dev->axis_min[idx] = min_val;
            dev->axis_max[idx] = max_val;
        }
    } else if (dwHow == DIPH_BYID) {
        int idx = (int)((dwObj >> 8) & 0xFFu);
        if (idx >= 8) idx = (int)(dwObj & 0xFFu);
        if (idx >= 0 && idx < 8) {
            dev->axis_min[idx] = min_val;
            dev->axis_max[idx] = max_val;
        }
    }
}

static void dev_get_range(struct DiDevHost *dev, u32 dwHow, u32 dwObj, s32 *out_min, s32 *out_max)
{
    int idx = 0;
    if (dwHow == DIPH_BYOFFSET) {
        idx = (int)(dwObj / 4);
    } else if (dwHow == DIPH_BYID) {
        idx = (int)((dwObj >> 8) & 0xFFu);
        if (idx >= 8) idx = (int)(dwObj & 0xFFu);
    }
    if (idx >= 0 && idx < 8) {
        *out_min = dev->axis_min[idx];
        *out_max = dev->axis_max[idx];
    } else {
        *out_min = dev->axis_min[0];
        *out_max = dev->axis_max[0];
    }
}

static void dev_set_deadzone(struct DiDevHost *dev, u32 dwHow, u32 dwObj, u32 deadzone)
{
    if (dwHow == DIPH_DEVICE) {
        for (int i = 0; i < 8; i++) {
            dev->deadzone[i] = deadzone;
        }
    } else if (dwHow == DIPH_BYOFFSET) {
        int idx = (int)(dwObj / 4);
        if (idx >= 0 && idx < 8) {
            dev->deadzone[idx] = deadzone;
        }
    } else if (dwHow == DIPH_BYID) {
        int idx = (int)((dwObj >> 8) & 0xFFu);
        if (idx >= 8) idx = (int)(dwObj & 0xFFu);
        if (idx >= 0 && idx < 8) {
            dev->deadzone[idx] = deadzone;
        }
    }
}

static u32 dev_get_deadzone(struct DiDevHost *dev, u32 dwHow, u32 dwObj)
{
    int idx = 0;
    if (dwHow == DIPH_BYOFFSET) {
        idx = (int)(dwObj / 4);
    } else if (dwHow == DIPH_BYID) {
        idx = (int)((dwObj >> 8) & 0xFFu);
        if (idx >= 8) idx = (int)(dwObj & 0xFFu);
    }
    if (idx >= 0 && idx < 8) {
        return dev->deadzone[idx];
    }
    return dev->deadzone[0];
}

/* Update buffered keyboard events */
static void pump_key_events(struct DiDevHost *dev)
{
    plat_pump();
    for (int i = 0; i < 256; i++) {
        u8 cur = dik_down[i] ? 0x80 : 0x00;
        if (cur != dev->prev_dik_down[i]) {
            dev->prev_dik_down[i] = cur;
            if (dev->buffer_capacity > 0 && dev->q_count < (int)dev->buffer_capacity && dev->q_count < KEY_QUEUE_CAP) {
                int tail = (dev->q_head + dev->q_count) % KEY_QUEUE_CAP;
                dev->queue[tail].dwOfs = (u32)i;
                dev->queue[tail].dwData = (u32)cur;
                dev->queue[tail].dwTimeStamp = SDL_GetTicks();
                dev->queue[tail].dwSequence = ++dev->seq;
                dev->q_count++;
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * IDirectInputA Methods
 * ------------------------------------------------------------------------- */

/* vtable index 0: QueryInterface(this, riid, ppvObj) */
static u32 imp_di_QueryInterface(Cpu *c)
{
    struct DiHost *di = (struct DiHost *)com_host(ARG(0));
    u32 riid = ARG(1);
    u32 ppv = ARG(2);

    if (!ppv) return E_INVALIDARG;

    if (match_guid(riid, 0x00000000u, 0x0000, 0x0000, guid_d4_unknown) ||
        match_guid(riid, 0x89521360u, 0xAA8A, 0x11CF, guid_d4_dinput)) {
        if (di) di->ref_count++;
        wr32(ppv, ARG(0));
        return DI_OK;
    }

    log_unknown_iid("IDirectInput::QueryInterface", riid);
    wr32(ppv, 0);
    return E_NOINTERFACE;
}

/* vtable index 1: AddRef(this) */
static u32 imp_di_AddRef(Cpu *c)
{
    struct DiHost *di = (struct DiHost *)com_host(ARG(0));
    return di ? ++di->ref_count : 1;
}

/* vtable index 2: Release(this) */
static u32 imp_di_Release(Cpu *c)
{
    struct DiHost *di = (struct DiHost *)com_host(ARG(0));
    if (!di) return 0;
    if (di->ref_count > 0) di->ref_count--;
    if (di->ref_count == 0) {
        free(di);
        return 0;
    }
    return di->ref_count;
}

/* vtable index 3: CreateDevice(this, rguid, lplpDirectInputDevice, pUnkOuter) */
static u32 imp_di_CreateDevice(Cpu *c)
{
    u32 rguid = ARG(1);
    u32 lplpDevice = ARG(2);
    u32 pUnkOuter = ARG(3);

    if (pUnkOuter != 0) return CLASS_E_NOAGGREGATION;
    if (!lplpDevice || !rguid) return E_INVALIDARG;

    enum DevKind kind = DEV_JOYSTICK;
    int joy_idx = 0;

    if (match_guid(rguid, 0x6F1D2B61u, 0xD5A0, 0x11CF, guid_d4_dinput)) {
        kind = DEV_KEYBOARD;
    } else if (match_guid(rguid, 0x6F1D2B60u, 0xD5A0, 0x11CF, guid_d4_dinput)) {
        kind = DEV_MOUSE;
    } else {
        u32 d1 = rd32(rguid);
        u16 d2 = rd16(rguid + 4);
        u16 d3 = rd16(rguid + 6);
        if (d2 == 0xD5A0 && d3 == 0x11CF && match_guid(rguid, d1, d2, d3, guid_d4_dinput)) {
            kind = DEV_JOYSTICK;
            if (d1 >= 0x6F1D2B70u) {
                joy_idx = (int)(d1 - 0x6F1D2B70u);
            }
        } else {
            plat_log("warn: IDirectInput::CreateDevice: unrecognized device GUID");
            kind = DEV_JOYSTICK;
            joy_idx = 0;
        }
    }

    struct DiDevHost *dev = (struct DiDevHost *)calloc(1, sizeof(*dev));
    if (!dev) return E_OUTOFMEMORY;

    dev->ref_count = 1;
    dev->kind = kind;
    dev->joy_index = joy_idx;
    dev->buffer_capacity = 64;
    for (int a = 0; a < 8; a++) {
        dev->axis_min[a] = 0;
        dev->axis_max[a] = 65535;
        dev->deadzone[a] = 0;
    }

    if (kind == DEV_MOUSE) {
        dev->last_mouse_x = mouse_x;
        dev->last_mouse_y = mouse_y;
    } else if (kind == DEV_JOYSTICK) {
        ensure_sdl_joystick();
        if (joy_idx >= 0 && joy_idx < SDL_NumJoysticks()) {
            if (SDL_IsGameController(joy_idx)) {
                dev->gamecontroller = SDL_GameControllerOpen(joy_idx);
                if (dev->gamecontroller) {
                    dev->joystick = SDL_GameControllerGetJoystick(dev->gamecontroller);
                }
            }
            if (!dev->joystick) {
                dev->joystick = SDL_JoystickOpen(joy_idx);
            }
        }
    }

    u32 obj = com_new(dev1_methods, 18, dev);
    dev->dev1_obj = obj;
    wr32(lplpDevice, obj);
    return DI_OK;
}

/* vtable index 4: EnumDevices(this, dwDevType, lpCallback, pvRef, dwFlags) */
static u32 imp_di_EnumDevices(Cpu *c)
{
    u32 dwDevType = ARG(1);
    u32 lpCallback = ARG(2);
    u32 pvRef = ARG(3);
    u32 dwFlags = ARG(4);
    (void)dwFlags;

    if (!lpCallback) return E_INVALIDARG;

    ensure_sdl_joystick();

    static u32 enum_ddi = 0;
    if (!enum_ddi) enum_ddi = galloc(592);

    u32 type_filter = dwDevType & 0xFFu;
    int enum_keyboard = (type_filter == 0 || type_filter == DIDEVTYPE_DEVICE || type_filter == DIDEVTYPE_KEYBOARD);
    int enum_mouse = (type_filter == 0 || type_filter == DIDEVTYPE_DEVICE || type_filter == DIDEVTYPE_MOUSE);
    int enum_joystick = (type_filter == 0 || type_filter == DIDEVTYPE_DEVICE || type_filter == DIDEVTYPE_JOYSTICK);

    u32 args[2];
    args[0] = enum_ddi;
    args[1] = pvRef;

    if (enum_keyboard) {
        fill_device_instance(enum_ddi,
                             0x6F1D2B61u, 0xD5A0, 0x11CF, guid_d4_dinput,
                             0x6F1D2B61u, 0xD5A0, 0x11CF, guid_d4_dinput,
                             DIDEVTYPE_KEYBOARD | (DIDEVTYPEKEYBOARD_PCENH << 8),
                             "Keyboard", "Standard 101/102-Key or Microsoft Natural Keyboard");
        u32 ret = guest_call(c, lpCallback, 2, args);
        if (ret == 0) return DI_OK;
    }

    if (enum_mouse) {
        fill_device_instance(enum_ddi,
                             0x6F1D2B60u, 0xD5A0, 0x11CF, guid_d4_dinput,
                             0x6F1D2B60u, 0xD5A0, 0x11CF, guid_d4_dinput,
                             DIDEVTYPE_MOUSE | (DIDEVTYPEMOUSE_TRADITIONAL << 8),
                             "Mouse", "Standard Mouse");
        u32 ret = guest_call(c, lpCallback, 2, args);
        if (ret == 0) return DI_OK;
    }

    if (enum_joystick) {
        SDL_JoystickUpdate();
        int num_joysticks = SDL_NumJoysticks();
        for (int i = 0; i < num_joysticks; i++) {
            const char *name = NULL;
            if (SDL_IsGameController(i)) {
                name = SDL_GameControllerNameForIndex(i);
            }
            if (!name || !*name) {
                name = SDL_JoystickNameForIndex(i);
            }
            char default_name[64];
            if (!name || !*name) {
                snprintf(default_name, sizeof(default_name), "Gamepad %d", i + 1);
                name = default_name;
            }
            plat_log("gamepad found: %s", name);
            fill_device_instance(enum_ddi,
                                 0x6F1D2B70u + (u32)i, 0xD5A0, 0x11CF, guid_d4_dinput,
                                 0x6F1D2B70u, 0xD5A0, 0x11CF, guid_d4_dinput,
                                 DIDEVTYPE_JOYSTICK | (DIDEVTYPEJOYSTICK_GAMEPAD << 8),
                                 name, name);
            u32 ret = guest_call(c, lpCallback, 2, args);
            if (ret == 0) break;
        }
    }

    return DI_OK;
}

/* vtable index 5: GetDeviceStatus(this, rguidInstance) */
static u32 imp_di_GetDeviceStatus(Cpu *c)
{
    (void)c;
    return DI_OK;
}

/* vtable index 6: RunControlPanel(this, hwndOwner, dwFlags) */
static u32 imp_di_RunControlPanel(Cpu *c)
{
    (void)c;
    plat_log("warn: IDirectInput::RunControlPanel");
    return DI_OK;
}

/* vtable index 7: Initialize(this, hinst, dwVersion) */
static u32 imp_di_Initialize(Cpu *c)
{
    (void)c;
    plat_log("warn: IDirectInput::Initialize");
    return DI_OK;
}

/* -------------------------------------------------------------------------
 * IDirectInputDeviceA & IDirectInputDevice2A Methods
 * ------------------------------------------------------------------------- */

/* vtable index 0: QueryInterface(this, riid, ppvObj) */
static u32 imp_dev_QueryInterface(Cpu *c)
{
    struct DiDevHost *dev = (struct DiDevHost *)com_host(ARG(0));
    u32 riid = ARG(1);
    u32 ppv = ARG(2);

    if (!ppv) return E_INVALIDARG;
    if (!dev) return E_FAIL;

    if (match_guid(riid, 0x00000000u, 0x0000, 0x0000, guid_d4_unknown) ||
        match_guid(riid, 0x5944E680u, 0xC92E, 0x11CF, guid_d4_dinput)) {
        dev->ref_count++;
        if (!dev->dev1_obj) {
            dev->dev1_obj = com_new(dev1_methods, 18, dev);
        }
        wr32(ppv, dev->dev1_obj);
        return DI_OK;
    }

    if (match_guid(riid, 0x5944E682u, 0xC92E, 0x11CF, guid_d4_dinput)) {
        dev->ref_count++;
        if (!dev->dev2_obj) {
            dev->dev2_obj = com_new(dev2_methods, 27, dev);
        }
        wr32(ppv, dev->dev2_obj);
        return DI_OK;
    }

    log_unknown_iid("IDirectInputDevice::QueryInterface", riid);
    wr32(ppv, 0);
    return E_NOINTERFACE;
}

/* vtable index 1: AddRef(this) */
static u32 imp_dev_AddRef(Cpu *c)
{
    struct DiDevHost *dev = (struct DiDevHost *)com_host(ARG(0));
    return dev ? ++dev->ref_count : 1;
}

/* vtable index 2: Release(this) */
static u32 imp_dev_Release(Cpu *c)
{
    struct DiDevHost *dev = (struct DiDevHost *)com_host(ARG(0));
    if (!dev) return 0;
    if (dev->ref_count > 0) dev->ref_count--;
    if (dev->ref_count == 0) {
        if (dev->gamecontroller) {
            SDL_GameControllerClose(dev->gamecontroller);
            dev->gamecontroller = NULL;
        }
        if (dev->joystick) {
            SDL_JoystickClose(dev->joystick);
            dev->joystick = NULL;
        }
        free(dev);
        return 0;
    }
    return dev->ref_count;
}

/* vtable index 3: GetCapabilities(this, lpDIDevCaps) */
static u32 imp_dev_GetCapabilities(Cpu *c)
{
    struct DiDevHost *dev = (struct DiDevHost *)com_host(ARG(0));
    u32 lpDIDevCaps = ARG(1);

    if (!dev || !lpDIDevCaps) return E_INVALIDARG;

    u32 sz = rd32(lpDIDevCaps);
    if (sz < 24) return E_INVALIDARG;

    if (sz > 512) sz = 512;
    memset(G(lpDIDevCaps + 4), 0, sz - 4);

    if (dev->kind == DEV_KEYBOARD) {
        wr32(lpDIDevCaps + 4, DIDC_ATTACHED | DIDC_EMULATED);
        wr32(lpDIDevCaps + 8, DIDEVTYPE_KEYBOARD | (DIDEVTYPEKEYBOARD_PCENH << 8));
        wr32(lpDIDevCaps + 12, 0);   /* dwAxes */
        wr32(lpDIDevCaps + 16, 256); /* dwButtons */
        wr32(lpDIDevCaps + 20, 0);   /* dwPOVs */
    } else if (dev->kind == DEV_MOUSE) {
        wr32(lpDIDevCaps + 4, DIDC_ATTACHED | DIDC_EMULATED);
        wr32(lpDIDevCaps + 8, DIDEVTYPE_MOUSE | (DIDEVTYPEMOUSE_TRADITIONAL << 8));
        wr32(lpDIDevCaps + 12, 3);   /* dwAxes */
        wr32(lpDIDevCaps + 16, 3);   /* dwButtons */
        wr32(lpDIDevCaps + 20, 0);   /* dwPOVs */
    } else if (dev->kind == DEV_JOYSTICK) {
        wr32(lpDIDevCaps + 4, DIDC_ATTACHED | DIDC_POLLEDDATAGEN | DIDC_POLLEDDEVICE);
        wr32(lpDIDevCaps + 8, DIDEVTYPE_JOYSTICK | (DIDEVTYPEJOYSTICK_GAMEPAD << 8));
        wr32(lpDIDevCaps + 12, 6);   /* dwAxes: X, Y, Z, Rx, Ry, Rz */
        wr32(lpDIDevCaps + 16, 32);  /* dwButtons */
        wr32(lpDIDevCaps + 20, 1);   /* dwPOVs */
    }

    return DI_OK;
}

/* vtable index 4: EnumObjects(this, lpCallback, pvRef, dwFlags) */
static u32 imp_dev_EnumObjects(Cpu *c)
{
    (void)c;
    plat_log("warn: IDirectInputDevice::EnumObjects");
    return DI_OK;
}

/* vtable index 5: GetProperty(this, rguidProp, pdiph) */
static u32 imp_dev_GetProperty(Cpu *c)
{
    struct DiDevHost *dev = (struct DiDevHost *)com_host(ARG(0));
    u32 rguidProp = ARG(1);
    u32 pdiph = ARG(2);

    if (!dev || !pdiph) return E_INVALIDARG;

    u32 prop_id = (rguidProp <= 0xFFFFu) ? rguidProp : rd32(rguidProp);
    if (prop_id == DIPROP_BUFFERSIZE_ID) {
        wr32(pdiph + 16, dev->buffer_capacity);
        return DI_OK;
    }
    if (prop_id == DIPROP_RANGE_ID) {
        u32 dwObj = rd32(pdiph + 8);
        u32 dwHow = rd32(pdiph + 12);
        s32 min_val = 0, max_val = 65535;
        dev_get_range(dev, dwHow, dwObj, &min_val, &max_val);
        wr32(pdiph + 16, (u32)min_val);
        wr32(pdiph + 20, (u32)max_val);
        return DI_OK;
    }
    if (prop_id == DIPROP_DEADZONE_ID) {
        u32 dwObj = rd32(pdiph + 8);
        u32 dwHow = rd32(pdiph + 12);
        u32 dz = dev_get_deadzone(dev, dwHow, dwObj);
        wr32(pdiph + 16, dz);
        return DI_OK;
    }

    return DI_OK;
}

/* vtable index 6: SetProperty(this, rguidProp, pdiph) */
static u32 imp_dev_SetProperty(Cpu *c)
{
    struct DiDevHost *dev = (struct DiDevHost *)com_host(ARG(0));
    u32 rguidProp = ARG(1);
    u32 pdiph = ARG(2);

    if (!dev || !pdiph) return E_INVALIDARG;

    u32 prop_id = (rguidProp <= 0xFFFFu) ? rguidProp : rd32(rguidProp);
    if (prop_id == DIPROP_BUFFERSIZE_ID) {
        u32 cap = rd32(pdiph + 16);
        if (cap > KEY_QUEUE_CAP) cap = KEY_QUEUE_CAP;
        dev->buffer_capacity = cap;
        return DI_OK;
    }
    if (prop_id == DIPROP_RANGE_ID) {
        u32 dwObj = rd32(pdiph + 8);
        u32 dwHow = rd32(pdiph + 12);
        s32 min_val = (s32)rd32(pdiph + 16);
        s32 max_val = (s32)rd32(pdiph + 20);
        dev_set_range(dev, dwHow, dwObj, min_val, max_val);
        return DI_OK;
    }
    if (prop_id == DIPROP_DEADZONE_ID) {
        u32 dwObj = rd32(pdiph + 8);
        u32 dwHow = rd32(pdiph + 12);
        u32 dz = rd32(pdiph + 16);
        dev_set_deadzone(dev, dwHow, dwObj, dz);
        return DI_OK;
    }

    return DI_OK;
}

/* vtable index 7: Acquire(this) */
static u32 imp_dev_Acquire(Cpu *c)
{
    struct DiDevHost *dev = (struct DiDevHost *)com_host(ARG(0));
    if (dev) {
        dev->acquired = 1;
        if (dev->kind == DEV_JOYSTICK) {
            if (!dev->gamecontroller && !dev->joystick) {
                if (dev->joy_index >= 0 && dev->joy_index < SDL_NumJoysticks()) {
                    if (SDL_IsGameController(dev->joy_index)) {
                        dev->gamecontroller = SDL_GameControllerOpen(dev->joy_index);
                        if (dev->gamecontroller) dev->joystick = SDL_GameControllerGetJoystick(dev->gamecontroller);
                    }
                    if (!dev->joystick) dev->joystick = SDL_JoystickOpen(dev->joy_index);
                }
            }
            const char *name = dev->gamecontroller ? SDL_GameControllerName(dev->gamecontroller) :
                               dev->joystick ? SDL_JoystickName(dev->joystick) : "Gamepad";
            plat_log("gamepad acquired: %s", name);
        }
    }
    return DI_OK;
}

/* vtable index 8: Unacquire(this) */
static u32 imp_dev_Unacquire(Cpu *c)
{
    struct DiDevHost *dev = (struct DiDevHost *)com_host(ARG(0));
    if (dev) dev->acquired = 0;
    return DI_OK;
}

/* vtable index 9: GetDeviceState(this, cbData, lpvData) */
static u32 imp_dev_GetDeviceState(Cpu *c)
{
    struct DiDevHost *dev = (struct DiDevHost *)com_host(ARG(0));
    u32 cbData = ARG(1);
    u32 lpvData = ARG(2);

    plat_pump();
    if (!dev || !lpvData || cbData == 0) return E_INVALIDARG;

    if (dev->kind == DEV_KEYBOARD) {
        pump_key_events(dev);
        u32 count = cbData < 256 ? cbData : 256;
        for (u32 i = 0; i < count; i++) {
            wr8(lpvData + i, dik_down[i] ? 0x80 : 0);
        }
        return DI_OK;
    }

    if (dev->kind == DEV_MOUSE) {
        int rel_x = mouse_x - dev->last_mouse_x;
        int rel_y = mouse_y - dev->last_mouse_y;
        dev->last_mouse_x = mouse_x;
        dev->last_mouse_y = mouse_y;

        if (cbData >= 4)  wr32(lpvData + 0, (u32)(s32)rel_x);
        if (cbData >= 8)  wr32(lpvData + 4, (u32)(s32)rel_y);
        if (cbData >= 12) wr32(lpvData + 8, 0); /* wheel */
        if (cbData >= 16) {
            wr8(lpvData + 12, (mouse_buttons & 1) ? 0x80 : 0);
            wr8(lpvData + 13, (mouse_buttons & 2) ? 0x80 : 0);
            wr8(lpvData + 14, (mouse_buttons & 4) ? 0x80 : 0);
            wr8(lpvData + 15, 0);
        }
        if (cbData >= 20) {
            for (u32 i = 16; i < cbData && i < 20; i++) wr8(lpvData + i, 0);
        }
        return DI_OK;
    }

    if (dev->kind == DEV_JOYSTICK) {
        u32 sz = cbData < 80 ? cbData : 80;
        memset(G(lpvData), 0, sz);
        SDL_JoystickUpdate();
        SDL_GameControllerUpdate();

        s16 raw_lx = 0, raw_ly = 0, raw_lz = 0, raw_rx = 0, raw_ry = 0, raw_rz = 0;
        s16 raw_slider0 = 0, raw_slider1 = 0;
        u32 pov = 0xFFFFFFFFu;
        u8 btn[32] = {0};

        if (dev->gamecontroller) {
            raw_lx = SDL_GameControllerGetAxis(dev->gamecontroller, SDL_CONTROLLER_AXIS_LEFTX);
            raw_ly = SDL_GameControllerGetAxis(dev->gamecontroller, SDL_CONTROLLER_AXIS_LEFTY);
            raw_rx = SDL_GameControllerGetAxis(dev->gamecontroller, SDL_CONTROLLER_AXIS_RIGHTX);
            raw_ry = SDL_GameControllerGetAxis(dev->gamecontroller, SDL_CONTROLLER_AXIS_RIGHTY);
            s16 trig_l = SDL_GameControllerGetAxis(dev->gamecontroller, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
            s16 trig_r = SDL_GameControllerGetAxis(dev->gamecontroller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);

            int up = SDL_GameControllerGetButton(dev->gamecontroller, SDL_CONTROLLER_BUTTON_DPAD_UP);
            int down = SDL_GameControllerGetButton(dev->gamecontroller, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
            int left = SDL_GameControllerGetButton(dev->gamecontroller, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
            int right = SDL_GameControllerGetButton(dev->gamecontroller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);

            if (up && right) pov = 4500;
            else if (down && right) pov = 13500;
            else if (down && left) pov = 22500;
            else if (up && left) pov = 31500;
            else if (up) pov = 0;
            else if (right) pov = 9000;
            else if (down) pov = 18000;
            else if (left) pov = 27000;

            /* D-pad also acts as X/Y when stick is centred */
            if (abs(raw_lx) <= 4000 && abs(raw_ly) <= 4000) {
                if (left && !right) raw_lx = -32768;
                else if (right && !left) raw_lx = 32767;
                if (up && !down) raw_ly = -32768;
                else if (down && !up) raw_ly = 32767;
            }

            /* Buttons 0..7: A, B, X, Y, LB, RB, Back, Start */
            btn[0] = SDL_GameControllerGetButton(dev->gamecontroller, SDL_CONTROLLER_BUTTON_A) ? 0x80 : 0;
            btn[1] = SDL_GameControllerGetButton(dev->gamecontroller, SDL_CONTROLLER_BUTTON_B) ? 0x80 : 0;
            btn[2] = SDL_GameControllerGetButton(dev->gamecontroller, SDL_CONTROLLER_BUTTON_X) ? 0x80 : 0;
            btn[3] = SDL_GameControllerGetButton(dev->gamecontroller, SDL_CONTROLLER_BUTTON_Y) ? 0x80 : 0;
            btn[4] = SDL_GameControllerGetButton(dev->gamecontroller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ? 0x80 : 0;
            btn[5] = SDL_GameControllerGetButton(dev->gamecontroller, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 0x80 : 0;
            btn[6] = SDL_GameControllerGetButton(dev->gamecontroller, SDL_CONTROLLER_BUTTON_BACK) ? 0x80 : 0;
            btn[7] = SDL_GameControllerGetButton(dev->gamecontroller, SDL_CONTROLLER_BUTTON_START) ? 0x80 : 0;
            /* Buttons 8..9: triggers (LT, RT) */
            btn[8] = (trig_l > 8000) ? 0x80 : 0;
            btn[9] = (trig_r > 8000) ? 0x80 : 0;
            btn[10] = SDL_GameControllerGetButton(dev->gamecontroller, SDL_CONTROLLER_BUTTON_LEFTSTICK) ? 0x80 : 0;
            btn[11] = SDL_GameControllerGetButton(dev->gamecontroller, SDL_CONTROLLER_BUTTON_RIGHTSTICK) ? 0x80 : 0;
            btn[12] = SDL_GameControllerGetButton(dev->gamecontroller, SDL_CONTROLLER_BUTTON_GUIDE) ? 0x80 : 0;

            /* Right stick also available via Z/Rz */
            raw_lz = raw_rx;
            raw_rz = raw_ry;
        } else if (dev->joystick) {
            int num_axes = SDL_JoystickNumAxes(dev->joystick);
            if (num_axes > 0) raw_lx = SDL_JoystickGetAxis(dev->joystick, 0);
            if (num_axes > 1) raw_ly = SDL_JoystickGetAxis(dev->joystick, 1);
            if (num_axes > 2) raw_rx = SDL_JoystickGetAxis(dev->joystick, 2);
            if (num_axes > 3) raw_ry = SDL_JoystickGetAxis(dev->joystick, 3);
            raw_lz = raw_rx;
            raw_rz = raw_ry;

            if (SDL_JoystickNumHats(dev->joystick) > 0) {
                Uint8 hat = SDL_JoystickGetHat(dev->joystick, 0);
                if ((hat & SDL_HAT_UP) && (hat & SDL_HAT_RIGHT)) pov = 4500;
                else if ((hat & SDL_HAT_DOWN) && (hat & SDL_HAT_RIGHT)) pov = 13500;
                else if ((hat & SDL_HAT_DOWN) && (hat & SDL_HAT_LEFT)) pov = 22500;
                else if ((hat & SDL_HAT_UP) && (hat & SDL_HAT_LEFT)) pov = 31500;
                else if (hat & SDL_HAT_UP) pov = 0;
                else if (hat & SDL_HAT_RIGHT) pov = 9000;
                else if (hat & SDL_HAT_DOWN) pov = 18000;
                else if (hat & SDL_HAT_LEFT) pov = 27000;

                if (abs(raw_lx) <= 4000 && abs(raw_ly) <= 4000) {
                    if ((hat & SDL_HAT_LEFT) && !(hat & SDL_HAT_RIGHT)) raw_lx = -32768;
                    else if ((hat & SDL_HAT_RIGHT) && !(hat & SDL_HAT_LEFT)) raw_lx = 32767;
                    if ((hat & SDL_HAT_UP) && !(hat & SDL_HAT_DOWN)) raw_ly = -32768;
                    else if ((hat & SDL_HAT_DOWN) && !(hat & SDL_HAT_UP)) raw_ly = 32767;
                }
            }

            int num_btns = SDL_JoystickNumButtons(dev->joystick);
            for (int b = 0; b < 32 && b < num_btns; b++) {
                btn[b] = SDL_JoystickGetButton(dev->joystick, b) ? 0x80 : 0;
            }
        }

        s32 lX = scale_axis(raw_lx, dev->axis_min[0], dev->axis_max[0], dev->deadzone[0]);
        s32 lY = scale_axis(raw_ly, dev->axis_min[1], dev->axis_max[1], dev->deadzone[1]);
        s32 lZ = scale_axis(raw_lz, dev->axis_min[2], dev->axis_max[2], dev->deadzone[2]);
        s32 lRx = scale_axis(raw_rx, dev->axis_min[3], dev->axis_max[3], dev->deadzone[3]);
        s32 lRy = scale_axis(raw_ry, dev->axis_min[4], dev->axis_max[4], dev->deadzone[4]);
        s32 lRz = scale_axis(raw_rz, dev->axis_min[5], dev->axis_max[5], dev->deadzone[5]);
        s32 slider0 = scale_axis(raw_slider0, dev->axis_min[6], dev->axis_max[6], dev->deadzone[6]);
        s32 slider1 = scale_axis(raw_slider1, dev->axis_min[7], dev->axis_max[7], dev->deadzone[7]);

        if (cbData >= 4)  wr32(lpvData + 0, (u32)lX);
        if (cbData >= 8)  wr32(lpvData + 4, (u32)lY);
        if (cbData >= 12) wr32(lpvData + 8, (u32)lZ);
        if (cbData >= 16) wr32(lpvData + 12, (u32)lRx);
        if (cbData >= 20) wr32(lpvData + 16, (u32)lRy);
        if (cbData >= 24) wr32(lpvData + 20, (u32)lRz);
        if (cbData >= 28) wr32(lpvData + 24, (u32)slider0);
        if (cbData >= 32) wr32(lpvData + 28, (u32)slider1);
        if (cbData >= 36) wr32(lpvData + 32, pov);
        if (cbData >= 40) wr32(lpvData + 36, 0xFFFFFFFFu);
        if (cbData >= 44) wr32(lpvData + 40, 0xFFFFFFFFu);
        if (cbData >= 48) wr32(lpvData + 44, 0xFFFFFFFFu);
        for (u32 b = 0; b < 32; b++) {
            if (cbData >= 48 + b + 1) {
                wr8(lpvData + 48 + b, btn[b]);
            }
        }
        return DI_OK;
    }

    return DI_OK;
}

/* vtable index 10: GetDeviceData(this, cbObjectData, rgdod, pdwInOut, dwFlags) */
static u32 imp_dev_GetDeviceData(Cpu *c)
{
    struct DiDevHost *dev = (struct DiDevHost *)com_host(ARG(0));
    u32 cbObjectData = ARG(1);
    u32 rgdod = ARG(2);
    u32 pdwInOut = ARG(3);
    u32 dwFlags = ARG(4);

    if (!dev || !pdwInOut) return E_INVALIDARG;
    if (cbObjectData != 16) return E_INVALIDARG;

    pump_key_events(dev);

    u32 req = rd32(pdwInOut);
    int is_peek = (dwFlags & DIGDD_PEEK) != 0;

    if (rgdod == 0) {
        wr32(pdwInOut, (u32)dev->q_count);
        if (!is_peek) {
            dev->q_head = 0;
            dev->q_count = 0;
        }
        return DI_OK;
    }

    u32 to_copy = (req < (u32)dev->q_count) ? req : (u32)dev->q_count;
    for (u32 i = 0; i < to_copy; i++) {
        int idx = (dev->q_head + i) % KEY_QUEUE_CAP;
        u32 elem = rgdod + i * 16;
        wr32(elem + 0, dev->queue[idx].dwOfs);
        wr32(elem + 4, dev->queue[idx].dwData);
        wr32(elem + 8, dev->queue[idx].dwTimeStamp);
        wr32(elem + 12, dev->queue[idx].dwSequence);
    }

    if (!is_peek) {
        dev->q_head = (dev->q_head + to_copy) % KEY_QUEUE_CAP;
        dev->q_count -= (int)to_copy;
    }

    wr32(pdwInOut, to_copy);
    return DI_OK;
}

/* vtable index 11: SetDataFormat(this, lpdf) */
static u32 imp_dev_SetDataFormat(Cpu *c)
{
    (void)c;
    return DI_OK;
}

/* vtable index 12: SetEventNotification(this, hEvent) */
static u32 imp_dev_SetEventNotification(Cpu *c)
{
    struct DiDevHost *dev = (struct DiDevHost *)com_host(ARG(0));
    u32 hEvent = ARG(1);
    if (dev) dev->event_handle = hEvent;
    return DI_OK;
}

/* vtable index 13: SetCooperativeLevel(this, hwnd, dwFlags) */
static u32 imp_dev_SetCooperativeLevel(Cpu *c)
{
    struct DiDevHost *dev = (struct DiDevHost *)com_host(ARG(0));
    u32 hwnd = ARG(1);
    u32 dwFlags = ARG(2);
    if (dev) {
        dev->hwnd = hwnd;
        dev->coop_flags = dwFlags;
    }
    return DI_OK;
}

/* vtable index 14: GetObjectInfo(this, pdidoi, dwObj, dwHow) */
static u32 imp_dev_GetObjectInfo(Cpu *c)
{
    (void)c;
    plat_log("warn: IDirectInputDevice::GetObjectInfo");
    return DI_OK;
}

/* vtable index 15: GetDeviceInfo(this, pdidi) */
static u32 imp_dev_GetDeviceInfo(Cpu *c)
{
    struct DiDevHost *dev = (struct DiDevHost *)com_host(ARG(0));
    u32 pdidi = ARG(1);

    if (!dev || !pdidi) return E_INVALIDARG;

    u32 sz = rd32(pdidi);
    if (sz < 4) return E_INVALIDARG;
    if (sz > 592) sz = 592;
    memset(G(pdidi + 4), 0, sz - 4);

    u32 inst_d1, prod_d1, dev_type;
    const char *inst_name, *prod_name;

    if (dev->kind == DEV_KEYBOARD) {
        inst_d1 = prod_d1 = 0x6F1D2B61u;
        dev_type = DIDEVTYPE_KEYBOARD | (DIDEVTYPEKEYBOARD_PCENH << 8);
        inst_name = "Keyboard";
        prod_name = "Standard 101/102-Key or Microsoft Natural Keyboard";
    } else if (dev->kind == DEV_MOUSE) {
        inst_d1 = prod_d1 = 0x6F1D2B60u;
        dev_type = DIDEVTYPE_MOUSE | (DIDEVTYPEMOUSE_TRADITIONAL << 8);
        inst_name = "Mouse";
        prod_name = "Standard Mouse";
    } else {
        inst_d1 = 0x6F1D2B70u + (u32)dev->joy_index;
        prod_d1 = 0x6F1D2B70u;
        dev_type = DIDEVTYPE_JOYSTICK | (DIDEVTYPEJOYSTICK_GAMEPAD << 8);
        inst_name = "Joystick";
        prod_name = "Standard Gamepad";
        if (dev->gamecontroller) {
            const char *name = SDL_GameControllerName(dev->gamecontroller);
            if (name && *name) { inst_name = name; prod_name = name; }
        } else if (dev->joystick) {
            const char *name = SDL_JoystickName(dev->joystick);
            if (name && *name) { inst_name = name; prod_name = name; }
        }
    }

    if (sz >= 20) {
        wr32(pdidi + 4, inst_d1);
        wr16(pdidi + 8, 0xD5A0);
        wr16(pdidi + 10, 0x11CF);
        for (int i = 0; i < 8; i++) wr8(pdidi + 12 + i, guid_d4_dinput[i]);
    }
    if (sz >= 36) {
        wr32(pdidi + 20, prod_d1);
        wr16(pdidi + 24, 0xD5A0);
        wr16(pdidi + 26, 0x11CF);
        for (int i = 0; i < 8; i++) wr8(pdidi + 28 + i, guid_d4_dinput[i]);
    }
    if (sz >= 40) {
        wr32(pdidi + 36, dev_type);
    }
    if (sz >= 300) {
        gstrcpy(pdidi + 40, inst_name, 260);
    }
    if (sz >= 560) {
        gstrcpy(pdidi + 300, prod_name, 260);
    }

    return DI_OK;
}

/* vtable index 16: RunControlPanel(this, hwndOwner, dwFlags) */
static u32 imp_dev_RunControlPanel(Cpu *c)
{
    (void)c;
    plat_log("warn: IDirectInputDevice::RunControlPanel");
    return DI_OK;
}

/* vtable index 17: Initialize(this, hinst, dwVersion, rguid) */
static u32 imp_dev_Initialize(Cpu *c)
{
    (void)c;
    plat_log("warn: IDirectInputDevice::Initialize");
    return DI_OK;
}

/* -------------------------------------------------------------------------
 * IDirectInputDevice2A Specific Methods
 * ------------------------------------------------------------------------- */

/* vtable index 18: CreateEffect(this, rguid, lpeff, ppdeff, punkOuter) */
static u32 imp_dev2_CreateEffect(Cpu *c)
{
    (void)c;
    plat_log("warn: IDirectInputDevice2::CreateEffect");
    return E_NOTIMPL;
}

/* vtable index 19: EnumEffects(this, lpCallback, pvRef, dwEffType) */
static u32 imp_dev2_EnumEffects(Cpu *c)
{
    (void)c;
    plat_log("warn: IDirectInputDevice2::EnumEffects");
    return DI_OK;
}

/* vtable index 20: GetEffectInfo(this, pdei, rguid) */
static u32 imp_dev2_GetEffectInfo(Cpu *c)
{
    (void)c;
    plat_log("warn: IDirectInputDevice2::GetEffectInfo");
    return E_NOTIMPL;
}

/* vtable index 21: GetForceFeedbackState(this, pdwOut) */
static u32 imp_dev2_GetForceFeedbackState(Cpu *c)
{
    u32 pdwOut = ARG(1);
    plat_log("warn: IDirectInputDevice2::GetForceFeedbackState");
    if (pdwOut) wr32(pdwOut, 0);
    return DI_OK;
}

/* vtable index 22: SendForceFeedbackCommand(this, dwFlags) */
static u32 imp_dev2_SendForceFeedbackCommand(Cpu *c)
{
    (void)c;
    plat_log("warn: IDirectInputDevice2::SendForceFeedbackCommand");
    return DI_OK;
}

/* vtable index 23: EnumCreatedEffectObjects(this, lpCallback, pvRef, dwFlags) */
static u32 imp_dev2_EnumCreatedEffectObjects(Cpu *c)
{
    (void)c;
    plat_log("warn: IDirectInputDevice2::EnumCreatedEffectObjects");
    return DI_OK;
}

/* vtable index 24: Escape(this, pesc) */
static u32 imp_dev2_Escape(Cpu *c)
{
    (void)c;
    plat_log("warn: IDirectInputDevice2::Escape");
    return E_NOTIMPL;
}

/* vtable index 25: Poll(this) */
static u32 imp_dev2_Poll(Cpu *c)
{
    (void)c;
    plat_pump();
    SDL_JoystickUpdate();
    SDL_GameControllerUpdate();
    return DI_OK;
}

/* vtable index 26: SendDeviceData(this, cbObjectData, rgdod, pdwInOut, dwFlags) */
static u32 imp_dev2_SendDeviceData(Cpu *c)
{
    (void)c;
    plat_log("warn: IDirectInputDevice2::SendDeviceData");
    return E_NOTIMPL;
}

/* -------------------------------------------------------------------------
 * Vtables
 * ------------------------------------------------------------------------- */

static const struct ImpDef di_methods[] = {
    /* 0 */ IMPORT(di_QueryInterface, 3),
    /* 1 */ IMPORT(di_AddRef, 1),
    /* 2 */ IMPORT(di_Release, 1),
    /* 3 */ IMPORT(di_CreateDevice, 4),
    /* 4 */ IMPORT(di_EnumDevices, 5),
    /* 5 */ IMPORT(di_GetDeviceStatus, 2),
    /* 6 */ IMPORT(di_RunControlPanel, 3),
    /* 7 */ IMPORT(di_Initialize, 3),
};

static const struct ImpDef dev1_methods[] = {
    /* 0 */ IMPORT(dev_QueryInterface, 3),
    /* 1 */ IMPORT(dev_AddRef, 1),
    /* 2 */ IMPORT(dev_Release, 1),
    /* 3 */ IMPORT(dev_GetCapabilities, 2),
    /* 4 */ IMPORT(dev_EnumObjects, 4),
    /* 5 */ IMPORT(dev_GetProperty, 3),
    /* 6 */ IMPORT(dev_SetProperty, 3),
    /* 7 */ IMPORT(dev_Acquire, 1),
    /* 8 */ IMPORT(dev_Unacquire, 1),
    /* 9 */ IMPORT(dev_GetDeviceState, 3),
    /* 10 */ IMPORT(dev_GetDeviceData, 5),
    /* 11 */ IMPORT(dev_SetDataFormat, 2),
    /* 12 */ IMPORT(dev_SetEventNotification, 2),
    /* 13 */ IMPORT(dev_SetCooperativeLevel, 3),
    /* 14 */ IMPORT(dev_GetObjectInfo, 4),
    /* 15 */ IMPORT(dev_GetDeviceInfo, 2),
    /* 16 */ IMPORT(dev_RunControlPanel, 3),
    /* 17 */ IMPORT(dev_Initialize, 4),
};

static const struct ImpDef dev2_methods[] = {
    /* 0 */ IMPORT(dev_QueryInterface, 3),
    /* 1 */ IMPORT(dev_AddRef, 1),
    /* 2 */ IMPORT(dev_Release, 1),
    /* 3 */ IMPORT(dev_GetCapabilities, 2),
    /* 4 */ IMPORT(dev_EnumObjects, 4),
    /* 5 */ IMPORT(dev_GetProperty, 3),
    /* 6 */ IMPORT(dev_SetProperty, 3),
    /* 7 */ IMPORT(dev_Acquire, 1),
    /* 8 */ IMPORT(dev_Unacquire, 1),
    /* 9 */ IMPORT(dev_GetDeviceState, 3),
    /* 10 */ IMPORT(dev_GetDeviceData, 5),
    /* 11 */ IMPORT(dev_SetDataFormat, 2),
    /* 12 */ IMPORT(dev_SetEventNotification, 2),
    /* 13 */ IMPORT(dev_SetCooperativeLevel, 3),
    /* 14 */ IMPORT(dev_GetObjectInfo, 4),
    /* 15 */ IMPORT(dev_GetDeviceInfo, 2),
    /* 16 */ IMPORT(dev_RunControlPanel, 3),
    /* 17 */ IMPORT(dev_Initialize, 4),
    /* 18 */ IMPORT(dev2_CreateEffect, 5),
    /* 19 */ IMPORT(dev2_EnumEffects, 4),
    /* 20 */ IMPORT(dev2_GetEffectInfo, 3),
    /* 21 */ IMPORT(dev2_GetForceFeedbackState, 2),
    /* 22 */ IMPORT(dev2_SendForceFeedbackCommand, 2),
    /* 23 */ IMPORT(dev2_EnumCreatedEffectObjects, 4),
    /* 24 */ IMPORT(dev2_Escape, 2),
    /* 25 */ IMPORT(dev2_Poll, 1),
    /* 26 */ IMPORT(dev2_SendDeviceData, 5),
};

/* -------------------------------------------------------------------------
 * Exported functions (DINPUT.dll)
 * ------------------------------------------------------------------------- */

/* HRESULT WINAPI DirectInputCreateA(HINSTANCE hinst, DWORD dwVersion, LPDIRECTINPUTA *ppDI, LPUNKNOWN punkOuter) */
static u32 imp_DirectInputCreateA(Cpu *c)
{
    u32 hinst = ARG(0);
    u32 dwVersion = ARG(1);
    u32 ppDI = ARG(2);
    u32 punkOuter = ARG(3);
    (void)hinst;
    (void)dwVersion;

    if (punkOuter != 0) return CLASS_E_NOAGGREGATION;
    if (!ppDI) return E_INVALIDARG;

    struct DiHost *di = (struct DiHost *)calloc(1, sizeof(*di));
    if (!di) return E_OUTOFMEMORY;

    di->ref_count = 1;
    ensure_sdl_joystick();

    u32 obj = com_new(di_methods, 8, di);
    wr32(ppDI, obj);
    return DI_OK;
}

const struct ImpDef imp_dinput[] = {
    IMPORT(DirectInputCreateA, 4),
};
IMPORT_TABLE(dinput);
