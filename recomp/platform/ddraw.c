/* DirectDraw implementation for Future Cop (DDRAW.dll)
 * Supports IDirectDraw, IDirectDraw2, IDirectDrawSurface/2/3,
 * IDirectDrawPalette, and IDirectDrawClipper.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <SDL.h>
#include "host.h"
#include "win.h"

/* Weak fallback for win_present in case u32_win.c has not provided it yet */
__attribute__((weak)) void win_present(const void *pixels, int w, int h, int pitch_bytes, int bits, u32 rmask, u32 gmask, u32 bmask)
{
    (void)pixels; (void)w; (void)h; (void)pitch_bytes; (void)bits;
    (void)rmask; (void)gmask; (void)bmask;
}

/* ---- Return Codes & Constants -------------------------------------------------------- */
#define DD_OK                   0x00000000u
#define S_OK                    0x00000000u
#define S_FALSE                 0x00000001u
#define E_NOTIMPL               0x80004001u
#define E_NOINTERFACE           0x80004002u
#define E_POINTER               0x80004003u
#define E_FAIL                  0x80004005u
#define DDERR_GENERIC           0x80004005u
#define DDERR_UNSUPPORTED       0x80004001u
#define DDERR_INVALIDPARAMS     0x80070057u
#define DDERR_NOTFOUND          0x887600FFu
#define DDERR_NOPALETTEATTACHED 0x88760240u
#define DDERR_NOCLIPPERATTACHED 0x8876023Fu

/* DDSURFACEDESC dwFlags */
#define DDSD_CAPS               0x00000001u
#define DDSD_HEIGHT             0x00000002u
#define DDSD_WIDTH              0x00000004u
#define DDSD_PITCH              0x00000008u
#define DDSD_BACKBUFFERCOUNT    0x00000020u
#define DDSD_LPSURFACE          0x00000800u
#define DDSD_PIXELFORMAT        0x00001000u
#define DDSD_CKSRCBLT           0x00010000u
#define DDSD_REFRESHRATE        0x00040000u

/* DDPIXELFORMAT dwFlags */
#define DDPF_ALPHAPIXELS        0x00000001u
#define DDPF_PALETTEINDEXED8    0x00000020u
#define DDPF_RGB                0x00000040u

/* DDSCAPS dwCaps */
#define DDSCAPS_BACKBUFFER      0x00000004u
#define DDSCAPS_COMPLEX         0x00000008u
#define DDSCAPS_FLIP            0x00000010u
#define DDSCAPS_FRONTBUFFER     0x00000020u
#define DDSCAPS_OFFSCREENPLAIN  0x00000040u
#define DDSCAPS_PRIMARYSURFACE  0x00000200u
#define DDSCAPS_SYSTEMMEMORY    0x00000800u
#define DDSCAPS_VIDEOMEMORY     0x00004000u

/* DDCAPS dwCaps */
#define DDCAPS_3D               0x00000001u
#define DDCAPS_BLT              0x00000040u
#define DDCAPS_COLORKEY         0x00000200u
#define DDCAPS_BLTCOLORFILL     0x04000000u
#define DDCAPS_CANBLTSYS        0x08000000u
#define DDCAPS_CANCLIP          0x00080000u

/* DDBLT flags */
#define DDBLT_COLORFILL         0x00000400u
#define DDBLT_KEYDEST           0x00004000u
#define DDBLT_KEYSRC            0x00008000u
#define DDBLT_WAIT              0x01000000u
#define DDBLT_KEYSRCOVERRIDE    0x00040000u

/* DDBLTFAST flags */
#define DDBLTFAST_SRCCOLORKEY   0x00000001u
#define DDBLTFAST_DESTCOLORKEY  0x00000002u
#define DDBLTFAST_WAIT          0x00000010u

/* DDCKEY flags */
#define DDCKEY_SRCBLT           0x00000008u
#define DDCKEY_DESTBLT          0x00000002u

/* ---- Data Structures ----------------------------------------------------------------- */

typedef struct HostPalette {
    u32 refcount;
    u32 flags;
    struct { u8 r, g, b, flags; } entries[256];
} HostPalette;

typedef struct HostClipper {
    u32 refcount;
    u32 hwnd;
} HostClipper;

struct HostDDraw;

typedef struct HostSurface {
    u32 refcount;
    struct HostDDraw *ddraw;
    int is_primary;
    int is_backbuffer;
    u32 width;
    u32 height;
    u32 bpp;
    u32 pitch;
    u32 pixels;       /* guest address from galloc */
    u32 caps;
    u32 rmask, gmask, bmask;
    u32 colorkey_src_low;
    u32 colorkey_src_high;
    int has_colorkey_src;
    u32 attached_back;   /* guest obj */
    u32 attached_front;  /* guest obj */
    u32 palette_obj;     /* guest obj */
    u32 clipper_obj;     /* guest obj */
    u32 surf1_obj;       /* guest obj */
    u32 surf2_obj;       /* guest obj */
    u32 surf3_obj;       /* guest obj */
} HostSurface;

typedef struct HostDDraw {
    u32 refcount;
    u32 hwnd;
    u32 coop_flags;
    u32 width;
    u32 height;
    u32 bpp;
    u32 primary_surf; /* guest obj */
    u32 ddraw1_obj;   /* guest obj */
    u32 ddraw2_obj;   /* guest obj */
} HostDDraw;

/* Forward declarations of method tables */
static const struct ImpDef methods_palette[];
static const struct ImpDef methods_clipper[];
static const struct ImpDef methods_surface[];
static const struct ImpDef methods_surface2[];
static const struct ImpDef methods_surface3[];
static const struct ImpDef methods_ddraw[];
static const struct ImpDef methods_ddraw2[];

/* ---- GUID Helpers -------------------------------------------------------------------- */

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

static const u8 guid_d4_unknown[8] = {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};
static const u8 guid_d4_ddraw1[8]  = {0xB0, 0x63, 0x00, 0x20, 0xAF, 0xC2, 0xCD, 0x35};
static const u8 guid_d4_ddraw2[8]  = {0xA2, 0xDE, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56};
static const u8 guid_d4_surf1[8]   = {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60};
static const u8 guid_d4_surf2[8]   = {0x94, 0x41, 0xA8, 0x23, 0x03, 0xC1, 0x0E, 0x27};
static const u8 guid_d4_surf3[8]   = {0xA1, 0xD5, 0x00, 0xAA, 0x00, 0xA3, 0xF9, 0xC5};
static const u8 guid_d4_pal[8]     = {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60};
static const u8 guid_d4_clip[8]    = {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60};
static const u8 guid_d4_d3d[8]     = {0xA3, 0x1A, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56};

/* ---- Surface Presentation ------------------------------------------------------------ */

static u16 conv_buffer[1024 * 768];

