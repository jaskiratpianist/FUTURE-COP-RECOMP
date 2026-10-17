/* GDI32, ADVAPI32, WINMM, ole32, COMCTL32 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include "host.h"
#include "win.h"
#include "sound.h"

/* ========================================================================= */
/* COMCTL32: Common Controls                                                 */
/* ========================================================================= */

/* void InitCommonControls(void) - ordinal #17 */
static u32 imp_InitCommonControls(Cpu *c)
{
    (void)c;
    return 1;
}

const struct ImpDef imp_comctl32[] = {
    {"#17", 0, imp_InitCommonControls},
};
IMPORT_TABLE(comctl32);

/* ========================================================================= */
/* GDI32: Graphics Device Interface                                          */
/* ========================================================================= */

#define GDI_MAGIC 0x47444930u

typedef struct GdiObj {
    u32 magic;
    int type; /* 1 = font, 2 = pen, 3 = brush */
    int height;
    int width;
    int weight;
    char face[64];
} GdiObj;

static u32 gdi_bk_color = 0x00000000;
static u32 gdi_bk_mode = 2; /* 1 = TRANSPARENT, 2 = OPAQUE */
static u32 gdi_text_color = 0x00FFFFFF;
static u32 cur_selected_font = 0x80000000u | 13; /* SYSTEM_FONT */
static u32 cur_selected_brush = 0x80000000u | 0; /* WHITE_BRUSH */
static u32 cur_selected_pen = 0x80000000u | 7;   /* BLACK_PEN */

/* BOOL TextOutA(HDC hdc, int x, int y, LPCSTR lpString, int c) */
static u32 imp_TextOutA(Cpu *c)
{
    (void)c;
    return 1;
}

/* COLORREF SetBkColor(HDC hdc, COLORREF color) */
static u32 imp_SetBkColor(Cpu *c)
{
    (void)c;
    u32 old = gdi_bk_color;
    gdi_bk_color = ARG(1);
    return old;
}

/* int SetBkMode(HDC hdc, int mode) */
static u32 imp_SetBkMode(Cpu *c)
{
    (void)c;
    u32 old = gdi_bk_mode;
    gdi_bk_mode = ARG(1);
    return old;
}

/* COLORREF SetTextColor(HDC hdc, COLORREF color) */
static u32 imp_SetTextColor(Cpu *c)
{
    (void)c;
    u32 old = gdi_text_color;
    gdi_text_color = ARG(1);
    return old;
}

/* HGDIOBJ SelectObject(HDC hdc, HGDIOBJ h) */
static u32 imp_SelectObject(Cpu *c)
{
    (void)c;
    u32 h = ARG(1);
    if (!h) return 0;

    if (h & 0x80000000u) {
        u32 idx = h & 0xffu;
        if ((idx >= 10 && idx <= 14) || idx == 16 || idx == 17) {
            u32 old = cur_selected_font;
            cur_selected_font = h;
            return old;
        } else if (idx <= 5) {
            u32 old = cur_selected_brush;
            cur_selected_brush = h;
            return old;
        } else if (idx >= 6 && idx <= 8) {
            u32 old = cur_selected_pen;
            cur_selected_pen = h;
            return old;
        }
        return h;
    }

    GdiObj *obj = (GdiObj *)handle_get(h, H_OTHER);
    if (obj && obj->magic == GDI_MAGIC) {
        if (obj->type == 1) {
            u32 old = cur_selected_font;
            cur_selected_font = h;
            return old;
        } else if (obj->type == 2) {
            u32 old = cur_selected_pen;
            cur_selected_pen = h;
            return old;
        } else if (obj->type == 3) {
            u32 old = cur_selected_brush;
            cur_selected_brush = h;
            return old;
        }
    }
    return h;
}

/* HFONT CreateFontA(int cHeight, int cWidth, int cEscapement, int cOrientation,
                     int cWeight, DWORD bItalic, DWORD bUnderline, DWORD bStrikeOut,
                     DWORD iCharSet, DWORD iOutPrecision, DWORD iClipPrecision,
                     DWORD iQuality, DWORD iPitchAndFamily, LPCSTR pszFaceName) */
static u32 imp_CreateFontA(Cpu *c)
{
    GdiObj *obj = (GdiObj *)calloc(1, sizeof(GdiObj));
    if (!obj) return 0;
    obj->magic = GDI_MAGIC;
    obj->type = 1;
    obj->height = (int)ARG(0);
    obj->width = (int)ARG(1);
    obj->weight = (int)ARG(4);
    if (ARG(13)) {
        strncpy(obj->face, GSTR(ARG(13)), sizeof(obj->face) - 1);
        obj->face[sizeof(obj->face) - 1] = '\0';
    }
    return handle_new(H_OTHER, obj);
}

