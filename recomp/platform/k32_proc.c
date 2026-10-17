#define _GNU_SOURCE
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host.h"

/* ---- Version & Module Info ----------------------------------------------------------- */

/* DWORD GetVersion(void)
 * Windows 98: 0xC0000A04 (Major 4, Minor 10, Win9x flag 0xC000) */
static u32 imp_GetVersion(Cpu *c)
{
    (void)c;
    return 0xC0000A04u;
}

/* LPSTR GetCommandLineA(void) */
static u32 cmd_line_ptr;
static u32 imp_GetCommandLineA(Cpu *c)
{
    (void)c;
    if (!cmd_line_ptr)
        cmd_line_ptr = gstrdup("FCopLAPD.exe");
    return cmd_line_ptr;
}

/* VOID GetStartupInfoA(LPSTARTUPINFOA lpStartupInfo) */
static u32 imp_GetStartupInfoA(Cpu *c)
{
    u32 p = ARG(0);
    if (p) {
        u32 cb = rd32(p);
        if (cb < 68) cb = 68;
        memset(G(p), 0, cb);
        wr32(p + 0x00, 68);          /* cb */
        wr16(p + 0x30, 1);           /* wShowWindow = SW_SHOWNORMAL */
        wr32(p + 0x38, HSTD_INPUT);  /* hStdInput */
        wr32(p + 0x3c, HSTD_OUTPUT); /* hStdOutput */
        wr32(p + 0x40, HSTD_ERROR);  /* hStdError */
    }
    return 0;
}

/* DWORD GetModuleFileNameA(HMODULE hModule, LPSTR lpFilename, DWORD nSize) */
static u32 imp_GetModuleFileNameA(Cpu *c)
{
    u32 hmod = ARG(0);
    u32 buf = ARG(1);
    u32 size = ARG(2);
    const char *path = "C:\\FCOP\\FCopLAPD.exe";
    u32 len = (u32)strlen(path);
    (void)hmod;

    if (!buf || !size) return 0;
    if (len >= size) {
        memcpy(G(buf), path, size);
        last_error = 122; /* ERROR_INSUFFICIENT_BUFFER */
        return size;
    }
    memcpy(G(buf), path, len + 1);
    return len;
}

/* HMODULE GetModuleHandleA(LPCSTR lpModuleName) */
static u32 imp_GetModuleHandleA(Cpu *c)
{
    u32 p = ARG(0);
    if (!p) return 0x400000u;
    const char *name = GSTR(p);
    if (!strcasecmp(name, "FCopLAPD.exe") || !strcasecmp(name, "FCopLAPD"))
        return 0x400000u;
    if (!strcasecmp(name, "KERNEL32") || !strcasecmp(name, "KERNEL32.dll"))
        return 0x70000000u;
    plat_log("warn: GetModuleHandleA('%s') -> NULL", name);
    last_error = 126; /* ERROR_MOD_NOT_FOUND */
    return 0;
}

/* HMODULE LoadLibraryA(LPCSTR lpLibFileName) */
static u32 imp_LoadLibraryA(Cpu *c)
{
    u32 name_p = ARG(0);
    plat_log("LoadLibraryA('%s') -> NULL", name_p ? GSTR(name_p) : "");
    last_error = 126; /* ERROR_MOD_NOT_FOUND */
    return 0;
}

/* FARPROC GetProcAddress(HMODULE hModule, LPCSTR lpProcName) */
static u32 imp_GetProcAddress(Cpu *c)
{
    u32 hmod = ARG(0);
    u32 proc_p = ARG(1);
    if (proc_p > 0xffff) {
        plat_log("GetProcAddress(0x%x, '%s') -> NULL", hmod, GSTR(proc_p));
    } else {
        plat_log("GetProcAddress(0x%x, #%u) -> NULL", hmod, proc_p);
    }
    last_error = 127; /* ERROR_PROC_NOT_FOUND */
    return 0;
}

/* BOOL FreeLibrary(HMODULE hLibModule) */
static u32 imp_FreeLibrary(Cpu *c)
{
    u32 hmod = ARG(0);
    (void)hmod;
    return 1;
}

/* ---- Environment --------------------------------------------------------------------- */

static const char default_env_strings[] =
    "PATH=C:\\WINDOWS;C:\\WINDOWS\\COMMAND\0"
    "WINDIR=C:\\WINDOWS\0"
    "SYSTEMROOT=C:\\WINDOWS\0"
    "TEMP=C:\\WINDOWS\\TEMP\0"
    "TMP=C:\\WINDOWS\\TEMP\0"
    "COMSPEC=C:\\WINDOWS\\COMMAND.COM\0";

/* LPVOID GetEnvironmentStrings(void) */
static u32 imp_GetEnvironmentStrings(Cpu *c)
{
    (void)c;
    u32 size = (u32)sizeof(default_env_strings);
    u32 p = galloc(size);
    if (p) memcpy(G(p), default_env_strings, size);
    return p;
}

/* LPVOID GetEnvironmentStringsW(void) */
static u32 imp_GetEnvironmentStringsW(Cpu *c)
{
    (void)c;
    u32 len = (u32)sizeof(default_env_strings);
    u32 p = galloc(len * 2);
    if (p) {
        for (u32 i = 0; i < len; i++)
            wr16(p + 2 * i, (u8)default_env_strings[i]);
    }
    return p;
}