static void present_surface(HostSurface *surf)
{
    if (!surf || !surf->pixels) return;
    int w = (int)surf->width;
    int h = (int)surf->height;
    if (w <= 0 || h <= 0) return;

    if (surf->bpp == 16) {
        u32 rmask = surf->rmask ? surf->rmask : 0xF800u;
        u32 gmask = surf->gmask ? surf->gmask : 0x07E0u;
        u32 bmask = surf->bmask ? surf->bmask : 0x001Fu;
        win_present(G(surf->pixels), w, h, (int)surf->pitch, 16, rmask, gmask, bmask);
    } else if (surf->bpp == 8) {
        if (w * h <= 1024 * 768) {
            HostPalette *pal = surf->palette_obj ? (HostPalette *)com_host(surf->palette_obj) : NULL;
            const u8 *src = (const u8 *)G(surf->pixels);
            for (int y = 0; y < h; y++) {
                const u8 *srow = src + y * surf->pitch;
                u16 *drow = conv_buffer + y * w;
                for (int x = 0; x < w; x++) {
                    u8 idx = srow[x];
                    u8 r = pal ? pal->entries[idx].r : idx;
                    u8 g = pal ? pal->entries[idx].g : idx;
                    u8 b = pal ? pal->entries[idx].b : idx;
                    drow[x] = (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                }
            }
            win_present(conv_buffer, w, h, w * 2, 16, 0xF800u, 0x07E0u, 0x001Fu);
        }
    }
}

/* Helper to fill a DDSURFACEDESC at guest address lpDDSD */
static void fill_surface_desc(HostSurface *surf, u32 desc_p)
{
    if (!desc_p || !surf) return;
    wr32(desc_p + 0x00, 108); /* dwSize */
    u32 flags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_CAPS | DDSD_PIXELFORMAT;
    if (surf->pixels) flags |= DDSD_LPSURFACE;
    if (surf->is_primary && surf->attached_back) flags |= DDSD_BACKBUFFERCOUNT;
    if (surf->has_colorkey_src) flags |= DDSD_CKSRCBLT;
    wr32(desc_p + 0x04, flags);
    wr32(desc_p + 0x08, surf->height);
    wr32(desc_p + 0x0C, surf->width);
    wr32(desc_p + 0x10, surf->pitch);
    wr32(desc_p + 0x14, (surf->is_primary && surf->attached_back) ? 1 : 0);
    wr32(desc_p + 0x18, 60); /* refresh rate */
    wr32(desc_p + 0x1C, 0);
    wr32(desc_p + 0x20, 0);
    wr32(desc_p + 0x24, surf->pixels);
    wr32(desc_p + 0x28, 0); wr32(desc_p + 0x2C, 0);
    wr32(desc_p + 0x30, 0); wr32(desc_p + 0x34, 0);
    wr32(desc_p + 0x38, 0); wr32(desc_p + 0x3C, 0);
    wr32(desc_p + 0x40, surf->colorkey_src_low);
    wr32(desc_p + 0x44, surf->colorkey_src_high);

    /* ddpfPixelFormat (32 bytes at 0x48) */
    wr32(desc_p + 0x48 + 0x00, 32);
    if (surf->bpp == 16) {
        wr32(desc_p + 0x48 + 0x04, DDPF_RGB);
        wr32(desc_p + 0x48 + 0x08, 0);
        wr32(desc_p + 0x48 + 0x0C, 16);
        wr32(desc_p + 0x48 + 0x10, surf->rmask ? surf->rmask : 0xF800u);
        wr32(desc_p + 0x48 + 0x14, surf->gmask ? surf->gmask : 0x07E0u);
        wr32(desc_p + 0x48 + 0x18, surf->bmask ? surf->bmask : 0x001Fu);
        wr32(desc_p + 0x48 + 0x1C, 0);
    } else {
        wr32(desc_p + 0x48 + 0x04, DDPF_RGB | DDPF_PALETTEINDEXED8);
        wr32(desc_p + 0x48 + 0x08, 0);
        wr32(desc_p + 0x48 + 0x0C, 8);
        wr32(desc_p + 0x48 + 0x10, 0);
        wr32(desc_p + 0x48 + 0x14, 0);
        wr32(desc_p + 0x48 + 0x18, 0);
        wr32(desc_p + 0x48 + 0x1C, 0);
    }

    /* ddsCaps (4 bytes at 0x68) */
    wr32(desc_p + 0x68, surf->caps);
}

/* ---- IDirectDrawPalette Methods ------------------------------------------------------ */

/* 0 */
static u32 imp_pal_QueryInterface(Cpu *c)
{
    u32 this_obj = ARG(0);
    u32 riid = ARG(1);
    u32 ppv = ARG(2);
    if (!ppv) return DDERR_INVALIDPARAMS;

    if (match_guid(riid, 0x00000000u, 0x0000, 0x0000, guid_d4_unknown) ||
        match_guid(riid, 0x6C14DB84u, 0xA733, 0x11CE, guid_d4_pal)) {
        wr32(ppv, this_obj);
        return DD_OK;
    }
    log_unknown_iid("IDirectDrawPalette::QueryInterface", riid);
    return E_NOINTERFACE;
}

/* 1 */
static u32 imp_pal_AddRef(Cpu *c)
{
    HostPalette *pal = (HostPalette *)com_host(ARG(0));
    return pal ? ++pal->refcount : 1;
}

/* 2 */
static u32 imp_pal_Release(Cpu *c)
{
    HostPalette *pal = (HostPalette *)com_host(ARG(0));
    if (!pal) return 0;
    if (pal->refcount > 0) pal->refcount--;
    return pal->refcount;
}

/* 3 */
static u32 imp_pal_GetCaps(Cpu *c)
{
    HostPalette *pal = (HostPalette *)com_host(ARG(0));
    u32 lpdwCaps = ARG(1);
    if (lpdwCaps && pal) wr32(lpdwCaps, pal->flags);
    return DD_OK;
}

/* 4 */
static u32 imp_pal_GetEntries(Cpu *c)
{
    HostPalette *pal = (HostPalette *)com_host(ARG(0));
    u32 dwFlags = ARG(1);
    u32 dwBase = ARG(2);
    u32 dwNum = ARG(3);
    u32 lpEntries = ARG(4);
    (void)dwFlags;
    if (!pal || !lpEntries) return DDERR_INVALIDPARAMS;
    if (dwBase >= 256) return DDERR_INVALIDPARAMS;
    if (dwBase + dwNum > 256) dwNum = 256 - dwBase;

    for (u32 i = 0; i < dwNum; i++) {
        wr8(lpEntries + 4 * i + 0, pal->entries[dwBase + i].r);
        wr8(lpEntries + 4 * i + 1, pal->entries[dwBase + i].g);
        wr8(lpEntries + 4 * i + 2, pal->entries[dwBase + i].b);
        wr8(lpEntries + 4 * i + 3, pal->entries[dwBase + i].flags);
    }
    return DD_OK;
}

/* 5 */
static u32 imp_pal_Initialize(Cpu *c)
{
    plat_log("warn: IDirectDrawPalette::Initialize");
    return DD_OK;
}

/* 6 */
static u32 imp_pal_SetEntries(Cpu *c)
{
    HostPalette *pal = (HostPalette *)com_host(ARG(0));
    u32 dwFlags = ARG(1);
    u32 dwBase = ARG(2);
    u32 dwNum = ARG(3);
    u32 lpEntries = ARG(4);
    (void)dwFlags;
    if (!pal || !lpEntries) return DDERR_INVALIDPARAMS;
    if (dwBase >= 256) return DDERR_INVALIDPARAMS;
    if (dwBase + dwNum > 256) dwNum = 256 - dwBase;

    for (u32 i = 0; i < dwNum; i++) {
        pal->entries[dwBase + i].r = rd8(lpEntries + 4 * i + 0);
        pal->entries[dwBase + i].g = rd8(lpEntries + 4 * i + 1);
        pal->entries[dwBase + i].b = rd8(lpEntries + 4 * i + 2);
        pal->entries[dwBase + i].flags = rd8(lpEntries + 4 * i + 3);
    }
    return DD_OK;
}

static const struct ImpDef methods_palette[] = {
    /* 0 */ IMPORT(pal_QueryInterface, 3),
    /* 1 */ IMPORT(pal_AddRef, 1),
    /* 2 */ IMPORT(pal_Release, 1),
    /* 3 */ IMPORT(pal_GetCaps, 2),
    /* 4 */ IMPORT(pal_GetEntries, 5),
    /* 5 */ IMPORT(pal_Initialize, 4),
    /* 6 */ IMPORT(pal_SetEntries, 5),
};

/* ---- IDirectDrawClipper Methods ------------------------------------------------------ */

/* 0 */
static u32 imp_clip_QueryInterface(Cpu *c)
{
    u32 this_obj = ARG(0);
    u32 riid = ARG(1);
    u32 ppv = ARG(2);
    if (!ppv) return DDERR_INVALIDPARAMS;

    if (match_guid(riid, 0x00000000u, 0x0000, 0x0000, guid_d4_unknown) ||
        match_guid(riid, 0x6C14DB85u, 0xA733, 0x11CE, guid_d4_clip)) {
        wr32(ppv, this_obj);
        return DD_OK;
    }
    log_unknown_iid("IDirectDrawClipper::QueryInterface", riid);
    return E_NOINTERFACE;
}

/* 1 */
static u32 imp_clip_AddRef(Cpu *c)
{
    HostClipper *clip = (HostClipper *)com_host(ARG(0));
    return clip ? ++clip->refcount : 1;
}

/* 2 */
static u32 imp_clip_Release(Cpu *c)
{
    HostClipper *clip = (HostClipper *)com_host(ARG(0));
    if (!clip) return 0;
    if (clip->refcount > 0) clip->refcount--;
    return clip->refcount;
}

/* 3 */
static u32 imp_clip_GetClipList(Cpu *c)
{
    plat_log("warn: IDirectDrawClipper::GetClipList");
    return DD_OK;
}

/* 4 */
static u32 imp_clip_GetHWnd(Cpu *c)
{
    HostClipper *clip = (HostClipper *)com_host(ARG(0));
    u32 lphWnd = ARG(1);
    if (lphWnd && clip) wr32(lphWnd, clip->hwnd);
    return DD_OK;
}

/* 5 */
static u32 imp_clip_Initialize(Cpu *c)
{
    plat_log("warn: IDirectDrawClipper::Initialize");
    return DD_OK;
}

/* 6 */
static u32 imp_clip_IsClipListChanged(Cpu *c)
{
    u32 lpb = ARG(1);
    if (lpb) wr32(lpb, 0);
    return DD_OK;
}

/* 7 */
static u32 imp_clip_SetClipList(Cpu *c)
{
    plat_log("warn: IDirectDrawClipper::SetClipList");
    return DD_OK;
}

/* 8 */
static u32 imp_clip_SetHWnd(Cpu *c)
{
    HostClipper *clip = (HostClipper *)com_host(ARG(0));
    u32 dwFlags = ARG(1);
    u32 hWnd = ARG(2);
    (void)dwFlags;
    if (clip) clip->hwnd = hWnd;
    return DD_OK;
}

static const struct ImpDef methods_clipper[] = {
    /* 0 */ IMPORT(clip_QueryInterface, 3),
    /* 1 */ IMPORT(clip_AddRef, 1),
    /* 2 */ IMPORT(clip_Release, 1),
    /* 3 */ IMPORT(clip_GetClipList, 4),
    /* 4 */ IMPORT(clip_GetHWnd, 2),
    /* 5 */ IMPORT(clip_Initialize, 3),
    /* 6 */ IMPORT(clip_IsClipListChanged, 2),
    /* 7 */ IMPORT(clip_SetClipList, 3),
    /* 8 */ IMPORT(clip_SetHWnd, 3),
};

/* ---- IDirectDrawSurface Methods ------------------------------------------------------ */

/* 0 */
static u32 imp_dds_QueryInterface(Cpu *c)
{
    HostSurface *surf = (HostSurface *)com_host(ARG(0));
    u32 riid = ARG(1);
    u32 ppv = ARG(2);
    if (!ppv || !surf) return DDERR_INVALIDPARAMS;

    if (match_guid(riid, 0x00000000u, 0x0000, 0x0000, guid_d4_unknown) ||
        match_guid(riid, 0x6C14DB81u, 0xA733, 0x11CE, guid_d4_surf1)) {
        if (!surf->surf1_obj) surf->surf1_obj = com_new(methods_surface, 36, surf);
        wr32(ppv, surf->surf1_obj);
        return DD_OK;
    }
    if (match_guid(riid, 0x57805885u, 0x6EEC, 0x11CF, guid_d4_surf2)) {
        if (!surf->surf2_obj) surf->surf2_obj = com_new(methods_surface2, 39, surf);
        wr32(ppv, surf->surf2_obj);
        return DD_OK;
    }
    if (match_guid(riid, 0xDA044E00u, 0x69B2, 0x11D0, guid_d4_surf3)) {
        if (!surf->surf3_obj) surf->surf3_obj = com_new(methods_surface3, 40, surf);
        wr32(ppv, surf->surf3_obj);
        return DD_OK;
    }

    log_unknown_iid("IDirectDrawSurface::QueryInterface", riid);
    return E_NOINTERFACE;
}

/* 1 */
static u32 imp_dds_AddRef(Cpu *c)
{
    HostSurface *surf = (HostSurface *)com_host(ARG(0));
    return surf ? ++surf->refcount : 1;
}

/* 2 */
static u32 imp_dds_Release(Cpu *c)
{
    HostSurface *surf = (HostSurface *)com_host(ARG(0));
    if (!surf) return 0;
    if (surf->refcount > 0) surf->refcount--;
    return surf->refcount;
}

/* 3 */
static u32 imp_dds_AddAttachedSurface(Cpu *c)
{
    plat_log("warn: IDirectDrawSurface::AddAttachedSurface");
    return DD_OK;
}

/* 4 */
static u32 imp_dds_AddOverlayDirtyRect(Cpu *c)
{
    plat_log("warn: IDirectDrawSurface::AddOverlayDirtyRect");
    return DD_OK;
}

/* 5 */
static u32 imp_dds_Blt(Cpu *c)
{
    HostSurface *dest = (HostSurface *)com_host(ARG(0));
    u32 lpDestRect = ARG(1);
    u32 lpDDSrc = ARG(2);
    u32 lpSrcRect = ARG(3);
    u32 dwFlags = ARG(4);
    u32 lpDDBltFx = ARG(5);

    if (!dest) return DDERR_INVALIDPARAMS;
    if (getenv("FCOP_DD_LOG"))
        plat_log("Blt dest %x (%ux%u primary %d caps %x) src %x flags %x", ARG(0), dest->width, dest->height,
                 dest->is_primary, dest->caps, lpDDSrc, dwFlags);

    /* Handle DDBLT_COLORFILL */
    if (dwFlags & DDBLT_COLORFILL) {
        u32 fill_color = lpDDBltFx ? rd32(lpDDBltFx + 0x48) : 0;
        int dx = 0, dy = 0, dw = (int)dest->width, dh = (int)dest->height;
        if (lpDestRect) {
            dx = (int)rd32(lpDestRect + 0);
            dy = (int)rd32(lpDestRect + 4);
            dw = (int)rd32(lpDestRect + 8) - dx;
            dh = (int)rd32(lpDestRect + 12) - dy;
        }
        if (dx < 0) { dw += dx; dx = 0; }
        if (dy < 0) { dh += dy; dy = 0; }
        if (dx + dw > (int)dest->width) dw = (int)dest->width - dx;
        if (dy + dh > (int)dest->height) dh = (int)dest->height - dy;

        if (dw > 0 && dh > 0 && dest->pixels) {
            if (dest->bpp == 16) {
                u16 col = (u16)fill_color;
                for (int y = 0; y < dh; y++) {
                    u16 *row = (u16 *)(MEM + dest->pixels + (dy + y) * dest->pitch) + dx;
                    for (int x = 0; x < dw; x++) row[x] = col;
                }
            } else if (dest->bpp == 8) {
                u8 col = (u8)fill_color;
                for (int y = 0; y < dh; y++) {
                    u8 *row = (u8 *)(MEM + dest->pixels + (dy + y) * dest->pitch) + dx;
                    memset(row, col, dw);
                }
            }
        }
        if (dest->is_primary) present_surface(dest);
        return DD_OK;
    }

    /* Surface-to-surface copy */
    if (!lpDDSrc) return DDERR_INVALIDPARAMS;
    HostSurface *src = (HostSurface *)com_host(lpDDSrc);
    if (!src || !src->pixels || !dest->pixels) {
        if (getenv("FCOP_DD_LOG")) plat_log("  Blt rejected: src %p pixels %x/%x", (void *)src, src ? src->pixels : 0, dest->pixels);
        return DDERR_INVALIDPARAMS;
    }

    int dx = 0, dy = 0, dw = (int)dest->width, dh = (int)dest->height;
    if (lpDestRect) {
        dx = (int)rd32(lpDestRect + 0);
        dy = (int)rd32(lpDestRect + 4);
        dw = (int)rd32(lpDestRect + 8) - dx;
        dh = (int)rd32(lpDestRect + 12) - dy;
    }
    int sx = 0, sy = 0, sw = (int)src->width, sh = (int)src->height;
    if (lpSrcRect) {
        sx = (int)rd32(lpSrcRect + 0);
        sy = (int)rd32(lpSrcRect + 4);
        sw = (int)rd32(lpSrcRect + 8) - sx;
        sh = (int)rd32(lpSrcRect + 12) - sy;
    }

    int w = dw < sw ? dw : sw;
    int h = dh < sh ? dh : sh;
    if (dx < 0) { sx -= dx; w += dx; dx = 0; }
    if (dy < 0) { sy -= dy; h += dy; dy = 0; }
    if (sx < 0) { dx -= sx; w += sx; sx = 0; }
    if (sy < 0) { dy -= sy; h += sy; sy = 0; }
    if (dx + w > (int)dest->width) w = (int)dest->width - dx;
    if (dy + h > (int)dest->height) h = (int)dest->height - dy;
    if (sx + w > (int)src->width) w = (int)src->width - sx;
    if (sy + h > (int)src->height) h = (int)src->height - sy;

    if (w <= 0 || h <= 0) {
        if (getenv("FCOP_DD_LOG")) plat_log("  Blt empty: d %d,%d s %d,%d %dx%d (src %ux%u)", dx, dy, sx, sy, w, h, src->width, src->height);
        return DD_OK;
    }

    int use_ckey = (dwFlags & DDBLT_KEYSRC) != 0;
    u32 ckey_low = 0, ckey_high = 0;
    if (use_ckey) {
        if (lpDDBltFx && (dwFlags & DDBLT_KEYSRCOVERRIDE)) {
            ckey_low = rd32(lpDDBltFx + 0x54);
            ckey_high = rd32(lpDDBltFx + 0x58);
        } else {
            ckey_low = src->colorkey_src_low;
            ckey_high = src->colorkey_src_high;
        }
    }

    if (dest->bpp == 16 && src->bpp == 16) {
        for (int y = 0; y < h; y++) {
            const u16 *srow = (const u16 *)(MEM + src->pixels + (sy + y) * src->pitch) + sx;
            u16 *drow = (u16 *)(MEM + dest->pixels + (dy + y) * dest->pitch) + dx;
            if (use_ckey) {
                for (int x = 0; x < w; x++) {
                    u16 p = srow[x];
                    if (p < ckey_low || p > ckey_high) drow[x] = p;
                }
            } else {
                memcpy(drow, srow, w * sizeof(u16));
            }
        }
    } else if (dest->bpp == 8 && src->bpp == 8) {
        for (int y = 0; y < h; y++) {
            const u8 *srow = (const u8 *)(MEM + src->pixels + (sy + y) * src->pitch) + sx;
            u8 *drow = (u8 *)(MEM + dest->pixels + (dy + y) * dest->pitch) + dx;
            if (use_ckey) {
                for (int x = 0; x < w; x++) {
                    u8 p = srow[x];
                    if (p < ckey_low || p > ckey_high) drow[x] = p;
                }
            } else {
                memcpy(drow, srow, w);
            }
        }
    } else if (dest->bpp == 16 && src->bpp == 8) {
        HostPalette *pal = src->palette_obj ? (HostPalette *)com_host(src->palette_obj) : NULL;
        for (int y = 0; y < h; y++) {
            const u8 *srow = (const u8 *)(MEM + src->pixels + (sy + y) * src->pitch) + sx;
            u16 *drow = (u16 *)(MEM + dest->pixels + (dy + y) * dest->pitch) + dx;
            for (int x = 0; x < w; x++) {
                u8 idx = srow[x];
                if (!use_ckey || (idx < ckey_low || idx > ckey_high)) {
                    u8 r = pal ? pal->entries[idx].r : idx;
                    u8 g = pal ? pal->entries[idx].g : idx;
                    u8 b = pal ? pal->entries[idx].b : idx;
                    drow[x] = (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                }
            }
        }
    }

    if (dest->is_primary) present_surface(dest);
    return DD_OK;
}

/* 6 */
static u32 imp_dds_BltBatch(Cpu *c)
{
    plat_log("warn: IDirectDrawSurface::BltBatch");
    return DD_OK;
}

/* 7 */
static u32 imp_dds_BltFast(Cpu *c)
{
    HostSurface *dest = (HostSurface *)com_host(ARG(0));
    u32 dwX = ARG(1);
    u32 dwY = ARG(2);
    u32 lpDDSrc = ARG(3);
    u32 lpSrcRect = ARG(4);
    u32 dwTrans = ARG(5);

    if (!dest || !lpDDSrc) return DDERR_INVALIDPARAMS;
    HostSurface *src = (HostSurface *)com_host(lpDDSrc);
    if (!src || !src->pixels || !dest->pixels) return DDERR_INVALIDPARAMS;

    int dx = (int)dwX;
    int dy = (int)dwY;
    int sx = 0, sy = 0, sw = (int)src->width, sh = (int)src->height;
    if (lpSrcRect) {
        sx = (int)rd32(lpSrcRect + 0);
        sy = (int)rd32(lpSrcRect + 4);
        sw = (int)rd32(lpSrcRect + 8) - sx;
        sh = (int)rd32(lpSrcRect + 12) - sy;
    }

    int w = sw, h = sh;
    if (dx < 0) { sx -= dx; w += dx; dx = 0; }
    if (dy < 0) { sy -= dy; h += dy; dy = 0; }
    if (sx < 0) { dx -= sx; w += sx; sx = 0; }
    if (sy < 0) { dy -= sy; h += sy; sy = 0; }
    if (dx + w > (int)dest->width) w = (int)dest->width - dx;
    if (dy + h > (int)dest->height) h = (int)dest->height - dy;
    if (sx + w > (int)src->width) w = (int)src->width - sx;
    if (sy + h > (int)src->height) h = (int)src->height - sy;

    if (w <= 0 || h <= 0) return DD_OK;

    int use_ckey = (dwTrans & DDBLTFAST_SRCCOLORKEY) != 0;
    u32 ckey_low = src->colorkey_src_low;
    u32 ckey_high = src->colorkey_src_high;

    if (dest->bpp == 16 && src->bpp == 16) {
        for (int y = 0; y < h; y++) {
            const u16 *srow = (const u16 *)(MEM + src->pixels + (sy + y) * src->pitch) + sx;
            u16 *drow = (u16 *)(MEM + dest->pixels + (dy + y) * dest->pitch) + dx;
            if (use_ckey) {
                for (int x = 0; x < w; x++) {
                    u16 p = srow[x];
                    if (p < ckey_low || p > ckey_high) drow[x] = p;
                }
            } else {
                memcpy(drow, srow, w * sizeof(u16));
            }
        }
    } else if (dest->bpp == 8 && src->bpp == 8) {
        for (int y = 0; y < h; y++) {
            const u8 *srow = (const u8 *)(MEM + src->pixels + (sy + y) * src->pitch) + sx;
            u8 *drow = (u8 *)(MEM + dest->pixels + (dy + y) * dest->pitch) + dx;
            if (use_ckey) {
                for (int x = 0; x < w; x++) {
                    u8 p = srow[x];
                    if (p < ckey_low || p > ckey_high) drow[x] = p;
                }
            } else {
                memcpy(drow, srow, w);
            }
        }
    } else if (dest->bpp == 16 && src->bpp == 8) {
        HostPalette *pal = src->palette_obj ? (HostPalette *)com_host(src->palette_obj) : NULL;
        for (int y = 0; y < h; y++) {
            const u8 *srow = (const u8 *)(MEM + src->pixels + (sy + y) * src->pitch) + sx;
            u16 *drow = (u16 *)(MEM + dest->pixels + (dy + y) * dest->pitch) + dx;
            for (int x = 0; x < w; x++) {
                u8 idx = srow[x];
                if (!use_ckey || (idx < ckey_low || idx > ckey_high)) {
                    u8 r = pal ? pal->entries[idx].r : idx;
                    u8 g = pal ? pal->entries[idx].g : idx;
                    u8 b = pal ? pal->entries[idx].b : idx;
                    drow[x] = (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                }
            }
        }
    }

    if (dest->is_primary) present_surface(dest);
    return DD_OK;
}

/* 8 */
static u32 imp_dds_DeleteAttachedSurface(Cpu *c)
{
    plat_log("warn: IDirectDrawSurface::DeleteAttachedSurface");
    return DD_OK;
}

/* 9 */
static u32 imp_dds_EnumAttachedSurfaces(Cpu *c)
{
    HostSurface *surf = (HostSurface *)com_host(ARG(0));
    u32 lpContext = ARG(1);
    u32 lpCallback = ARG(2);

    if (surf && surf->attached_back && lpCallback) {
        u32 args[2] = { surf->attached_back, lpContext };
        guest_call(c, lpCallback, 2, args);
    }
    return DD_OK;
}

/* 10 */
static u32 imp_dds_EnumOverlayZOrders(Cpu *c)
{
    plat_log("warn: IDirectDrawSurface::EnumOverlayZOrders");
    return DD_OK;
}

/* 11 */
static u32 imp_dds_Flip(Cpu *c)
{
    HostSurface *surf = (HostSurface *)com_host(ARG(0));
    u32 lpOverride = ARG(1);
    u32 dwFlags = ARG(2);
    (void)lpOverride; (void)dwFlags;

    if (!surf || !surf->is_primary || !surf->attached_back) return DD_OK;

    HostSurface *back = (HostSurface *)com_host(surf->attached_back);
    if (!back) return DD_OK;

    /* Swap surface pixel memory */
    u32 tmp = surf->pixels;
    surf->pixels = back->pixels;
    back->pixels = tmp;

    /* Present new front buffer */
    present_surface(surf);
    return DD_OK;
}

/* 12 */
static u32 imp_dds_GetAttachedSurface(Cpu *c)
{
    HostSurface *surf = (HostSurface *)com_host(ARG(0));
    u32 lpDDSCaps = ARG(1);
    u32 lplpAttached = ARG(2);

    if (!surf || !lplpAttached) return DDERR_INVALIDPARAMS;
    (void)lpDDSCaps;

    if (surf->attached_back) {
        wr32(lplpAttached, surf->attached_back);
        HostSurface *back = (HostSurface *)com_host(surf->attached_back);
        if (back) back->refcount++;
        return DD_OK;
    }
    return DDERR_NOTFOUND;
}

/* 13 */
static u32 imp_dds_GetBltStatus(Cpu *c)
{
    return DD_OK;
}

/* 14 */
static u32 imp_dds_GetCaps(Cpu *c)
{
    HostSurface *surf = (HostSurface *)com_host(ARG(0));
    u32 lpCaps = ARG(1);
    if (lpCaps && surf) wr32(lpCaps, surf->caps);
    return DD_OK;
}

/* 15 */
static u32 imp_dds_GetClipper(Cpu *c)
{
    HostSurface *surf = (HostSurface *)com_host(ARG(0));
    u32 lplpClipper = ARG(1);
    if (!surf || !lplpClipper) return DDERR_INVALIDPARAMS;
    if (!surf->clipper_obj) return DDERR_NOCLIPPERATTACHED;
    wr32(lplpClipper, surf->clipper_obj);
    return DD_OK;
}

/* 16 */
static u32 imp_dds_GetColorKey(Cpu *c)
{
    HostSurface *surf = (HostSurface *)com_host(ARG(0));
    u32 dwFlags = ARG(1);
    u32 lpCKey = ARG(2);
    (void)dwFlags;
    if (!surf || !lpCKey) return DDERR_INVALIDPARAMS;
    wr32(lpCKey + 0, surf->colorkey_src_low);
    wr32(lpCKey + 4, surf->colorkey_src_high);
    return DD_OK;
}

/* 17 */
static u32 imp_dds_GetDC(Cpu *c)
{
    HostSurface *surf = (HostSurface *)com_host(ARG(0));
    u32 lphDC = ARG(1);
    if (!lphDC || !surf) return DDERR_INVALIDPARAMS;
    u32 hdc = handle_new(H_OTHER, surf);
    wr32(lphDC, hdc);
    return DD_OK;
}

/* 18 */
static u32 imp_dds_GetFlipStatus(Cpu *c)
{
    return DD_OK;
}

/* 19 */
static u32 imp_dds_GetOverlayPosition(Cpu *c)
{
    plat_log("warn: IDirectDrawSurface::GetOverlayPosition");
    return DD_OK;
}

/* 20 */
static u32 imp_dds_GetPalette(Cpu *c)
{
    HostSurface *surf = (HostSurface *)com_host(ARG(0));
    u32 lplpPal = ARG(1);
    if (!surf || !lplpPal) return DDERR_INVALIDPARAMS;
    if (!surf->palette_obj) return DDERR_NOPALETTEATTACHED;
    wr32(lplpPal, surf->palette_obj);
    return DD_OK;
}

/* 21 */
static u32 imp_dds_GetPixelFormat(Cpu *c)
{
    HostSurface *surf = (HostSurface *)com_host(ARG(0));
    u32 lpFormat = ARG(1);
    if (!surf || !lpFormat) return DDERR_INVALIDPARAMS;

    wr32(lpFormat + 0x00, 32);
    if (surf->bpp == 16) {
        wr32(lpFormat + 0x04, DDPF_RGB);
        wr32(lpFormat + 0x08, 0);
        wr32(lpFormat + 0x0C, 16);
        wr32(lpFormat + 0x10, surf->rmask ? surf->rmask : 0xF800u);
        wr32(lpFormat + 0x14, surf->gmask ? surf->gmask : 0x07E0u);
        wr32(lpFormat + 0x18, surf->bmask ? surf->bmask : 0x001Fu);
        wr32(lpFormat + 0x1C, 0);
    } else {
        wr32(lpFormat + 0x04, DDPF_RGB | DDPF_PALETTEINDEXED8);
        wr32(lpFormat + 0x08, 0);
        wr32(lpFormat + 0x0C, 8);
        wr32(lpFormat + 0x10, 0);
        wr32(lpFormat + 0x14, 0);
        wr32(lpFormat + 0x18, 0);
        wr32(lpFormat + 0x1C, 0);
    }
    return DD_OK;
}

/* 22 */
static u32 imp_dds_GetSurfaceDesc(Cpu *c)
{
    HostSurface *surf = (HostSurface *)com_host(ARG(0));
    u32 lpDDSD = ARG(1);
    if (!surf || !lpDDSD) return DDERR_INVALIDPARAMS;
    fill_surface_desc(surf, lpDDSD);
    return DD_OK;
}

/* 23 */
static u32 imp_dds_Initialize(Cpu *c)
{
    plat_log("warn: IDirectDrawSurface::Initialize");
    return DD_OK;
}

/* 24 */
static u32 imp_dds_IsLost(Cpu *c)
{
    return DD_OK;
}

/* 25 */
static u32 imp_dds_Lock(Cpu *c)
{
    HostSurface *surf = (HostSurface *)com_host(ARG(0));
    u32 lpDestRect = ARG(1);
    u32 lpDDSD = ARG(2);
    u32 dwFlags = ARG(3);
    u32 hEvent = ARG(4);
    (void)dwFlags; (void)hEvent;

    if (!surf || !lpDDSD) return DDERR_INVALIDPARAMS;

    fill_surface_desc(surf, lpDDSD);

    u32 surf_ptr = surf->pixels;
    if (lpDestRect) {
        int left = (int)rd32(lpDestRect + 0);
        int top = (int)rd32(lpDestRect + 4);
        int bpp_bytes = (int)((surf->bpp + 7) / 8);
        surf_ptr += (u32)(top * (int)surf->pitch + left * bpp_bytes);
    }
    wr32(lpDDSD + 0x24, surf_ptr); /* lpSurface */
    return DD_OK;
}

/* 26 */
static u32 imp_dds_ReleaseDC(Cpu *c)
{
    HostSurface *surf = (HostSurface *)com_host(ARG(0));
    u32 hDC = ARG(1);
    handle_free(hDC);
    if (surf && surf->is_primary) present_surface(surf);
    return DD_OK;
}

/* 27 */
static u32 imp_dds_Restore(Cpu *c)
{
    return DD_OK;
}

/* 28 */
static u32 imp_dds_SetClipper(Cpu *c)
{
    HostSurface *surf = (HostSurface *)com_host(ARG(0));
    u32 lpClipper = ARG(1);
    if (surf) surf->clipper_obj = lpClipper;
    return DD_OK;
}

/* 29 */
static u32 imp_dds_SetColorKey(Cpu *c)
{
    HostSurface *surf = (HostSurface *)com_host(ARG(0));
    u32 dwFlags = ARG(1);
    u32 lpCKey = ARG(2);

    if (!surf) return DDERR_INVALIDPARAMS;
    if (dwFlags & DDCKEY_SRCBLT) {
        if (lpCKey) {
            surf->colorkey_src_low = rd32(lpCKey + 0);
            surf->colorkey_src_high = rd32(lpCKey + 4);
            surf->has_colorkey_src = 1;
        } else {
            surf->has_colorkey_src = 0;
        }
    }
    return DD_OK;
}

/* 30 */
static u32 imp_dds_SetOverlayPosition(Cpu *c)
{
    plat_log("warn: IDirectDrawSurface::SetOverlayPosition");
    return DD_OK;
}

/* 31 */
static u32 imp_dds_SetPalette(Cpu *c)
{
    HostSurface *surf = (HostSurface *)com_host(ARG(0));
    u32 lpPalette = ARG(1);
    if (surf) {
        surf->palette_obj = lpPalette;
        if (surf->is_primary) present_surface(surf);
    }
    return DD_OK;
}

/* 32 */
static u32 imp_dds_Unlock(Cpu *c)
{
    HostSurface *surf = (HostSurface *)com_host(ARG(0));
    u32 lpSurfaceData = ARG(1);
    (void)lpSurfaceData;
    if (surf && surf->is_primary) present_surface(surf);
    return DD_OK;
}

/* 33 */
static u32 imp_dds_UpdateOverlay(Cpu *c)
{
    plat_log("warn: IDirectDrawSurface::UpdateOverlay");
    return DD_OK;
}

/* 34 */
static u32 imp_dds_UpdateOverlayDisplay(Cpu *c)
{
    plat_log("warn: IDirectDrawSurface::UpdateOverlayDisplay");
    return DD_OK;
}

/* 35 */
static u32 imp_dds_UpdateOverlayZOrder(Cpu *c)
{
    plat_log("warn: IDirectDrawSurface::UpdateOverlayZOrder");
    return DD_OK;
}

/* Added in IDirectDrawSurface2 */
/* 36 */
static u32 imp_dds2_GetDDInterface(Cpu *c)
{
    HostSurface *surf = (HostSurface *)com_host(ARG(0));
    u32 lplpDD = ARG(1);
    if (!surf || !lplpDD) return DDERR_INVALIDPARAMS;
    wr32(lplpDD, surf->ddraw ? surf->ddraw->ddraw1_obj : 0);
    return DD_OK;
}

/* 37 */
static u32 imp_dds2_PageLock(Cpu *c)
{
    return DD_OK;
}

/* 38 */
static u32 imp_dds2_PageUnlock(Cpu *c)
{
    return DD_OK;
}

/* Added in IDirectDrawSurface3 */
/* 39 */
static u32 imp_dds3_SetSurfaceDesc(Cpu *c)
{
    HostSurface *surf = (HostSurface *)com_host(ARG(0));
    u32 lpDDSD = ARG(1);
    u32 dwFlags = ARG(2);
    (void)dwFlags;
    if (!surf || !lpDDSD) return DDERR_INVALIDPARAMS;
    u32 flags = rd32(lpDDSD + 0x04);
    if (flags & DDSD_WIDTH) surf->width = rd32(lpDDSD + 0x0C);
    if (flags & DDSD_HEIGHT) surf->height = rd32(lpDDSD + 0x08);
    if (flags & DDSD_PITCH) surf->pitch = rd32(lpDDSD + 0x10);
    return DD_OK;
}

static const struct ImpDef methods_surface[] = {
    /* 0 */ IMPORT(dds_QueryInterface, 3),
    /* 1 */ IMPORT(dds_AddRef, 1),
    /* 2 */ IMPORT(dds_Release, 1),
    /* 3 */ IMPORT(dds_AddAttachedSurface, 2),
    /* 4 */ IMPORT(dds_AddOverlayDirtyRect, 2),
    /* 5 */ IMPORT(dds_Blt, 6),
    /* 6 */ IMPORT(dds_BltBatch, 4),
    /* 7 */ IMPORT(dds_BltFast, 6),
    /* 8 */ IMPORT(dds_DeleteAttachedSurface, 3),
    /* 9 */ IMPORT(dds_EnumAttachedSurfaces, 3),
    /* 10 */ IMPORT(dds_EnumOverlayZOrders, 4),
    /* 11 */ IMPORT(dds_Flip, 3),
    /* 12 */ IMPORT(dds_GetAttachedSurface, 3),
    /* 13 */ IMPORT(dds_GetBltStatus, 2),
    /* 14 */ IMPORT(dds_GetCaps, 2),
    /* 15 */ IMPORT(dds_GetClipper, 2),
    /* 16 */ IMPORT(dds_GetColorKey, 3),
    /* 17 */ IMPORT(dds_GetDC, 2),
    /* 18 */ IMPORT(dds_GetFlipStatus, 2),
    /* 19 */ IMPORT(dds_GetOverlayPosition, 3),
    /* 20 */ IMPORT(dds_GetPalette, 2),
    /* 21 */ IMPORT(dds_GetPixelFormat, 2),
    /* 22 */ IMPORT(dds_GetSurfaceDesc, 2),
    /* 23 */ IMPORT(dds_Initialize, 3),
    /* 24 */ IMPORT(dds_IsLost, 1),
    /* 25 */ IMPORT(dds_Lock, 5),
    /* 26 */ IMPORT(dds_ReleaseDC, 2),
    /* 27 */ IMPORT(dds_Restore, 1),
    /* 28 */ IMPORT(dds_SetClipper, 2),
    /* 29 */ IMPORT(dds_SetColorKey, 3),
    /* 30 */ IMPORT(dds_SetOverlayPosition, 3),
    /* 31 */ IMPORT(dds_SetPalette, 2),
    /* 32 */ IMPORT(dds_Unlock, 2),
    /* 33 */ IMPORT(dds_UpdateOverlay, 6),
    /* 34 */ IMPORT(dds_UpdateOverlayDisplay, 2),
    /* 35 */ IMPORT(dds_UpdateOverlayZOrder, 3),
};

static const struct ImpDef methods_surface2[] = {
    /* 0 */ IMPORT(dds_QueryInterface, 3),
    /* 1 */ IMPORT(dds_AddRef, 1),
    /* 2 */ IMPORT(dds_Release, 1),
    /* 3 */ IMPORT(dds_AddAttachedSurface, 2),
    /* 4 */ IMPORT(dds_AddOverlayDirtyRect, 2),
    /* 5 */ IMPORT(dds_Blt, 6),
    /* 6 */ IMPORT(dds_BltBatch, 4),
    /* 7 */ IMPORT(dds_BltFast, 6),
    /* 8 */ IMPORT(dds_DeleteAttachedSurface, 3),
    /* 9 */ IMPORT(dds_EnumAttachedSurfaces, 3),
    /* 10 */ IMPORT(dds_EnumOverlayZOrders, 4),
    /* 11 */ IMPORT(dds_Flip, 3),
    /* 12 */ IMPORT(dds_GetAttachedSurface, 3),
    /* 13 */ IMPORT(dds_GetBltStatus, 2),
    /* 14 */ IMPORT(dds_GetCaps, 2),
    /* 15 */ IMPORT(dds_GetClipper, 2),
    /* 16 */ IMPORT(dds_GetColorKey, 3),
    /* 17 */ IMPORT(dds_GetDC, 2),
    /* 18 */ IMPORT(dds_GetFlipStatus, 2),
    /* 19 */ IMPORT(dds_GetOverlayPosition, 3),
    /* 20 */ IMPORT(dds_GetPalette, 2),
    /* 21 */ IMPORT(dds_GetPixelFormat, 2),
    /* 22 */ IMPORT(dds_GetSurfaceDesc, 2),
    /* 23 */ IMPORT(dds_Initialize, 3),
    /* 24 */ IMPORT(dds_IsLost, 1),
    /* 25 */ IMPORT(dds_Lock, 5),
    /* 26 */ IMPORT(dds_ReleaseDC, 2),
    /* 27 */ IMPORT(dds_Restore, 1),
    /* 28 */ IMPORT(dds_SetClipper, 2),
    /* 29 */ IMPORT(dds_SetColorKey, 3),
    /* 30 */ IMPORT(dds_SetOverlayPosition, 3),
    /* 31 */ IMPORT(dds_SetPalette, 2),
    /* 32 */ IMPORT(dds_Unlock, 2),
    /* 33 */ IMPORT(dds_UpdateOverlay, 6),
    /* 34 */ IMPORT(dds_UpdateOverlayDisplay, 2),
    /* 35 */ IMPORT(dds_UpdateOverlayZOrder, 3),
    /* 36 */ IMPORT(dds2_GetDDInterface, 2),
    /* 37 */ IMPORT(dds2_PageLock, 2),
    /* 38 */ IMPORT(dds2_PageUnlock, 2),
};

static const struct ImpDef methods_surface3[] = {
    /* 0 */ IMPORT(dds_QueryInterface, 3),
    /* 1 */ IMPORT(dds_AddRef, 1),
    /* 2 */ IMPORT(dds_Release, 1),
    /* 3 */ IMPORT(dds_AddAttachedSurface, 2),
    /* 4 */ IMPORT(dds_AddOverlayDirtyRect, 2),
    /* 5 */ IMPORT(dds_Blt, 6),
    /* 6 */ IMPORT(dds_BltBatch, 4),
    /* 7 */ IMPORT(dds_BltFast, 6),
    /* 8 */ IMPORT(dds_DeleteAttachedSurface, 3),
    /* 9 */ IMPORT(dds_EnumAttachedSurfaces, 3),
    /* 10 */ IMPORT(dds_EnumOverlayZOrders, 4),
    /* 11 */ IMPORT(dds_Flip, 3),
    /* 12 */ IMPORT(dds_GetAttachedSurface, 3),
    /* 13 */ IMPORT(dds_GetBltStatus, 2),
    /* 14 */ IMPORT(dds_GetCaps, 2),
    /* 15 */ IMPORT(dds_GetClipper, 2),
    /* 16 */ IMPORT(dds_GetColorKey, 3),
    /* 17 */ IMPORT(dds_GetDC, 2),
    /* 18 */ IMPORT(dds_GetFlipStatus, 2),
    /* 19 */ IMPORT(dds_GetOverlayPosition, 3),
    /* 20 */ IMPORT(dds_GetPalette, 2),
    /* 21 */ IMPORT(dds_GetPixelFormat, 2),
    /* 22 */ IMPORT(dds_GetSurfaceDesc, 2),
    /* 23 */ IMPORT(dds_Initialize, 3),
    /* 24 */ IMPORT(dds_IsLost, 1),
    /* 25 */ IMPORT(dds_Lock, 5),
    /* 26 */ IMPORT(dds_ReleaseDC, 2),
    /* 27 */ IMPORT(dds_Restore, 1),
    /* 28 */ IMPORT(dds_SetClipper, 2),
    /* 29 */ IMPORT(dds_SetColorKey, 3),
    /* 30 */ IMPORT(dds_SetOverlayPosition, 3),
    /* 31 */ IMPORT(dds_SetPalette, 2),
    /* 32 */ IMPORT(dds_Unlock, 2),
    /* 33 */ IMPORT(dds_UpdateOverlay, 6),
    /* 34 */ IMPORT(dds_UpdateOverlayDisplay, 2),
    /* 35 */ IMPORT(dds_UpdateOverlayZOrder, 3),
    /* 36 */ IMPORT(dds2_GetDDInterface, 2),
    /* 37 */ IMPORT(dds2_PageLock, 2),
    /* 38 */ IMPORT(dds2_PageUnlock, 2),
    /* 39 */ IMPORT(dds3_SetSurfaceDesc, 3),
};

/* ---- IDirectDraw Methods ------------------------------------------------------------- */

/* 0 */
static u32 imp_dd_QueryInterface(Cpu *c)
{
    HostDDraw *dd = (HostDDraw *)com_host(ARG(0));
    u32 riid = ARG(1);
    u32 ppv = ARG(2);
    if (!ppv || !dd) return DDERR_INVALIDPARAMS;

    if (match_guid(riid, 0x00000000u, 0x0000, 0x0000, guid_d4_unknown) ||
        match_guid(riid, 0xD7B70EE0u, 0x4340, 0x11CF, guid_d4_ddraw1)) {
        if (!dd->ddraw1_obj) dd->ddraw1_obj = com_new(methods_ddraw, 23, dd);
        wr32(ppv, dd->ddraw1_obj);
        return DD_OK;
    }
    if (match_guid(riid, 0xB3A6F3E0u, 0x2B43, 0x11CF, guid_d4_ddraw2)) {
        if (!dd->ddraw2_obj) dd->ddraw2_obj = com_new(methods_ddraw2, 24, dd);
        wr32(ppv, dd->ddraw2_obj);
        return DD_OK;
    }
    if (match_guid(riid, 0x3BBA0080u, 0x2421, 0x11CF, guid_d4_d3d)) {
        /* No 3D device supported: software renderer */
        plat_log("QueryInterface(IID_IDirect3D) -> E_NOINTERFACE");
        return E_NOINTERFACE;
    }

    log_unknown_iid("IDirectDraw::QueryInterface", riid);
    return E_NOINTERFACE;
}

/* 1 */
static u32 imp_dd_AddRef(Cpu *c)
{
    HostDDraw *dd = (HostDDraw *)com_host(ARG(0));
    return dd ? ++dd->refcount : 1;
}

/* 2 */
static u32 imp_dd_Release(Cpu *c)
{
    HostDDraw *dd = (HostDDraw *)com_host(ARG(0));
    if (!dd) return 0;
    if (dd->refcount > 0) dd->refcount--;
    return dd->refcount;
}

/* 3 */
static u32 imp_dd_Compact(Cpu *c)
{
    return DD_OK;
}

/* 4 */
static u32 imp_dd_CreateClipper(Cpu *c)
{
    u32 dwFlags = ARG(1);
    u32 lplpClipper = ARG(2);
    u32 pUnkOuter = ARG(3);
    (void)dwFlags; (void)pUnkOuter;
    if (!lplpClipper) return DDERR_INVALIDPARAMS;

    HostClipper *clip = (HostClipper *)calloc(1, sizeof(HostClipper));
    clip->refcount = 1;
    u32 clip_obj = com_new(methods_clipper, 9, clip);
    wr32(lplpClipper, clip_obj);
    return DD_OK;
}

/* 5 */
static u32 imp_dd_CreatePalette(Cpu *c)
{
    u32 dwFlags = ARG(1);
    u32 lpColorTable = ARG(2);
    u32 lplpPalette = ARG(3);
    u32 pUnkOuter = ARG(4);
    (void)pUnkOuter;
    if (!lplpPalette) return DDERR_INVALIDPARAMS;

    HostPalette *pal = (HostPalette *)calloc(1, sizeof(HostPalette));
    pal->refcount = 1;
    pal->flags = dwFlags;

    if (lpColorTable) {
        for (int i = 0; i < 256; i++) {
            pal->entries[i].r = rd8(lpColorTable + 4 * i + 0);
            pal->entries[i].g = rd8(lpColorTable + 4 * i + 1);
            pal->entries[i].b = rd8(lpColorTable + 4 * i + 2);
            pal->entries[i].flags = rd8(lpColorTable + 4 * i + 3);
        }
    }

    u32 pal_obj = com_new(methods_palette, 7, pal);
    wr32(lplpPalette, pal_obj);
    return DD_OK;
}

/* 6 */
static u32 imp_dd_CreateSurface(Cpu *c)
{
    HostDDraw *dd = (HostDDraw *)com_host(ARG(0));
    u32 lpDDSD = ARG(1);
    u32 lplpSurface = ARG(2);
    u32 pUnkOuter = ARG(3);
    (void)pUnkOuter;

    if (!dd || !lpDDSD || !lplpSurface) return DDERR_INVALIDPARAMS;

    u32 dwFlags = rd32(lpDDSD + 0x04);
    u32 dwCaps = rd32(lpDDSD + 0x68);
    u32 bb_count = (dwFlags & DDSD_BACKBUFFERCOUNT) ? rd32(lpDDSD + 0x14) : 0;
    u32 width = (dwFlags & DDSD_WIDTH) ? rd32(lpDDSD + 0x0C) : dd->width;
    u32 height = (dwFlags & DDSD_HEIGHT) ? rd32(lpDDSD + 0x08) : dd->height;

    u32 bpp = dd->bpp;
    u32 rmask = 0xF800u, gmask = 0x07E0u, bmask = 0x001Fu;

    if (dwFlags & DDSD_PIXELFORMAT) {
        bpp = rd32(lpDDSD + 0x48 + 0x0C);
        rmask = rd32(lpDDSD + 0x48 + 0x10);
        gmask = rd32(lpDDSD + 0x48 + 0x14);
        bmask = rd32(lpDDSD + 0x48 + 0x18);
    }

    if (dwCaps & DDSCAPS_PRIMARYSURFACE) {
        width = dd->width;
        height = dd->height;
        bpp = dd->bpp;
    }

    u32 bytes_per_pixel = (bpp + 7) / 8;
    u32 pitch = (width * bytes_per_pixel + 7) & ~7u;

    HostSurface *surf = (HostSurface *)calloc(1, sizeof(HostSurface));
    surf->refcount = 1;
    surf->ddraw = dd;
    surf->width = width;
    surf->height = height;
    surf->bpp = bpp;
    surf->pitch = pitch;
    surf->caps = dwCaps;
    surf->rmask = rmask;
    surf->gmask = gmask;
    surf->bmask = bmask;
    surf->pixels = galloc(pitch * height);

    u32 surf_obj = com_new(methods_surface, 36, surf);
    surf->surf1_obj = surf_obj;

    if (dwCaps & DDSCAPS_PRIMARYSURFACE) {
        surf->is_primary = 1;
        dd->primary_surf = surf_obj;

        if ((dwCaps & DDSCAPS_FLIP) && bb_count >= 1) {
            HostSurface *back = (HostSurface *)calloc(1, sizeof(HostSurface));
            back->refcount = 1;
            back->ddraw = dd;
            back->width = width;
            back->height = height;
            back->bpp = bpp;
            back->pitch = pitch;
            back->caps = DDSCAPS_BACKBUFFER | (dwCaps & ~DDSCAPS_PRIMARYSURFACE);
            back->rmask = rmask;
            back->gmask = gmask;
            back->bmask = bmask;
            back->pixels = galloc(pitch * height);
            back->is_backbuffer = 1;

            u32 back_obj = com_new(methods_surface, 36, back);
            back->surf1_obj = back_obj;

            surf->attached_back = back_obj;
            back->attached_front = surf_obj;
        }
    }

    fill_surface_desc(surf, lpDDSD);
    wr32(lplpSurface, surf_obj);
    return DD_OK;
}

/* 7 */
static u32 imp_dd_DuplicateSurface(Cpu *c)
{
    plat_log("warn: IDirectDraw::DuplicateSurface");
    return E_NOTIMPL;
}

/* 8 */
struct ModeDef { u32 w, h, bpp; };
static const struct ModeDef display_modes[] = {
    {320, 240, 16},
    {640, 480, 16},
    {800, 600, 16},
    {1024, 768, 16},
    {320, 240, 8},
    {640, 480, 8},
    {800, 600, 8},
    {1024, 768, 8},
};

static u32 imp_dd_EnumDisplayModes(Cpu *c)
{
    u32 dwFlags = ARG(1);
    u32 lpDDSD = ARG(2);
    u32 lpContext = ARG(3);
    u32 lpCallback = ARG(4);
    (void)dwFlags;

    if (!lpCallback) return DDERR_INVALIDPARAMS;

    u32 filter_bpp = 0;
    if (lpDDSD && (rd32(lpDDSD + 0x04) & DDSD_PIXELFORMAT)) {
        filter_bpp = rd32(lpDDSD + 0x48 + 0x0C);
    }

    static u32 enum_desc = 0;
    if (!enum_desc) enum_desc = galloc(108);

    for (size_t i = 0; i < sizeof(display_modes) / sizeof(display_modes[0]); i++) {
        const struct ModeDef *m = &display_modes[i];
        if (filter_bpp && m->bpp != filter_bpp) continue;

        memset(G(enum_desc), 0, 108);
        wr32(enum_desc + 0x00, 108);
        wr32(enum_desc + 0x04, DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE);
        wr32(enum_desc + 0x08, m->h);
        wr32(enum_desc + 0x0C, m->w);
        u32 bytes = (m->bpp + 7) / 8;
        wr32(enum_desc + 0x10, (m->w * bytes + 7) & ~7u);
        wr32(enum_desc + 0x18, 60);

        wr32(enum_desc + 0x48 + 0x00, 32);
        if (m->bpp == 16) {
            wr32(enum_desc + 0x48 + 0x04, DDPF_RGB);
            wr32(enum_desc + 0x48 + 0x0C, 16);
            wr32(enum_desc + 0x48 + 0x10, 0xF800u);
            wr32(enum_desc + 0x48 + 0x14, 0x07E0u);
            wr32(enum_desc + 0x48 + 0x18, 0x001Fu);
        } else {
            wr32(enum_desc + 0x48 + 0x04, DDPF_RGB | DDPF_PALETTEINDEXED8);
            wr32(enum_desc + 0x48 + 0x0C, 8);
        }
        wr32(enum_desc + 0x68, DDSCAPS_PRIMARYSURFACE | DDSCAPS_VIDEOMEMORY);

        u32 args[2] = { enum_desc, lpContext };
        u32 res = guest_call(c, lpCallback, 2, args);
        if (res == 0) break; /* DDENUMRET_CANCEL */
    }
    return DD_OK;
}

/* 9 */
static u32 imp_dd_EnumSurfaces(Cpu *c)
{
    plat_log("warn: IDirectDraw::EnumSurfaces");
    return DD_OK;
}

/* 10 */
static u32 imp_dd_FlipToGDISurface(Cpu *c)
{
    return DD_OK;
}

/* 11 */
static u32 imp_dd_GetCaps(Cpu *c)
{
    u32 lpDriverCaps = ARG(1);
    u32 lpHELCaps = ARG(2);

    /* HEL-only device with no 3D hardware: dwCaps without DDCAPS_3D */
    if (lpDriverCaps) {
        u32 sz = rd32(lpDriverCaps + 0x00);
        if (sz >= 4) {
            if (sz > 512) sz = 512;
            memset(G(lpDriverCaps + 4), 0, sz - 4);
            /* HEL only: driver caps have no 3D */
            wr32(lpDriverCaps + 0x04, DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_COLORKEY);
            if (sz >= 0x40) wr32(lpDriverCaps + 0x3C, 64 * 1024 * 1024);
            if (sz >= 0x44) wr32(lpDriverCaps + 0x40, 64 * 1024 * 1024);
            if (sz >= 0x88) wr32(lpDriverCaps + 0x84, DDSCAPS_PRIMARYSURFACE | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY);
        }
    }

    if (lpHELCaps) {
        u32 sz = rd32(lpHELCaps + 0x00);
        if (sz >= 4) {
            if (sz > 512) sz = 512;
            memset(G(lpHELCaps + 4), 0, sz - 4);
            u32 caps = DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_COLORKEY | DDCAPS_CANBLTSYS | DDCAPS_CANCLIP;
            wr32(lpHELCaps + 0x04, caps);
            wr32(lpHELCaps + 0x0C, DDCKEY_SRCBLT | DDCKEY_DESTBLT);
            wr32(lpHELCaps + 0x10, 0x00000400u); /* DDFXCAPS_BLTCOLORFILL */
            if (sz >= 0x40) wr32(lpHELCaps + 0x3C, 64 * 1024 * 1024);
            if (sz >= 0x44) wr32(lpHELCaps + 0x40, 64 * 1024 * 1024);
            if (sz >= 0x88) wr32(lpHELCaps + 0x84, DDSCAPS_PRIMARYSURFACE | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY);
        }
    }
    return DD_OK;
}

/* 12 */
static u32 imp_dd_GetDisplayMode(Cpu *c)
{
    HostDDraw *dd = (HostDDraw *)com_host(ARG(0));
    u32 lpDDSD = ARG(1);
    if (!dd || !lpDDSD) return DDERR_INVALIDPARAMS;

    memset(G(lpDDSD), 0, 108);
    wr32(lpDDSD + 0x00, 108);
    wr32(lpDDSD + 0x04, DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE);
    wr32(lpDDSD + 0x08, dd->height);
    wr32(lpDDSD + 0x0C, dd->width);
    u32 bytes = (dd->bpp + 7) / 8;
    wr32(lpDDSD + 0x10, (dd->width * bytes + 7) & ~7u);
    wr32(lpDDSD + 0x18, 60);

    wr32(lpDDSD + 0x48 + 0x00, 32);
    if (dd->bpp == 16) {
        wr32(lpDDSD + 0x48 + 0x04, DDPF_RGB);
        wr32(lpDDSD + 0x48 + 0x0C, 16);
        wr32(lpDDSD + 0x48 + 0x10, 0xF800u);
        wr32(lpDDSD + 0x48 + 0x14, 0x07E0u);
        wr32(lpDDSD + 0x48 + 0x18, 0x001Fu);
    } else {
        wr32(lpDDSD + 0x48 + 0x04, DDPF_RGB | DDPF_PALETTEINDEXED8);
        wr32(lpDDSD + 0x48 + 0x0C, 8);
    }
    wr32(lpDDSD + 0x68, DDSCAPS_PRIMARYSURFACE | DDSCAPS_VIDEOMEMORY);
    return DD_OK;
}

/* 13 */
static u32 imp_dd_GetFourCCCodes(Cpu *c)
{
    u32 lpNum = ARG(1);
    if (lpNum) wr32(lpNum, 0);
    return DD_OK;
}

/* 14 */
static u32 imp_dd_GetGDISurface(Cpu *c)
{
    HostDDraw *dd = (HostDDraw *)com_host(ARG(0));
    u32 lplpSurf = ARG(1);
    if (!dd || !lplpSurf) return DDERR_INVALIDPARAMS;
    if (!dd->primary_surf) return DDERR_NOTFOUND;
    wr32(lplpSurf, dd->primary_surf);
    return DD_OK;
}

/* 15 */
static u32 imp_dd_GetMonitorFrequency(Cpu *c)
{
    u32 lpFreq = ARG(1);
    if (lpFreq) wr32(lpFreq, 60);
    return DD_OK;
}

/* 16 */
static u32 imp_dd_GetScanLine(Cpu *c)
{
    u32 lpLine = ARG(1);
    if (lpLine) wr32(lpLine, 0);
    return DD_OK;
}

/* 17 */
static u32 imp_dd_GetVerticalBlankStatus(Cpu *c)
{
    u32 lpb = ARG(1);
    if (lpb) wr32(lpb, 1);
    return DD_OK;
}

/* 18 */
static u32 imp_dd_Initialize(Cpu *c)
{
    plat_log("warn: IDirectDraw::Initialize");
    return DD_OK;
}

/* 19 */
static u32 imp_dd_RestoreDisplayMode(Cpu *c)
{
    return DD_OK;
}

/* 20 */
static u32 imp_dd_SetCooperativeLevel(Cpu *c)
{
    HostDDraw *dd = (HostDDraw *)com_host(ARG(0));
    u32 hWnd = ARG(1);
    u32 dwFlags = ARG(2);
    if (dd) {
        dd->hwnd = hWnd;
        dd->coop_flags = dwFlags;
    }
    return DD_OK;
}

/* 21 */
static u32 imp_dd_SetDisplayMode(Cpu *c)
{
    HostDDraw *dd = (HostDDraw *)com_host(ARG(0));
    u32 width = ARG(1);
    u32 height = ARG(2);
    u32 bpp = ARG(3);

    if (dd) {
        if (width) dd->width = width;
        if (height) dd->height = height;
        if (bpp) dd->bpp = bpp;
    }
    return DD_OK;
}

/* 22 */
static u32 imp_dd_WaitForVerticalBlank(Cpu *c)
{
    return DD_OK;
}

static const struct ImpDef methods_ddraw[] = {
    /* 0 */ IMPORT(dd_QueryInterface, 3),
    /* 1 */ IMPORT(dd_AddRef, 1),
    /* 2 */ IMPORT(dd_Release, 1),
    /* 3 */ IMPORT(dd_Compact, 1),
    /* 4 */ IMPORT(dd_CreateClipper, 4),
    /* 5 */ IMPORT(dd_CreatePalette, 5),
    /* 6 */ IMPORT(dd_CreateSurface, 4),
    /* 7 */ IMPORT(dd_DuplicateSurface, 3),
    /* 8 */ IMPORT(dd_EnumDisplayModes, 5),
    /* 9 */ IMPORT(dd_EnumSurfaces, 5),
    /* 10 */ IMPORT(dd_FlipToGDISurface, 1),
    /* 11 */ IMPORT(dd_GetCaps, 3),
    /* 12 */ IMPORT(dd_GetDisplayMode, 2),
    /* 13 */ IMPORT(dd_GetFourCCCodes, 3),
    /* 14 */ IMPORT(dd_GetGDISurface, 2),
    /* 15 */ IMPORT(dd_GetMonitorFrequency, 2),
    /* 16 */ IMPORT(dd_GetScanLine, 2),
    /* 17 */ IMPORT(dd_GetVerticalBlankStatus, 2),
    /* 18 */ IMPORT(dd_Initialize, 2),
    /* 19 */ IMPORT(dd_RestoreDisplayMode, 1),
    /* 20 */ IMPORT(dd_SetCooperativeLevel, 3),
    /* 21 */ IMPORT(dd_SetDisplayMode, 4),
    /* 22 */ IMPORT(dd_WaitForVerticalBlank, 3),
};

/* ---- IDirectDraw2 Specific Methods --------------------------------------------------- */

/* 21 for IDirectDraw2 (takes 5 params + this = 6 args) */
static u32 imp_dd2_SetDisplayMode(Cpu *c)
{
    HostDDraw *dd = (HostDDraw *)com_host(ARG(0));
    u32 width = ARG(1);
    u32 height = ARG(2);
    u32 bpp = ARG(3);
    u32 dwRefreshRate = ARG(4);
    u32 dwFlags = ARG(5);
    (void)dwRefreshRate; (void)dwFlags;

    if (dd) {
        if (width) dd->width = width;
        if (height) dd->height = height;
        if (bpp) dd->bpp = bpp;
    }
    return DD_OK;
}

/* 23 for IDirectDraw2 */
static u32 imp_dd2_GetAvailableVidMem(Cpu *c)
{
    u32 lpDDSCaps = ARG(1);
    u32 lpdwTotal = ARG(2);
    u32 lpdwFree = ARG(3);
    (void)lpDDSCaps;

    if (lpdwTotal) wr32(lpdwTotal, 64 * 1024 * 1024);
    if (lpdwFree) wr32(lpdwFree, 64 * 1024 * 1024);
    return DD_OK;
}

static const struct ImpDef methods_ddraw2[] = {
    /* 0 */ IMPORT(dd_QueryInterface, 3),
    /* 1 */ IMPORT(dd_AddRef, 1),
    /* 2 */ IMPORT(dd_Release, 1),
    /* 3 */ IMPORT(dd_Compact, 1),
    /* 4 */ IMPORT(dd_CreateClipper, 4),
    /* 5 */ IMPORT(dd_CreatePalette, 5),
    /* 6 */ IMPORT(dd_CreateSurface, 4),
    /* 7 */ IMPORT(dd_DuplicateSurface, 3),
    /* 8 */ IMPORT(dd_EnumDisplayModes, 5),
    /* 9 */ IMPORT(dd_EnumSurfaces, 5),
    /* 10 */ IMPORT(dd_FlipToGDISurface, 1),
    /* 11 */ IMPORT(dd_GetCaps, 3),
    /* 12 */ IMPORT(dd_GetDisplayMode, 2),
    /* 13 */ IMPORT(dd_GetFourCCCodes, 3),
    /* 14 */ IMPORT(dd_GetGDISurface, 2),
    /* 15 */ IMPORT(dd_GetMonitorFrequency, 2),
    /* 16 */ IMPORT(dd_GetScanLine, 2),
    /* 17 */ IMPORT(dd_GetVerticalBlankStatus, 2),
    /* 18 */ IMPORT(dd_Initialize, 2),
    /* 19 */ IMPORT(dd_RestoreDisplayMode, 1),
    /* 20 */ IMPORT(dd_SetCooperativeLevel, 3),
    /* 21 */ IMPORT(dd2_SetDisplayMode, 6),
    /* 22 */ IMPORT(dd_WaitForVerticalBlank, 3),
    /* 23 */ IMPORT(dd2_GetAvailableVidMem, 4),
};

/* ---- Exported DLL Functions ---------------------------------------------------------- */

/* HRESULT WINAPI DirectDrawEnumerateA(LPDDENUMCALLBACKA lpCallback, LPVOID lpContext) */
static u32 imp_DirectDrawEnumerateA(Cpu *c)
{
    u32 callback = ARG(0);
    u32 context = ARG(1);

    if (!callback) return DD_OK;

    static u32 desc_str = 0;
    static u32 name_str = 0;
    if (!desc_str) desc_str = gstrdup("Primary Display Driver");
    if (!name_str) name_str = gstrdup("display");

    u32 args[4];
    args[0] = 0;        /* lpGUID = NULL */
    args[1] = desc_str; /* "Primary Display Driver" */
    args[2] = name_str; /* "display" */
    args[3] = context;  /* lpContext */

    guest_call(c, callback, 4, args);
    return DD_OK;
}

/* HRESULT WINAPI DirectDrawCreate(GUID FAR *lpGUID, LPDIRECTDRAW FAR *lplpDD, IUnknown FAR *pUnkOuter) */
static u32 imp_DirectDrawCreate(Cpu *c)
{
    u32 guid_p = ARG(0);
    u32 out_p = ARG(1);
    u32 unk_outer = ARG(2);
    (void)guid_p; (void)unk_outer;

    if (!out_p) return DDERR_INVALIDPARAMS;

    HostDDraw *dd = (HostDDraw *)calloc(1, sizeof(HostDDraw));
    dd->refcount = 1;
    dd->width = 640;
    dd->height = 480;
    dd->bpp = 16;

    u32 obj = com_new(methods_ddraw, 23, dd);
    dd->ddraw1_obj = obj;

    wr32(out_p, obj);
    return DD_OK;
}

const struct ImpDef imp_ddraw[] = {
    IMPORT(DirectDrawCreate, 3),
    IMPORT(DirectDrawEnumerateA, 2),
};
IMPORT_TABLE(ddraw);