/* BOOL GetTextMetricsA(HDC hdc, LPTEXTMETRICA lptm) */
static u32 imp_GetTextMetricsA(Cpu *c)
{
    (void)c;
    u32 p = ARG(1);
    if (!p) return 0;

    int h = 16, w = 8;
    if (cur_selected_font && !(cur_selected_font & 0x80000000u)) {
        GdiObj *font = (GdiObj *)handle_get(cur_selected_font, H_OTHER);
        if (font && font->magic == GDI_MAGIC && font->type == 1) {
            h = font->height < 0 ? -font->height : font->height;
            if (h < 6) h = 16;
            w = font->width > 0 ? font->width : (h / 2);
        }
    }

    wr32(p + 0,  h);               /* tmHeight */
    wr32(p + 4,  (h * 3) / 4);     /* tmAscent */
    wr32(p + 8,  h - (h * 3) / 4); /* tmDescent */
    wr32(p + 12, 0);               /* tmInternalLeading */
    wr32(p + 16, 0);               /* tmExternalLeading */
    wr32(p + 20, w);               /* tmAveCharWidth */
    wr32(p + 24, w * 2);           /* tmMaxCharWidth */
    wr32(p + 28, 400);             /* tmWeight (FW_NORMAL) */
    wr32(p + 32, 0);               /* tmOverhang */
    wr32(p + 36, 96);              /* tmDigitizedAspectX */
    wr32(p + 40, 96);              /* tmDigitizedAspectY */
    wr8(p + 44,  32);              /* tmFirstChar */
    wr8(p + 45,  255);             /* tmLastChar */
    wr8(p + 46,  '?');             /* tmDefaultChar */
    wr8(p + 47,  ' ');             /* tmBreakChar */
    wr8(p + 48,  0);               /* tmItalic */
    wr8(p + 49,  0);               /* tmUnderlined */
    wr8(p + 50,  0);               /* tmStruckOut */
    wr8(p + 51,  0);               /* tmPitchAndFamily */
    wr8(p + 52,  0);               /* tmCharSet */
    return 1;
}

/* int GetDeviceCaps(HDC hdc, int index) */
static u32 imp_GetDeviceCaps(Cpu *c)
{
    (void)c;
    switch (ARG(1)) {
    case 8:   /* HORZRES */    return DESKTOP_W;
    case 10:  /* VERTRES */    return DESKTOP_H;
    case 12:  /* BITSPIXEL */  return 16;
    case 14:  /* PLANES */     return 1;
    case 2:   /* TECHNOLOGY */ return 1;   /* DT_RASDISPLAY */
    case 4:   /* HORZSIZE */   return 320; /* mm */
    case 6:   /* VERTSIZE */   return 240; /* mm */
    case 24:  /* NUMCOLORS */  return 65536;
    case 40:  /* ASPECTX */    return 36;
    case 42:  /* ASPECTY */    return 36;
    case 44:  /* ASPECTXY */   return 51;
    case 88:  /* LOGPIXELSX */ return 96;
    case 90:  /* LOGPIXELSY */ return 96;
    case 108: /* COLORRES */   return 16;
    case 116: /* VREFRESH */   return 60;
    default:  return 0;
    }
}

/* HGDIOBJ GetStockObject(int i) */
static u32 imp_GetStockObject(Cpu *c)
{
    (void)c;
    return 0x80000000u | (ARG(0) & 0xffu);
}

/* BOOL DeleteObject(HGDIOBJ ho) */
static u32 imp_DeleteObject(Cpu *c)
{
    (void)c;
    u32 h = ARG(0);
    if (!h || (h & 0x80000000u)) return 1;
    GdiObj *obj = (GdiObj *)handle_get(h, H_OTHER);
    if (obj && obj->magic == GDI_MAGIC) {
        obj->magic = 0;
        free(obj);
        handle_free(h);
    }
    return 1;
}

const struct ImpDef imp_gdi32[] = {
    IMPORT(TextOutA, 5),
    IMPORT(SetBkColor, 2),
    IMPORT(SetBkMode, 2),
    IMPORT(SetTextColor, 2),
    IMPORT(SelectObject, 2),
    IMPORT(CreateFontA, 14),
    IMPORT(GetTextMetricsA, 2),
    IMPORT(GetDeviceCaps, 2),
    IMPORT(GetStockObject, 1),
    IMPORT(DeleteObject, 1),
};
IMPORT_TABLE(gdi32);

/* ========================================================================= */
/* ADVAPI32: Registry and User Info                                          */
/* ========================================================================= */

#define REG_KEY_MAGIC 0x5245474bu

typedef struct RegKeyHandle {
    u32 magic;
    char path[512];
} RegKeyHandle;

typedef struct RegValue {
    char name[128];
    u32 type;
    u32 data_len;
    u8 *data;
} RegValue;

typedef struct RegKey {
    char path[512];
    RegValue *values;
    int num_values;
    int cap_values;
} RegKey;