/* BOOL FreeEnvironmentStringsA(LPSTR lpszEnvironmentBlock) */
static u32 imp_FreeEnvironmentStringsA(Cpu *c)
{
    u32 p = ARG(0);
    if (p) gfree(p);
    return 1;
}

/* BOOL FreeEnvironmentStringsW(LPWSTR lpszEnvironmentBlock) */
static u32 imp_FreeEnvironmentStringsW(Cpu *c)
{
    u32 p = ARG(0);
    if (p) gfree(p);
    return 1;
}

/* BOOL SetEnvironmentVariableA(LPCSTR lpName, LPCSTR lpValue) */
static u32 imp_SetEnvironmentVariableA(Cpu *c)
{
    u32 name_p = ARG(0);
    u32 val_p = ARG(1);
    if (!name_p || !*GSTR(name_p)) {
        last_error = 87; /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    const char *name = GSTR(name_p);
    if (val_p && *GSTR(val_p)) {
        setenv(name, GSTR(val_p), 1);
    } else {
        unsetenv(name);
    }
    return 1;
}

/* BOOL SetEnvironmentVariableW(LPCWSTR lpName, LPCWSTR lpValue) */
static u32 imp_SetEnvironmentVariableW(Cpu *c)
{
    u32 name_p = ARG(0);
    u32 val_p = ARG(1);
    if (!name_p) {
        last_error = 87;
        return 0;
    }
    char name[256] = {0};
    const u16 *wname = (const u16 *)G(name_p);
    int i = 0;
    while (wname[i] && i < 255) {
        name[i] = (char)(wname[i] & 0xff);
        i++;
    }
    name[i] = 0;
    if (!name[0]) {
        last_error = 87;
        return 0;
    }
    if (val_p) {
        char val[1024] = {0};
        const u16 *wval = (const u16 *)G(val_p);
        int j = 0;
        while (wval[j] && j < 1023) {
            val[j] = (char)(wval[j] & 0xff);
            j++;
        }
        val[j] = 0;
        if (val[0]) setenv(name, val, 1);
        else unsetenv(name);
    } else {
        unsetenv(name);
    }
    return 1;
}

/* ---- Handles & I/O Types ------------------------------------------------------------- */

static u32 cur_std_in = HSTD_INPUT;
static u32 cur_std_out = HSTD_OUTPUT;
static u32 cur_std_err = HSTD_ERROR;

/* HANDLE GetStdHandle(DWORD nStdHandle) */
static u32 imp_GetStdHandle(Cpu *c)
{
    u32 n = ARG(0);
    if (n == (u32)-10) return cur_std_in;
    if (n == (u32)-11) return cur_std_out;
    if (n == (u32)-12) return cur_std_err;
    last_error = 6; /* ERROR_INVALID_HANDLE */
    return (u32)-1;
}

/* BOOL SetStdHandle(DWORD nStdHandle, HANDLE hHandle) */
static u32 imp_SetStdHandle(Cpu *c)
{
    u32 n = ARG(0);
    u32 h = ARG(1);
    if (n == (u32)-10) { cur_std_in = h; return 1; }
    if (n == (u32)-11) { cur_std_out = h; return 1; }
    if (n == (u32)-12) { cur_std_err = h; return 1; }
    last_error = 87;
    return 0;
}

/* DWORD GetFileType(HANDLE hFile) */
static u32 imp_GetFileType(Cpu *c)
{
    u32 h = ARG(0);
    if (h == HSTD_INPUT || h == HSTD_OUTPUT || h == HSTD_ERROR)
        return 0x0002; /* FILE_TYPE_CHAR */
    if (handle_get(h, H_FILE))
        return 0x0001; /* FILE_TYPE_DISK */
    if (h == cur_std_in || h == cur_std_out || h == cur_std_err)
        return 0x0002; /* FILE_TYPE_CHAR */
    if (!h) {
        last_error = 6; /* ERROR_INVALID_HANDLE */
        return 0; /* FILE_TYPE_UNKNOWN */
    }
    return 0x0001; /* FILE_TYPE_DISK */
}

/* UINT SetHandleCount(UINT uNumber) */
static u32 imp_SetHandleCount(Cpu *c)
{
    return ARG(0);
}

/* ---- Code Pages, Locale & Character Classification ----------------------------------- */

/* UINT GetACP(void) */
static u32 imp_GetACP(Cpu *c)
{
    (void)c;
    return 1252;
}

/* UINT GetOEMCP(void) */
static u32 imp_GetOEMCP(Cpu *c)
{
    (void)c;
    return 437;
}

/* BOOL GetCPInfo(UINT CodePage, LPCPINFO lpCPInfo) */
static u32 imp_GetCPInfo(Cpu *c)
{
    u32 cp = ARG(0);
    u32 p = ARG(1);
    (void)cp;
    if (!p) {
        last_error = 87;
        return 0;
    }
    wr32(p + 0, 1);          /* MaxCharSize = 1 */
    wr8(p + 4, '?');         /* DefaultChar[0] */
    wr8(p + 5, 0);           /* DefaultChar[1] */
    memset(G(p + 6), 0, 12); /* LeadByte: no lead bytes for single-byte CP */
    return 1;
}

static u16 cp1252_to_ucs2(u8 b)
{
    static const u16 t[32] = {
        0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
        0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
    };
    if (b >= 0x80 && b <= 0x9F) return t[b - 0x80];
    return (u16)b;
}

static u8 ucs2_to_cp1252(u16 wc)
{
    if (wc <= 0x7F) return (u8)wc;
    if (wc >= 0xA0 && wc <= 0xFF) return (u8)wc;
    static const u16 t[32] = {
        0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
        0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
    };
    for (int i = 0; i < 32; i++) {
        if (t[i] == wc) return (u8)(0x80 + i);
    }
    return 0;
}

/* int MultiByteToWideChar(UINT CodePage, DWORD dwFlags, LPCCH lpMultiByteStr,
                           int cbMultiByte, LPWSTR lpWideCharStr, int cchWideChar) */
static u32 imp_MultiByteToWideChar(Cpu *c)
{
    u32 cp = ARG(0);
    u32 flags = ARG(1);
    u32 mb_ptr = ARG(2);
    int cb_mb = (int)ARG(3);
    u32 wc_ptr = ARG(4);
    int cch_wc = (int)ARG(5);
    (void)cp; (void)flags;

    if (!mb_ptr) {
        last_error = 87;
        return 0;
    }
    int len;
    if (cb_mb == -1) {
        len = (int)strlen(GSTR(mb_ptr)) + 1;
    } else if (cb_mb > 0) {
        len = cb_mb;
    } else {
        last_error = 87;
        return 0;
    }

    if (cch_wc == 0) return (u32)len;
    if (cch_wc < len) {
        last_error = 122; /* ERROR_INSUFFICIENT_BUFFER */
        return 0;
    }

    const u8 *src = (const u8 *)G(mb_ptr);
    for (int i = 0; i < len; i++) {
        wr16(wc_ptr + 2 * i, cp1252_to_ucs2(src[i]));
    }
    return (u32)len;
}

/* int WideCharToMultiByte(UINT CodePage, DWORD dwFlags, LPCWCH lpWideCharStr,
                           int cchWideChar, LPSTR lpMultiByteStr, int cbMultiByte,
                           LPCCH lpDefaultChar, LPBOOL lpUsedDefaultChar) */
static u32 imp_WideCharToMultiByte(Cpu *c)
{
    u32 cp = ARG(0);
    u32 flags = ARG(1);
    u32 wc_ptr = ARG(2);
    int cch_wc = (int)ARG(3);
    u32 mb_ptr = ARG(4);
    int cb_mb = (int)ARG(5);
    u32 def_char_p = ARG(6);
    u32 used_def_p = ARG(7);
    (void)cp; (void)flags;

    if (!wc_ptr) {
        last_error = 87;
        return 0;
    }
    int len;
    if (cch_wc == -1) {
        len = 0;
        while (rd16(wc_ptr + 2 * len)) len++;
        len++; /* include null */
    } else if (cch_wc > 0) {
        len = cch_wc;
    } else {
        last_error = 87;
        return 0;
    }

    if (cb_mb == 0) return (u32)len;
    if (cb_mb < len) {
        last_error = 122; /* ERROR_INSUFFICIENT_BUFFER */
        return 0;
    }

    u8 def_char = def_char_p ? rd8(def_char_p) : '?';
    int used_def = 0;
    u8 *dst = (u8 *)G(mb_ptr);
    for (int i = 0; i < len; i++) {
        u16 wc = rd16(wc_ptr + 2 * i);
        u8 b = ucs2_to_cp1252(wc);
        if (b) {
            dst[i] = b;
        } else {
            dst[i] = def_char;
            used_def = 1;
        }
    }
    if (used_def_p) wr32(used_def_p, used_def ? 1 : 0);
    return (u32)len;
}

#define WIN_LCMAP_LOWERCASE 0x00000100u
#define WIN_LCMAP_UPPERCASE 0x00000200u
#define WIN_LCMAP_SORTKEY   0x00000400u
#define WIN_NORM_IGNORECASE 0x00000001u

/* int LCMapStringA(LCID Locale, DWORD dwMapFlags, LPCSTR lpSrcStr,
                    int cchSrc, LPSTR lpDestStr, int cchDest) */
static u32 imp_LCMapStringA(Cpu *c)
{
    u32 loc = ARG(0);
    u32 flags = ARG(1);
    u32 src_p = ARG(2);
    int src_cch = (int)ARG(3);
    u32 dst_p = ARG(4);
    int dst_cch = (int)ARG(5);
    (void)loc;

    if (!src_p) {
        last_error = 87;
        return 0;
    }
    int len = (src_cch == -1) ? (int)strlen(GSTR(src_p)) + 1 : src_cch;
    if (len <= 0) {
        last_error = 87;
        return 0;
    }

    if (flags & WIN_LCMAP_SORTKEY) {
        int copy_len = (src_cch == -1) ? len - 1 : len;
        int needed = copy_len + 1;
        if (dst_cch == 0) return (u32)needed;
        if (dst_cch < needed) {
            last_error = 122;
            return 0;
        }
        const u8 *src = (const u8 *)G(src_p);
        u8 *dst = (u8 *)G(dst_p);
        for (int i = 0; i < copy_len; i++) {
            u8 ch = src[i];
            if (flags & WIN_NORM_IGNORECASE) ch = (u8)tolower(ch);
            dst[i] = ch;
        }
        dst[copy_len] = 0;
        return (u32)needed;
    }

    if (dst_cch == 0) return (u32)len;
    if (dst_cch < len) {
        last_error = 122;
        return 0;
    }
    const u8 *src = (const u8 *)G(src_p);
    u8 *dst = (u8 *)G(dst_p);
    for (int i = 0; i < len; i++) {
        u8 ch = src[i];
        if (flags & WIN_LCMAP_LOWERCASE) ch = (u8)tolower(ch);
        else if (flags & WIN_LCMAP_UPPERCASE) ch = (u8)toupper(ch);
        dst[i] = ch;
    }
    return (u32)len;
}

/* int LCMapStringW(LCID Locale, DWORD dwMapFlags, LPCWSTR lpSrcStr,
                    int cchSrc, LPWSTR lpDestStr, int cchDest) */
static u32 imp_LCMapStringW(Cpu *c)
{
    u32 loc = ARG(0);
    u32 flags = ARG(1);
    u32 src_p = ARG(2);
    int src_cch = (int)ARG(3);
    u32 dst_p = ARG(4);
    int dst_cch = (int)ARG(5);
    (void)loc;

    if (!src_p) {
        last_error = 87;
        return 0;
    }
    int len;
    if (src_cch == -1) {
        len = 0;
        while (rd16(src_p + 2 * len)) len++;
        len++;
    } else {
        len = src_cch;
    }
    if (len <= 0) {
        last_error = 87;
        return 0;
    }

    if (flags & WIN_LCMAP_SORTKEY) {
        int copy_len = (src_cch == -1) ? len - 1 : len;
        int needed_bytes = copy_len + 1;
        if (dst_cch == 0) return (u32)needed_bytes;
        if (dst_cch < needed_bytes) {
            last_error = 122;
            return 0;
        }
        u8 *dst = (u8 *)G(dst_p);
        for (int i = 0; i < copy_len; i++) {
            u16 wc = rd16(src_p + 2 * i);
            if (flags & WIN_NORM_IGNORECASE) wc = (u16)tolower((int)wc);
            dst[i] = (u8)wc;
        }
        dst[copy_len] = 0;
        return (u32)needed_bytes;
    }

    if (dst_cch == 0) return (u32)len;
    if (dst_cch < len) {
        last_error = 122;
        return 0;
    }
    for (int i = 0; i < len; i++) {
        u16 wc = rd16(src_p + 2 * i);
        if (flags & WIN_LCMAP_LOWERCASE) wc = (u16)tolower((int)wc);
        else if (flags & WIN_LCMAP_UPPERCASE) wc = (u16)toupper((int)wc);
        wr16(dst_p + 2 * i, wc);
    }
    return (u32)len;
}

#define WIN_C1_UPPER   0x0001u
#define WIN_C1_LOWER   0x0002u
#define WIN_C1_DIGIT   0x0004u
#define WIN_C1_SPACE   0x0008u
#define WIN_C1_PUNCT   0x0010u
#define WIN_C1_CNTRL   0x0020u
#define WIN_C1_BLANK   0x0040u
#define WIN_C1_XDIGIT  0x0080u
#define WIN_C1_ALPHA   0x0100u
#define WIN_C1_DEFINED 0x0200u

static u16 char_type1(u16 c)
{
    u16 m = 0;
    if (c <= 0xFF) {
        unsigned char ch = (unsigned char)c;
        if (ch) m |= WIN_C1_DEFINED;
        if (isupper(ch)) m |= WIN_C1_UPPER | WIN_C1_ALPHA;
        if (islower(ch)) m |= WIN_C1_LOWER | WIN_C1_ALPHA;
        if (isdigit(ch)) m |= WIN_C1_DIGIT;
        if (isspace(ch)) {
            m |= WIN_C1_SPACE;
            if (ch == ' ' || ch == '\t') m |= WIN_C1_BLANK;
        }
        if (ispunct(ch)) m |= WIN_C1_PUNCT;
        if (iscntrl(ch)) m |= WIN_C1_CNTRL;
        if (isxdigit(ch)) m |= WIN_C1_XDIGIT;
    } else {
        m |= WIN_C1_DEFINED;
    }
    return m;
}

/* BOOL GetStringTypeA(LCID Locale, DWORD dwInfoType, LPCSTR lpSrcStr,
                       int cchSrc, LPWORD lpCharType) */
static u32 imp_GetStringTypeA(Cpu *c)
{
    u32 loc = ARG(0);
    u32 info_type = ARG(1);
    u32 src_p = ARG(2);
    int cch = (int)ARG(3);
    u32 out_p = ARG(4);
    (void)loc; (void)info_type;

    if (!src_p || !out_p) {
        last_error = 87;
        return 0;
    }
    int len = (cch == -1) ? (int)strlen(GSTR(src_p)) + 1 : cch;
    const u8 *src = (const u8 *)G(src_p);
    for (int i = 0; i < len; i++) {
        wr16(out_p + 2 * i, char_type1(src[i]));
    }
    return 1;
}

/* BOOL GetStringTypeW(DWORD dwInfoType, LPCWSTR lpSrcStr,
                       int cchSrc, LPWORD lpCharType) */
static u32 imp_GetStringTypeW(Cpu *c)
{
    u32 info_type = ARG(0);
    u32 src_p = ARG(1);
    int cch = (int)ARG(2);
    u32 out_p = ARG(3);
    (void)info_type;

    if (!src_p || !out_p) {
        last_error = 87;
        return 0;
    }
    int len;
    if (cch == -1) {
        len = 0;
        while (rd16(src_p + 2 * len)) len++;
        len++;
    } else {
        len = cch;
    }
    for (int i = 0; i < len; i++) {
        wr16(out_p + 2 * i, char_type1(rd16(src_p + 2 * i)));
    }
    return 1;
}

/* int CompareStringA(LCID Locale, DWORD dwCmpFlags, PCNZCH lpString1,
                      int cchCount1, PCNZCH lpString2, int cchCount2) */
static u32 imp_CompareStringA(Cpu *c)
{
    u32 loc = ARG(0);
    u32 flags = ARG(1);
    u32 s1_p = ARG(2);
    int cch1 = (int)ARG(3);
    u32 s2_p = ARG(4);
    int cch2 = (int)ARG(5);
    (void)loc;

    if (!s1_p || !s2_p) {
        last_error = 87;
        return 0;
    }
    int len1 = (cch1 == -1) ? (int)strlen(GSTR(s1_p)) : cch1;
    int len2 = (cch2 == -1) ? (int)strlen(GSTR(s2_p)) : cch2;
    int n = len1 < len2 ? len1 : len2;
    int icase = (flags & WIN_NORM_IGNORECASE) != 0;
    const u8 *s1 = (const u8 *)G(s1_p);
    const u8 *s2 = (const u8 *)G(s2_p);

    for (int i = 0; i < n; i++) {
        int ch1 = icase ? tolower(s1[i]) : s1[i];
        int ch2 = icase ? tolower(s2[i]) : s2[i];
        if (ch1 != ch2) return ch1 < ch2 ? 1 : 3;
    }
    if (len1 < len2) return 1;
    if (len1 > len2) return 3;
    return 2;
}

/* int CompareStringW(LCID Locale, DWORD dwCmpFlags, PCNZWCH lpString1,
                      int cchCount1, PCNZWCH lpString2, int cchCount2) */
static u32 imp_CompareStringW(Cpu *c)
{
    u32 loc = ARG(0);
    u32 flags = ARG(1);
    u32 s1_p = ARG(2);
    int cch1 = (int)ARG(3);
    u32 s2_p = ARG(4);
    int cch2 = (int)ARG(5);
    (void)loc;

    if (!s1_p || !s2_p) {
        last_error = 87;
        return 0;
    }
    int len1;
    if (cch1 == -1) {
        len1 = 0;
        while (rd16(s1_p + 2 * len1)) len1++;
    } else {
        len1 = cch1;
    }
    int len2;
    if (cch2 == -1) {
        len2 = 0;
        while (rd16(s2_p + 2 * len2)) len2++;
    } else {
        len2 = cch2;
    }
    int n = len1 < len2 ? len1 : len2;
    int icase = (flags & WIN_NORM_IGNORECASE) != 0;

    for (int i = 0; i < n; i++) {
        int ch1 = rd16(s1_p + 2 * i);
        int ch2 = rd16(s2_p + 2 * i);
        if (icase) {
            ch1 = tolower(ch1);
            ch2 = tolower(ch2);
        }
        if (ch1 != ch2) return ch1 < ch2 ? 1 : 3;
    }
    if (len1 < len2) return 1;
    if (len1 > len2) return 3;
    return 2;
}

static const char *get_locale_str(u32 type)
{
    switch (type) {
    case 0x0001: /* LOCALE_ILANGUAGE */ return "0409";
    case 0x0002: /* LOCALE_SLANGUAGE */ return "English (United States)";
    case 0x0003: /* LOCALE_SABBREVLCTYNAME */ return "USA";
    case 0x0004: /* LOCALE_SNATIVELANGNAME */ return "English";
    case 0x0005: /* LOCALE_ICOUNTRY */ return "1";
    case 0x0006: /* LOCALE_SCOUNTRY */ return "United States";
    case 0x0007: /* LOCALE_SABBREVCTRYNAME */ return "USA";
    case 0x0008: /* LOCALE_SNATIVECTRYNAME */ return "United States";
    case 0x0009: /* LOCALE_IDEFAULTLANGUAGE */ return "0409";
    case 0x000A: /* LOCALE_IDEFAULTCOUNTRY */ return "1";
    case 0x000B: /* LOCALE_IDEFAULTCODEPAGE */ return "1252";
    case 0x000C: /* LOCALE_SLIST */ return ",";
    case 0x000D: /* LOCALE_IMEASURE */ return "1";
    case 0x000E: /* LOCALE_SDECIMAL */ return ".";
    case 0x000F: /* LOCALE_STHOUSAND */ return ",";
    case 0x0010: /* LOCALE_SGROUPING */ return "3;0";
    case 0x0014: /* LOCALE_SCURRENCY */ return "$";
    case 0x0015: /* LOCALE_SINTLSYMBOL */ return "USD";
    case 0x001D: /* LOCALE_SDATE */ return "/";
    case 0x001E: /* LOCALE_STIME */ return ":";
    case 0x001F: /* LOCALE_SSHORTDATE */ return "M/d/yyyy";
    case 0x0020: /* LOCALE_SLONGDATE */ return "dddd, MMMM dd, yyyy";
    case 0x0028: /* LOCALE_S1159 */ return "AM";
    case 0x0029: /* LOCALE_S2359 */ return "PM";
    case 0x1001: /* LOCALE_SENGLANGUAGE */ return "English";
    case 0x1002: /* LOCALE_SENGCOUNTRY */ return "United States";
    case 0x1004: /* LOCALE_IDEFAULTANSICODEPAGE */ return "1252";
    case 0x1005: /* LOCALE_IDEFAULTOEMCODEPAGE */ return "437";
    case 0x1006: /* LOCALE_IDEFAULTMACCODEPAGE */ return "10000";
    default: return "";
    }
}

/* int GetLocaleInfoA(LCID Locale, LCTYPE LCType, LPSTR lpLCData, int cchData) */
static u32 imp_GetLocaleInfoA(Cpu *c)
{
    u32 loc = ARG(0);
    u32 lctype = ARG(1);
    u32 data_p = ARG(2);
    int cch = (int)ARG(3);
    (void)loc;

    u32 raw_type = lctype & 0xFFFFu;
    const char *s = get_locale_str(raw_type);
    int len = (int)strlen(s) + 1;

    if (lctype & 0x20000000u) { /* LOCALE_RETURN_NUMBER */
        if (!data_p || cch < 2) {
            last_error = 122;
            return 0;
        }
        wr32(data_p, (u32)atoi(s));
        return 2;
    }

    if (cch == 0) return (u32)len;
    if (cch < len) {
        last_error = 122;
        return 0;
    }
    memcpy(G(data_p), s, len);
    return (u32)len;
}

/* int GetLocaleInfoW(LCID Locale, LCTYPE LCType, LPWSTR lpLCData, int cchData) */
static u32 imp_GetLocaleInfoW(Cpu *c)
{
    u32 loc = ARG(0);
    u32 lctype = ARG(1);
    u32 data_p = ARG(2);
    int cch = (int)ARG(3);
    (void)loc;

    u32 raw_type = lctype & 0xFFFFu;
    const char *s = get_locale_str(raw_type);
    int len = (int)strlen(s) + 1;

    if (lctype & 0x20000000u) { /* LOCALE_RETURN_NUMBER */
        if (!data_p || cch < 2) {
            last_error = 122;
            return 0;
        }
        wr32(data_p, (u32)atoi(s));
        return 2;
    }

    if (cch == 0) return (u32)len;
    if (cch < len) {
        last_error = 122;
        return 0;
    }
    for (int i = 0; i < len; i++) {
        wr16(data_p + 2 * i, (u8)s[i]);
    }
    return (u32)len;
}

/* ---- Threads & Processes ------------------------------------------------------------- */

static __thread u32 my_thread_id;
static u32 get_my_thread_id(void)
{
    if (!my_thread_id) {
        static pthread_mutex_t tid_lock = PTHREAD_MUTEX_INITIALIZER;
        static u32 next_tid = 1001;
        pthread_mutex_lock(&tid_lock);
        my_thread_id = next_tid++;
        pthread_mutex_unlock(&tid_lock);
    }
    return my_thread_id;
}

/* HANDLE GetCurrentProcess(void) */
static u32 imp_GetCurrentProcess(Cpu *c)
{
    (void)c;
    return 0xffffffffu; /* (HANDLE)-1 */
}

/* DWORD GetCurrentThreadId(void) */
static u32 imp_GetCurrentThreadId(Cpu *c)
{
    (void)c;
    return get_my_thread_id();
}

/* HANDLE GetCurrentThread(void) */
static u32 imp_GetCurrentThread(Cpu *c)
{
    (void)c;
    return 0xfffffffeu; /* (HANDLE)-2 */
}

/* BOOL TerminateProcess(HANDLE hProcess, UINT uExitCode) */
static u32 imp_TerminateProcess(Cpu *c)
{
    u32 hproc = ARG(0);
    u32 code = ARG(1);
    plat_log("TerminateProcess(0x%x, %u)", hproc, code);
    exit((int)code);
    return 1;
}

/* VOID ExitProcess(UINT uExitCode) */
static u32 imp_ExitProcess(Cpu *c)
{
    u32 code = ARG(0);
    plat_log("ExitProcess(%u)", code);
    exit((int)code);
    return 0;
}

/* ---- Exceptions & Diagnostics -------------------------------------------------------- */

/* LONG UnhandledExceptionFilter(struct _EXCEPTION_POINTERS *ExceptionInfo) */
static u32 imp_UnhandledExceptionFilter(Cpu *c)
{
    plat_log("UnhandledExceptionFilter(0x%08x) -> abort", ARG(0));
    abort();
    return 0;
}

/* VOID RaiseException(DWORD dwExceptionCode, DWORD dwExceptionFlags,
                       DWORD nNumberOfArguments, const ULONG_PTR *lpArguments) */
static u32 imp_RaiseException(Cpu *c)
{
    plat_log("RaiseException(code=0x%08x, flags=0x%08x) -> abort", ARG(0), ARG(1));
    abort();
    return 0;
}

/* VOID RtlUnwind(PVOID TargetFrame, PVOID TargetIp,
                  PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue) */
static u32 imp_RtlUnwind(Cpu *c)
{
    u32 target_frame = ARG(0);
    u32 target_ip = ARG(1);
    u32 ex_rec = ARG(2);
    u32 ret_val = ARG(3);
    plat_log("RtlUnwind(target_frame=0x%x, target_ip=0x%x, ex_rec=0x%x, ret_val=0x%x)",
             target_frame, target_ip, ex_rec, ret_val);
    if (c->fs_base) {
        wr32(c->fs_base, target_frame ? target_frame : 0xffffffffu);
    }
    return 0;
}

static u32 process_error_mode;

/* UINT SetErrorMode(UINT uMode) */
static u32 imp_SetErrorMode(Cpu *c)
{
    u32 mode = ARG(0);
    u32 prev = process_error_mode;
    process_error_mode = mode;
    return prev;
}

/* VOID OutputDebugStringA(LPCSTR lpOutputString) */
static u32 imp_OutputDebugStringA(Cpu *c)
{
    u32 p = ARG(0);
    if (p) plat_log("[OutputDebugString] %s", GSTR(p));
    return 0;
}

/* VOID DebugBreak(void) */
static u32 imp_DebugBreak(Cpu *c)
{
    (void)c;
    plat_log("DebugBreak() -> abort");
    abort();
    return 0;
}

/* ---- System, Drives & Environment Info ----------------------------------------------- */

/* BOOL GetComputerNameA(LPSTR lpBuffer, LPDWORD nSize) */
static u32 imp_GetComputerNameA(Cpu *c)
{
    u32 buf_p = ARG(0);
    u32 size_p = ARG(1);
    if (!size_p) {
        last_error = 87;
        return 0;
    }
    const char *name = "FUTURECOP";
    u32 len = (u32)strlen(name);
    u32 max_len = rd32(size_p);
    if (max_len <= len) {
        wr32(size_p, len + 1);
        last_error = 111; /* ERROR_BUFFER_OVERFLOW */
        return 0;
    }
    if (buf_p) {
        memcpy(G(buf_p), name, len + 1);
        wr32(size_p, len);
    }
    return 1;
}

/* int lstrlenA(LPCSTR lpString) */
static u32 imp_lstrlenA(Cpu *c)
{
    u32 p = ARG(0);
    return p ? (u32)strlen(GSTR(p)) : 0;
}

/* LPSTR lstrcpyA(LPSTR lpString1, LPCSTR lpString2) */
static u32 imp_lstrcpyA(Cpu *c)
{
    u32 dst = ARG(0);
    u32 src = ARG(1);
    if (dst && src) strcpy((char *)G(dst), GSTR(src));
    return dst;
}

/* BOOL Beep(DWORD dwFreq, DWORD dwDuration) */
static u32 imp_Beep(Cpu *c)
{
    u32 freq = ARG(0);
    u32 dur = ARG(1);
    (void)freq; (void)dur;
    return 1;
}

/* DWORD GetLogicalDrives(void) */
static u32 imp_GetLogicalDrives(Cpu *c)
{
    (void)c;
    /* Drives C: (bit 2) and D: (bit 3) -> 0x0000000C */
    return (1u << 2) | (1u << 3);
}

/* UINT GetDriveTypeA(LPCSTR lpRootPathName) */
static u32 imp_GetDriveTypeA(Cpu *c)
{
    u32 p = ARG(0);
    if (!p) return 3; /* DRIVE_FIXED (C:) */
    const char *path = GSTR(p);
    if (path[0] == 'D' || path[0] == 'd')
        return 5; /* DRIVE_CDROM */
    if (path[0] == 'C' || path[0] == 'c')
        return 3; /* DRIVE_FIXED */
    return 1; /* DRIVE_NO_ROOT_DIR */
}

/* BOOL GetVolumeInformationA(LPCSTR lpRootPathName, LPSTR lpVolumeNameBuffer,
                              DWORD nVolumeNameSize, LPDWORD lpVolumeSerialNumber,
                              LPDWORD lpMaximumComponentLength, LPDWORD lpFileSystemFlags,
                              LPSTR lpFileSystemNameBuffer, DWORD nFileSystemNameSize) */
static u32 imp_GetVolumeInformationA(Cpu *c)
{
    u32 root_p = ARG(0);
    u32 vol_name_p = ARG(1);
    u32 vol_name_size = ARG(2);
    u32 serial_p = ARG(3);
    u32 max_comp_p = ARG(4);
    u32 flags_p = ARG(5);
    u32 fs_name_p = ARG(6);
    u32 fs_name_size = ARG(7);

    int is_cdrom = 0;
    if (root_p) {
        const char *root = GSTR(root_p);
        if (root[0] == 'D' || root[0] == 'd') is_cdrom = 1;
    }

    const char *vol_name = is_cdrom ? "FCOP_CD" : "HARDDRIVE";  /* the label the game checks */
    const char *fs_name = is_cdrom ? "CDFS" : "FAT32";
    u32 serial_num = is_cdrom ? 0x19980815u : 0x12345678u;

    if (vol_name_p && vol_name_size)
        gstrcpy(vol_name_p, vol_name, vol_name_size);
    if (serial_p)
        wr32(serial_p, serial_num);
    if (max_comp_p)
        wr32(max_comp_p, 255);
    if (flags_p)
        wr32(flags_p, 0x00000006u); /* FS_CASE_IS_PRESERVED | FS_UNICODE_STORED_ON_DISK */
    if (fs_name_p && fs_name_size)
        gstrcpy(fs_name_p, fs_name, fs_name_size);

    return 1;
}

/* VOID GetSystemInfo(LPSYSTEM_INFO lpSystemInfo) */
static u32 imp_GetSystemInfo(Cpu *c)
{
    u32 p = ARG(0);
    if (p) {
        memset(G(p), 0, 36);
        wr16(p + 0x00, 0);          /* PROCESSOR_ARCHITECTURE_INTEL */
        wr32(p + 0x04, 4096);       /* dwPageSize */
        wr32(p + 0x08, 0x00010000); /* lpMinimumApplicationAddress */
        wr32(p + 0x0C, 0x7FFEFFFF); /* lpMaximumApplicationAddress */
        wr32(p + 0x10, 1);          /* dwActiveProcessorMask */
        wr32(p + 0x14, 1);          /* dwNumberOfProcessors */
        wr32(p + 0x18, 586);        /* dwProcessorType: PROCESSOR_INTEL_PENTIUM */
        wr32(p + 0x1C, 65536);      /* dwAllocationGranularity */
        wr16(p + 0x20, 6);          /* wProcessorLevel (Pentium II) */
        wr16(p + 0x22, 0);          /* wProcessorRevision */
    }
    return 0;
}

/* DWORD GetLastError(void) */
static u32 imp_GetLastError(Cpu *c)
{
    (void)c;
    return last_error;
}

/* VOID SetLastError(DWORD dwErrCode) */
static u32 imp_SetLastError(Cpu *c)
{
    last_error = ARG(0);
    return 0;
}

/* ---- Import Table -------------------------------------------------------------------- */

const struct ImpDef imp_k32_proc[] = {
    IMPORT(GetVersion, 0),
    IMPORT(GetCommandLineA, 0),
    IMPORT(GetStartupInfoA, 1),
    IMPORT(GetModuleFileNameA, 3),
    IMPORT(GetModuleHandleA, 1),
    IMPORT(GetEnvironmentStrings, 0),
    {"GetEnvironmentStringsA", 0, imp_GetEnvironmentStrings},
    IMPORT(GetEnvironmentStringsW, 0),
    IMPORT(FreeEnvironmentStringsA, 1),
    IMPORT(FreeEnvironmentStringsW, 1),
    IMPORT(SetEnvironmentVariableA, 2),
    IMPORT(SetEnvironmentVariableW, 2),
    IMPORT(GetStdHandle, 1),
    IMPORT(SetStdHandle, 2),
    IMPORT(GetFileType, 1),
    IMPORT(SetHandleCount, 1),
    IMPORT(GetACP, 0),
    IMPORT(GetOEMCP, 0),
    IMPORT(GetCPInfo, 2),
    IMPORT(MultiByteToWideChar, 6),
    IMPORT(WideCharToMultiByte, 8),
    IMPORT(LCMapStringA, 6),
    IMPORT(LCMapStringW, 6),
    IMPORT(GetStringTypeA, 5),
    IMPORT(GetStringTypeW, 4),
    IMPORT(CompareStringA, 6),
    IMPORT(CompareStringW, 6),
    IMPORT(GetLocaleInfoA, 4),
    IMPORT(GetLocaleInfoW, 4),
    IMPORT(GetCurrentProcess, 0),
    IMPORT(GetCurrentThreadId, 0),
    IMPORT(GetCurrentThread, 0),
    IMPORT(TerminateProcess, 2),
    IMPORT(ExitProcess, 1),
    IMPORT(UnhandledExceptionFilter, 1),
    IMPORT(RaiseException, 4),
    IMPORT(RtlUnwind, 4),
    IMPORT(SetErrorMode, 1),
    IMPORT(OutputDebugStringA, 1),
    IMPORT(DebugBreak, 0),
    IMPORT(LoadLibraryA, 1),
    IMPORT(GetProcAddress, 2),
    IMPORT(FreeLibrary, 1),
    IMPORT(GetComputerNameA, 2),
    IMPORT(lstrlenA, 1),
    IMPORT(lstrcpyA, 2),
    IMPORT(Beep, 2),
    IMPORT(GetVolumeInformationA, 8),
    IMPORT(GetDriveTypeA, 1),
    IMPORT(GetLogicalDrives, 0),
    IMPORT(GetSystemInfo, 1),
    IMPORT(GetLastError, 0),
    IMPORT(SetLastError, 1),
};
IMPORT_TABLE(k32_proc);