static RegKey *reg_keys = NULL;
static int reg_keys_count = 0;
static int reg_keys_cap = 0;
static int reg_loaded = 0;
static int reg_dirty = 0;
static pthread_mutex_t reg_mutex = PTHREAD_MUTEX_INITIALIZER;

static const char *get_root_name(u32 hKey)
{
    switch (hKey) {
    case 0x80000000u: return "HKEY_CLASSES_ROOT";
    case 0x80000001u: return "HKEY_CURRENT_USER";
    case 0x80000002u: return "HKEY_LOCAL_MACHINE";
    case 0x80000003u: return "HKEY_USERS";
    case 0x80000004u: return "HKEY_PERFORMANCE_DATA";
    case 0x80000005u: return "HKEY_CURRENT_CONFIG";
    case 0x80000006u: return "HKEY_DYN_DATA";
    default: return NULL;
    }
}

static int resolve_key_path(u32 hKey, const char *sub_path, char *out_path, size_t out_size)
{
    const char *base = get_root_name(hKey);
    char parent[512] = {0};
    if (base) {
        strncpy(parent, base, sizeof(parent) - 1);
    } else {
        RegKeyHandle *rh = (RegKeyHandle *)handle_get(hKey, H_OTHER);
        if (!rh || rh->magic != REG_KEY_MAGIC) return -1;
        strncpy(parent, rh->path, sizeof(parent) - 1);
    }

    if (!sub_path || !*sub_path) {
        strncpy(out_path, parent, out_size - 1);
        out_path[out_size - 1] = '\0';
        return 0;
    }

    while (*sub_path == '\\' || *sub_path == '/') sub_path++;
    if (!*sub_path) {
        strncpy(out_path, parent, out_size - 1);
        out_path[out_size - 1] = '\0';
        return 0;
    }

    char clean_sub[512];
    strncpy(clean_sub, sub_path, sizeof(clean_sub) - 1);
    clean_sub[sizeof(clean_sub) - 1] = '\0';
    for (char *p = clean_sub; *p; p++) {
        if (*p == '/') *p = '\\';
    }
    size_t len = strlen(clean_sub);
    while (len > 0 && clean_sub[len - 1] == '\\') {
        clean_sub[--len] = '\0';
    }

    snprintf(out_path, out_size, "%s\\%s", parent, clean_sub);
    return 0;
}

static int reg_key_exists_locked(const char *path)
{
    if (!strcasecmp(path, "HKEY_CLASSES_ROOT") ||
        !strcasecmp(path, "HKEY_CURRENT_USER") ||
        !strcasecmp(path, "HKEY_LOCAL_MACHINE") ||
        !strcasecmp(path, "HKEY_USERS") ||
        !strcasecmp(path, "HKEY_PERFORMANCE_DATA") ||
        !strcasecmp(path, "HKEY_CURRENT_CONFIG") ||
        !strcasecmp(path, "HKEY_DYN_DATA") ||
        /* present on every Windows system: the game opens it before creating its own key */
        !strcasecmp(path, "HKEY_LOCAL_MACHINE\\Software") ||
        !strcasecmp(path, "HKEY_CURRENT_USER\\Software"))
        return 1;

    size_t plen = strlen(path);
    for (int i = 0; i < reg_keys_count; i++) {
        if (!strcasecmp(reg_keys[i].path, path)) return 1;
        size_t klen = strlen(reg_keys[i].path);
        if (klen > plen && !strncasecmp(reg_keys[i].path, path, plen) && reg_keys[i].path[plen] == '\\')
            return 1;
    }
    return 0;
}

static RegKey *reg_get_or_create_key_locked(const char *path)
{
    for (int i = 0; i < reg_keys_count; i++) {
        if (!strcasecmp(reg_keys[i].path, path))
            return &reg_keys[i];
    }
    if (reg_keys_count >= reg_keys_cap) {
        reg_keys_cap = reg_keys_cap ? reg_keys_cap * 2 : 16;
        reg_keys = (RegKey *)realloc(reg_keys, reg_keys_cap * sizeof(RegKey));
    }
    RegKey *k = &reg_keys[reg_keys_count++];
    memset(k, 0, sizeof(*k));
    strncpy(k->path, path, sizeof(k->path) - 1);
    reg_dirty = 1;
    return k;
}

static void reg_set_value_locked(RegKey *key, const char *vname, u32 type, const u8 *data, u32 dlen)
{
    if (!vname) vname = "";
    for (int j = 0; j < key->num_values; j++) {
        if (!strcasecmp(key->values[j].name, vname)) {
            RegValue *v = &key->values[j];
            free(v->data);
            v->type = type;
            v->data_len = dlen;
            v->data = (u8 *)malloc(dlen ? dlen : 1);
            if (dlen && data) memcpy(v->data, data, dlen);
            reg_dirty = 1;
            return;
        }
    }
    if (key->num_values >= key->cap_values) {
        key->cap_values = key->cap_values ? key->cap_values * 2 : 4;
        key->values = (RegValue *)realloc(key->values, key->cap_values * sizeof(RegValue));
    }
    RegValue *v = &key->values[key->num_values++];
    memset(v, 0, sizeof(*v));
    strncpy(v->name, vname, sizeof(v->name) - 1);
    v->type = type;
    v->data_len = dlen;
    v->data = (u8 *)malloc(dlen ? dlen : 1);
    if (dlen && data) memcpy(v->data, data, dlen);
    reg_dirty = 1;
}

static RegValue *reg_find_value_locked(const char *kpath, const char *vname)
{
    if (!vname) vname = "";
    for (int i = 0; i < reg_keys_count; i++) {
        if (!strcasecmp(reg_keys[i].path, kpath)) {
            for (int j = 0; j < reg_keys[i].num_values; j++) {
                if (!strcasecmp(reg_keys[i].values[j].name, vname))
                    return &reg_keys[i].values[j];
            }
            return NULL;
        }
    }
    return NULL;
}

static void reg_save_locked(void)
{
    char reg_path[1200];
    snprintf(reg_path, sizeof(reg_path), "%s/registry.txt", game_dir);
    FILE *f = fopen(reg_path, "w");
    if (!f) return;
    for (int i = 0; i < reg_keys_count; i++) {
        fprintf(f, "K %s\n", reg_keys[i].path);
        for (int j = 0; j < reg_keys[i].num_values; j++) {
            RegValue *v = &reg_keys[i].values[j];
            fprintf(f, "V %u ", v->type);
            if (v->name[0] == '\0') {
                fputc('@', f);
            } else {
                for (int k = 0; v->name[k]; k++)
                    fprintf(f, "%02x", (unsigned char)v->name[k]);
            }
            fputc(' ', f);
            if (v->data_len == 0) {
                fputc('@', f);
            } else {
                for (u32 k = 0; k < v->data_len; k++)
                    fprintf(f, "%02x", v->data[k]);
            }
            fprintf(f, " %s\n", reg_keys[i].path);
        }
    }
    fclose(f);
    reg_dirty = 0;
}

static void reg_load_locked(void)
{
    if (reg_loaded) return;
    reg_loaded = 1;
    /* the game's installer creates its key; the game only opens it */
    reg_get_or_create_key_locked("HKEY_LOCAL_MACHINE\\Software\\Electronic Arts\\Future Cop");
    char reg_path[1200];
    snprintf(reg_path, sizeof(reg_path), "%s/registry.txt", game_dir);
    FILE *f = fopen(reg_path, "r");
    if (!f) return;
    char line[8192];
    while (fgets(line, sizeof(line), f)) {
        char *p = line + strlen(line);
        while (p > line && (p[-1] == '\r' || p[-1] == '\n' || p[-1] == ' ')) *--p = '\0';
        if (line[0] == 'K' && line[1] == ' ') {
            reg_get_or_create_key_locked(line + 2);
        } else if (line[0] == 'V' && line[1] == ' ') {
            char *cur = line + 2;
            while (*cur == ' ') cur++;
            u32 type = (u32)strtoul(cur, &cur, 10);
            while (*cur == ' ') cur++;
            char *token_name = cur;
            while (*cur && *cur != ' ') cur++;
            if (*cur) *cur++ = '\0';
            while (*cur == ' ') cur++;
            char *token_data = cur;
            while (*cur && *cur != ' ') cur++;
            if (*cur) *cur++ = '\0';
            while (*cur == ' ') cur++;
            char *token_path = cur;

            if (*token_path) {
                RegKey *k = reg_get_or_create_key_locked(token_path);
                char name[128] = {0};
                if (strcmp(token_name, "@") != 0) {
                    size_t nlen = strlen(token_name) / 2;
                    if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
                    for (size_t x = 0; x < nlen; x++) {
                        unsigned int b = 0;
                        if (sscanf(token_name + 2 * x, "%02x", &b) == 1)
                            name[x] = (char)b;
                    }
                    name[nlen] = '\0';
                }
                u32 dlen = 0;
                u8 *data = NULL;
                if (strcmp(token_data, "@") != 0) {
                    dlen = (u32)(strlen(token_data) / 2);
                    data = (u8 *)malloc(dlen ? dlen : 1);
                    for (u32 x = 0; x < dlen; x++) {
                        unsigned int b = 0;
                        if (sscanf(token_data + 2 * x, "%02x", &b) == 1)
                            data[x] = (u8)b;
                    }
                }
                reg_set_value_locked(k, name, type, data, dlen);
                free(data);
            }
        }
    }
    fclose(f);
}

/* LONG RegOpenKeyExA(HKEY hKey, LPCSTR lpSubKey, DWORD ulOptions, REGSAM samDesired, PHKEY phkResult) */
static u32 imp_RegOpenKeyExA(Cpu *c)
{
    if (getenv("FCOP_TRACE_REG")) plat_log("RegOpenKeyExA(key 0x%x, sub '%s') from 0x%08x", ARG(0), ARG(1) ? GSTR(ARG(1)) : "", rd32(c->r[4]));
    u32 hKey = ARG(0);
    u32 sub_ptr = ARG(1);
    u32 out_ptr = ARG(4);

    if (!out_ptr) return 87; /* ERROR_INVALID_PARAMETER */

    const char *sub = sub_ptr ? GSTR(sub_ptr) : NULL;
    char path[512];
    if (resolve_key_path(hKey, sub, path, sizeof(path)) != 0) {
        return 6; /* ERROR_INVALID_HANDLE */
    }

    pthread_mutex_lock(&reg_mutex);
    reg_load_locked();
    if (!reg_key_exists_locked(path)) {
        pthread_mutex_unlock(&reg_mutex);
        return 2; /* ERROR_FILE_NOT_FOUND */
    }
    pthread_mutex_unlock(&reg_mutex);

    RegKeyHandle *rh = (RegKeyHandle *)calloc(1, sizeof(RegKeyHandle));
    if (!rh) return 8; /* ERROR_NOT_ENOUGH_MEMORY */
    rh->magic = REG_KEY_MAGIC;
    strncpy(rh->path, path, sizeof(rh->path) - 1);
    u32 h = handle_new(H_OTHER, rh);
    wr32(out_ptr, h);
    return 0; /* ERROR_SUCCESS */
}

/* LONG RegCreateKeyExA(HKEY hKey, LPCSTR lpSubKey, DWORD Reserved, LPSTR lpClass,
                        DWORD dwOptions, REGSAM samDesired,
                        LPSECURITY_ATTRIBUTES lpSecurityAttributes, PHKEY phkResult,
                        LPDWORD lpdwDisposition) */
static u32 imp_RegCreateKeyExA(Cpu *c)
{
    if (getenv("FCOP_TRACE_REG")) plat_log("RegCreateKeyExA(key 0x%x, sub '%s') from 0x%08x", ARG(0), ARG(1) ? GSTR(ARG(1)) : "", rd32(c->r[4]));
    u32 hKey = ARG(0);
    u32 sub_ptr = ARG(1);
    u32 out_ptr = ARG(7);
    u32 disp_ptr = ARG(8);

    if (!out_ptr) return 87; /* ERROR_INVALID_PARAMETER */

    const char *sub = sub_ptr ? GSTR(sub_ptr) : NULL;
    char path[512];
    if (resolve_key_path(hKey, sub, path, sizeof(path)) != 0) {
        return 6; /* ERROR_INVALID_HANDLE */
    }

    pthread_mutex_lock(&reg_mutex);
    reg_load_locked();
    int existed = reg_key_exists_locked(path);
    reg_get_or_create_key_locked(path);
    if (reg_dirty) reg_save_locked();
    pthread_mutex_unlock(&reg_mutex);

    if (disp_ptr) {
        wr32(disp_ptr, existed ? 2 /* REG_OPENED_EXISTING_KEY */ : 1 /* REG_CREATED_NEW_KEY */);
    }

    RegKeyHandle *rh = (RegKeyHandle *)calloc(1, sizeof(RegKeyHandle));
    if (!rh) return 8; /* ERROR_NOT_ENOUGH_MEMORY */
    rh->magic = REG_KEY_MAGIC;
    strncpy(rh->path, path, sizeof(rh->path) - 1);
    u32 h = handle_new(H_OTHER, rh);
    wr32(out_ptr, h);
    return 0; /* ERROR_SUCCESS */
}

/* LONG RegCloseKey(HKEY hKey) */
static u32 imp_RegCloseKey(Cpu *c)
{
    u32 hKey = ARG(0);
    if (get_root_name(hKey)) return 0;

    RegKeyHandle *rh = (RegKeyHandle *)handle_get(hKey, H_OTHER);
    if (!rh || rh->magic != REG_KEY_MAGIC) return 6; /* ERROR_INVALID_HANDLE */

    rh->magic = 0;
    free(rh);
    handle_free(hKey);

    pthread_mutex_lock(&reg_mutex);
    if (reg_dirty) reg_save_locked();
    pthread_mutex_unlock(&reg_mutex);

    return 0;
}

/* LONG RegFlushKey(HKEY hKey) */
static u32 imp_RegFlushKey(Cpu *c)
{
    (void)c;
    pthread_mutex_lock(&reg_mutex);
    reg_load_locked();
    reg_save_locked();
    pthread_mutex_unlock(&reg_mutex);
    return 0;
}

/* LONG RegQueryValueExA(HKEY hKey, LPCSTR lpValueName, LPDWORD lpReserved,
                         LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData) */
static u32 imp_RegQueryValueExA(Cpu *c)
{
    if (getenv("FCOP_TRACE_REG")) plat_log("RegQueryValueExA(key 0x%x, value '%s') from 0x%08x", ARG(0), ARG(1) ? GSTR(ARG(1)) : "", rd32(c->r[4]));
    u32 hKey = ARG(0);
    u32 vname_ptr = ARG(1);
    u32 type_ptr = ARG(3);
    u32 data_ptr = ARG(4);
    u32 len_ptr = ARG(5);

    char path[512];
    if (resolve_key_path(hKey, NULL, path, sizeof(path)) != 0) {
        return 6; /* ERROR_INVALID_HANDLE */
    }

    const char *vname = vname_ptr ? GSTR(vname_ptr) : "";

    pthread_mutex_lock(&reg_mutex);
    reg_load_locked();
    RegValue *val = reg_find_value_locked(path, vname);
    if (!val) {
        pthread_mutex_unlock(&reg_mutex);
        return 2; /* ERROR_FILE_NOT_FOUND */
    }

    if (type_ptr) wr32(type_ptr, val->type);

    if (!len_ptr) {
        pthread_mutex_unlock(&reg_mutex);
        return 87; /* ERROR_INVALID_PARAMETER */
    }

    if (!data_ptr) {
        wr32(len_ptr, val->data_len);
        pthread_mutex_unlock(&reg_mutex);
        return 0; /* ERROR_SUCCESS */
    }

    u32 avail = rd32(len_ptr);
    if (avail < val->data_len) {
        wr32(len_ptr, val->data_len);
        pthread_mutex_unlock(&reg_mutex);
        return 234; /* ERROR_MORE_DATA */
    }

    if (val->data_len) {
        memcpy(G(data_ptr), val->data, val->data_len);
    }
    wr32(len_ptr, val->data_len);
    pthread_mutex_unlock(&reg_mutex);

    return 0; /* ERROR_SUCCESS */
}

/* LONG RegSetValueExA(HKEY hKey, LPCSTR lpValueName, DWORD Reserved,
                       DWORD dwType, const BYTE *lpData, DWORD cbData) */
static u32 imp_RegSetValueExA(Cpu *c)
{
    if (getenv("FCOP_TRACE_REG")) plat_log("RegSetValueExA(key 0x%x, value '%s') from 0x%08x", ARG(0), ARG(1) ? GSTR(ARG(1)) : "", rd32(c->r[4]));
    u32 hKey = ARG(0);
    u32 vname_ptr = ARG(1);
    u32 type = ARG(3);
    u32 data_ptr = ARG(4);
    u32 cbData = ARG(5);

    char path[512];
    if (resolve_key_path(hKey, NULL, path, sizeof(path)) != 0) {
        return 6; /* ERROR_INVALID_HANDLE */
    }

    const char *vname = vname_ptr ? GSTR(vname_ptr) : "";
    const u8 *data = (cbData && data_ptr) ? (const u8 *)G(data_ptr) : NULL;

    pthread_mutex_lock(&reg_mutex);
    reg_load_locked();
    RegKey *key = reg_get_or_create_key_locked(path);
    reg_set_value_locked(key, vname, type, data, cbData);
    reg_save_locked();
    pthread_mutex_unlock(&reg_mutex);

    return 0; /* ERROR_SUCCESS */
}

/* BOOL GetUserNameA(LPSTR lpBuffer, LPDWORD pcbBuffer) */
static u32 imp_GetUserNameA(Cpu *c)
{
    u32 buf = ARG(0);
    u32 len_ptr = ARG(1);
    if (!len_ptr) {
        last_error = 87; /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    const char *user = getenv("USER");
    if (!user || !*user) user = "Player";
    u32 needed = (u32)strlen(user) + 1;
    u32 avail = rd32(len_ptr);
    if (!buf || avail < needed) {
        wr32(len_ptr, needed);
        last_error = 122; /* ERROR_INSUFFICIENT_BUFFER */
        return 0;
    }
    memcpy(G(buf), user, needed);
    wr32(len_ptr, needed);
    return 1;
}

const struct ImpDef imp_advapi32[] = {
    IMPORT(RegCreateKeyExA, 9),
    IMPORT(RegCloseKey, 1),
    IMPORT(RegFlushKey, 1),
    IMPORT(GetUserNameA, 2),
    IMPORT(RegSetValueExA, 6),
    IMPORT(RegOpenKeyExA, 5),
    IMPORT(RegQueryValueExA, 6),
};
IMPORT_TABLE(advapi32);

/* ========================================================================= */
/* ole32: Object Linking and Embedding / COM                                 */
/* ========================================================================= */

/* HRESULT CoInitialize(LPVOID pvReserved) */
static u32 imp_CoInitialize(Cpu *c)
{
    (void)c;
    return 0; /* S_OK */
}

/* void CoUninitialize(void) */
static u32 imp_CoUninitialize(Cpu *c)
{
    (void)c;
    return 0;
}

/* HRESULT CoCreateInstance(REFCLSID rclsid, LPUNKNOWN pUnkOuter,
                            DWORD dwClsContext, REFIID riid, LPVOID *ppv) */
static u32 imp_CoCreateInstance(Cpu *c)
{
    u32 rclsid = ARG(0);
    u32 ppv = ARG(4);

    if (rclsid) {
        u32 d1 = rd32(rclsid);
        u16 d2 = rd16(rclsid + 4);
        u16 d3 = rd16(rclsid + 6);
        u8 b[8];
        for (int i = 0; i < 8; i++) b[i] = rd8(rclsid + 8 + i);
        plat_log("CoCreateInstance CLSID: {%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
                 d1, d2, d3, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
    }
    if (ppv) wr32(ppv, 0);
    return 0x80004002u; /* E_NOINTERFACE */
}

const struct ImpDef imp_ole32[] = {
    IMPORT(CoInitialize, 1),
    IMPORT(CoUninitialize, 0),
    IMPORT(CoCreateInstance, 5),
};
IMPORT_TABLE(ole32);

/* ========================================================================= */
/* WINMM: Windows Multimedia                                                 */
/* ========================================================================= */

typedef struct TimerEvent {
    u32 id;
    u32 delay_ms;
    u32 resolution_ms;
    u32 callback;
    u32 user_data;
    u32 flags;
    volatile int active;
    volatile int joined;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    Cpu cpu;
    u32 stack_lo;
    u32 stack_hi;
    u32 tib;
} TimerEvent;

#define MAX_TIMERS 32
static TimerEvent *timers[MAX_TIMERS];
static pthread_mutex_t timers_lock = PTHREAD_MUTEX_INITIALIZER;
static u32 next_timer_id = 1;

static struct timespec winmm_start_time;
static int winmm_start_init = 0;

static u32 winmm_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (!winmm_start_init) {
        winmm_start_time = ts;
        winmm_start_init = 1;
    }
    u64 ms = (u64)(ts.tv_sec - winmm_start_time.tv_sec) * 1000 + (ts.tv_nsec - winmm_start_time.tv_nsec) / 1000000;
    return (u32)ms;
}

/* BOOL PlaySoundA(LPCSTR pszSound, HMODULE hmod, DWORD fdwSound) */
static u32 imp_PlaySoundA(Cpu *c)
{
    return sound_playsound(ARG(0), ARG(1), ARG(2));
}

/* DWORD timeGetTime(void) */
static u32 imp_timeGetTime(Cpu *c)
{
    (void)c;
    return winmm_time_ms();
}

/* MMRESULT timeGetDevCaps(LPTIMECAPS ptc, UINT cbtc) */
static u32 imp_timeGetDevCaps(Cpu *c)
{
    u32 ptc = ARG(0);
    u32 cbtc = ARG(1);
    if (!ptc || cbtc < 8) return 11; /* MMSYSERR_INVALPARAM */
    wr32(ptc + 0, 1);       /* wPeriodMin = 1 ms */
    wr32(ptc + 4, 1000000); /* wPeriodMax = 1000000 ms */
    return 0; /* TIMERR_NOERROR */
}

/* MMRESULT timeBeginPeriod(UINT uPeriod) */
static u32 imp_timeBeginPeriod(Cpu *c)
{
    (void)c;
    return 0; /* TIMERR_NOERROR */
}

/* MMRESULT timeEndPeriod(UINT uPeriod) */
static u32 imp_timeEndPeriod(Cpu *c)
{
    (void)c;
    return 0; /* TIMERR_NOERROR */
}

static void *timer_thread_func(void *arg)
{
    TimerEvent *te = (TimerEvent *)arg;
    cur_cpu = &te->cpu;

    while (1) {
        pthread_mutex_lock(&te->lock);
        if (!te->active) {
            pthread_mutex_unlock(&te->lock);
            break;
        }

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        u32 delay = te->delay_ms ? te->delay_ms : 1;
        ts.tv_sec += delay / 1000;
        ts.tv_nsec += (long)(delay % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }

        int r = pthread_cond_timedwait(&te->cond, &te->lock, &ts);
        if (!te->active) {
            pthread_mutex_unlock(&te->lock);
            break;
        }
        pthread_mutex_unlock(&te->lock);

        if (r == ETIMEDOUT) {
            if (te->callback) {
                u32 args[5] = { te->id, 0, te->user_data, 0, 0 };
                guest_call(&te->cpu, te->callback, 5, args);
            }
            if ((te->flags & 1) == 0) {
                /* TIME_ONESHOT */
                pthread_mutex_lock(&te->lock);
                te->active = 0;
                pthread_mutex_unlock(&te->lock);
                break;
            }
        }
    }
    return NULL;
}

/* MMRESULT timeSetEvent(UINT uDelay, UINT uResolution, LPTIMECALLBACK lpTimeProc,
                         DWORD_PTR dwUser, UINT fuEvent) */
static u32 imp_timeSetEvent(Cpu *c)
{
    u32 delay = ARG(0);
    u32 res = ARG(1);
    u32 cb = ARG(2);
    u32 user = ARG(3);
    u32 flags = ARG(4);

    if (!cb) return 0;

    pthread_mutex_lock(&timers_lock);
    int slot = -1;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!timers[i]) {
            slot = i;
            break;
        } else if (!timers[i]->active) {
            TimerEvent *old = timers[i];
            timers[i] = NULL;
            pthread_mutex_unlock(&timers_lock);
            if (!old->joined) {
                pthread_join(old->thread, NULL);
            }
            pthread_mutex_destroy(&old->lock);
            pthread_cond_destroy(&old->cond);
            if (old->tib) gfree(old->tib);
            if (old->stack_lo) gfree(old->stack_lo);
            free(old);
            pthread_mutex_lock(&timers_lock);
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        pthread_mutex_unlock(&timers_lock);
        plat_log("timeSetEvent: out of timer slots");
        return 0;
    }

    TimerEvent *te = (TimerEvent *)calloc(1, sizeof(TimerEvent));
    if (!te) {
        pthread_mutex_unlock(&timers_lock);
        return 0;
    }

    u32 stack_size = 1024 * 1024;
    u32 stack_lo = galloc(stack_size);
    u32 stack_hi = stack_lo + stack_size;
    u32 tib = galloc(0x1000);
    if (!stack_lo || !tib) {
        if (stack_lo) gfree(stack_lo);
        if (tib) gfree(tib);
        free(te);
        pthread_mutex_unlock(&timers_lock);
        return 0;
    }

    wr32(tib + 0x00, 0xffffffffu);
    wr32(tib + 0x04, stack_hi);
    wr32(tib + 0x08, stack_lo);
    wr32(tib + 0x18, tib);

    te->id = next_timer_id++;
    te->delay_ms = delay;
    te->resolution_ms = res;
    te->callback = cb;
    te->user_data = user;
    te->flags = flags;
    te->active = 1;
    te->joined = 0;
    te->stack_lo = stack_lo;
    te->stack_hi = stack_hi;
    te->tib = tib;

    te->cpu.cw = 0x27f;
    te->cpu.fs_base = tib;
    te->cpu.r[4] = stack_hi - 64;

    pthread_mutex_init(&te->lock, NULL);
    pthread_cond_init(&te->cond, NULL);

    timers[slot] = te;
    u32 tid = te->id;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);
    if (pthread_create(&te->thread, &attr, timer_thread_func, te) != 0) {
        pthread_attr_destroy(&attr);
        timers[slot] = NULL;
        pthread_mutex_destroy(&te->lock);
        pthread_cond_destroy(&te->cond);
        gfree(tib);
        gfree(stack_lo);
        free(te);
        pthread_mutex_unlock(&timers_lock);
        return 0;
    }
    pthread_attr_destroy(&attr);
    pthread_mutex_unlock(&timers_lock);

    return tid;
}

/* MMRESULT timeKillEvent(UINT uTimerID) */
static u32 imp_timeKillEvent(Cpu *c)
{
    u32 id = ARG(0);
    if (!id) return 11; /* MMSYSERR_INVALPARAM */

    pthread_mutex_lock(&timers_lock);
    TimerEvent *te = NULL;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (timers[i] && timers[i]->id == id) {
            te = timers[i];
            timers[i] = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&timers_lock);

    if (!te) return 11; /* MMSYSERR_INVALPARAM */

    pthread_mutex_lock(&te->lock);
    te->active = 0;
    pthread_cond_broadcast(&te->cond);
    pthread_mutex_unlock(&te->lock);

    if (!pthread_equal(pthread_self(), te->thread)) {
        pthread_join(te->thread, NULL);
        te->joined = 1;
    } else {
        pthread_detach(te->thread);
        te->joined = 1;
    }

    pthread_mutex_destroy(&te->lock);
    pthread_cond_destroy(&te->cond);
    if (te->tib) gfree(te->tib);
    if (te->stack_lo) gfree(te->stack_lo);
    free(te);

    return 0; /* TIMERR_NOERROR */
}

const struct ImpDef imp_winmm[] = {
    IMPORT(PlaySoundA, 3),
    IMPORT(timeGetTime, 0),
    IMPORT(timeGetDevCaps, 2),
    IMPORT(timeBeginPeriod, 1),
    IMPORT(timeEndPeriod, 1),
    IMPORT(timeSetEvent, 5),
    IMPORT(timeKillEvent, 1),
};
IMPORT_TABLE(winmm);

/* Fallback table alias */
const struct ImpDef imp_gdi_misc[] = {
    {"", 0, NULL},
};
IMPORT_TABLE(gdi_misc);
