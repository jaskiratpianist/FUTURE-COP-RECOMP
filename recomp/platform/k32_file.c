/* KERNEL32.dll: k32_file - Win32 file operations */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "host.h"

#define GENERIC_READ    0x80000000u
#define GENERIC_WRITE   0x40000000u
#define GENERIC_EXECUTE 0x20000000u
#define GENERIC_ALL     0x10000000u

#define CREATE_NEW        1u
#define CREATE_ALWAYS     2u
#define OPEN_EXISTING     3u
#define OPEN_ALWAYS       4u
#define TRUNCATE_EXISTING 5u

#define INVALID_HANDLE_VALUE ((u32)0xffffffffu)

#define ERROR_SUCCESS           0u
#define ERROR_FILE_NOT_FOUND    2u
#define ERROR_PATH_NOT_FOUND    3u
#define ERROR_ACCESS_DENIED     5u
#define ERROR_INVALID_HANDLE    6u
#define ERROR_WRITE_FAULT       29u
#define ERROR_READ_FAULT        30u
#define ERROR_FILE_EXISTS       80u
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_NEGATIVE_SEEK     131u
#define ERROR_ALREADY_EXISTS    183u

/* HANDLE CreateFileA(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE) */
static u32 imp_CreateFileA(Cpu *c)
{
    u32 p_name = ARG(0);
    u32 access_flags = ARG(1);
    /* ARG(2): dwShareMode */
    /* ARG(3): lpSecurityAttributes */
    u32 disp = ARG(4);
    /* ARG(5): dwFlagsAndAttributes */
    /* ARG(6): hTemplateFile */

    if (!p_name) {
        last_error = ERROR_PATH_NOT_FOUND;
        return INVALID_HANDLE_VALUE;
    }
    const char *gpath = GSTR(p_name);
    if (!gpath || !gpath[0]) {
        last_error = ERROR_PATH_NOT_FOUND;
        return INVALID_HANDLE_VALUE;
    }

    if (!strcasecmp(gpath, "CONOUT$") || !strcasecmp(gpath, "CON")) {
        last_error = ERROR_SUCCESS;
        return HSTD_OUTPUT;
    }
    if (!strcasecmp(gpath, "CONIN$")) {
        last_error = ERROR_SUCCESS;
        return HSTD_INPUT;
    }

    char hpath[1024];
    host_path(gpath, hpath, sizeof(hpath));

    int can_read = (access_flags & GENERIC_READ) || (access_flags & GENERIC_ALL) || (access_flags & 0x0001u);
    int can_write = (access_flags & GENERIC_WRITE) || (access_flags & GENERIC_ALL) || (access_flags & 0x0006u);
    int oflags = 0;
    if (can_read && can_write) {
        oflags = O_RDWR;
    } else if (can_write) {
        oflags = O_WRONLY;
    } else {
        oflags = O_RDONLY;
    }

    int existed = (access(hpath, F_OK) == 0);

    switch (disp) {
    case CREATE_NEW:
        oflags |= O_CREAT | O_EXCL;
        break;
    case CREATE_ALWAYS:
        oflags |= O_CREAT | O_TRUNC;
        break;
    case OPEN_EXISTING:
        /* Neither O_CREAT nor O_TRUNC */
        break;
    case OPEN_ALWAYS:
        oflags |= O_CREAT;
        break;
    case TRUNCATE_EXISTING:
        oflags |= O_TRUNC;
        break;
    default:
        plat_log("warn: CreateFileA unknown disposition %u for %s", disp, gpath);
        break;
    }

    int fd = open(hpath, oflags, 0666);
    if (fd < 0) {
        if (errno == ENOENT) {
            char dir[1024];
            strncpy(dir, hpath, sizeof(dir) - 1);
            dir[sizeof(dir) - 1] = '\0';
            char *slash = strrchr(dir, '/');
            if (slash && slash != dir) {
                *slash = '\0';
                if (access(dir, F_OK) != 0) {
                    last_error = ERROR_PATH_NOT_FOUND;
                } else {
                    last_error = ERROR_FILE_NOT_FOUND;
                }
            } else {
                last_error = ERROR_FILE_NOT_FOUND;
            }
        } else if (errno == EEXIST) {
            last_error = ERROR_FILE_EXISTS;
        } else if (errno == EACCES || errno == EPERM || errno == EISDIR) {
            last_error = ERROR_ACCESS_DENIED;
        } else {
            last_error = ERROR_FILE_NOT_FOUND;
        }
        return INVALID_HANDLE_VALUE;
    }

    if (fd == 0) {
        int fd2 = fcntl(0, F_DUPFD, 3);
        close(0);
        fd = fd2;
    }

    if (disp == CREATE_ALWAYS || disp == OPEN_ALWAYS) {
        last_error = existed ? ERROR_ALREADY_EXISTS : ERROR_SUCCESS;
    } else {
        last_error = ERROR_SUCCESS;
    }

    return handle_new(H_FILE, (void *)(intptr_t)fd);
}

/* BOOL ReadFile(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED) */
static u32 imp_ReadFile(Cpu *c)
{
    u32 hFile = ARG(0);
    u32 lpBuffer = ARG(1);
    u32 nToRead = ARG(2);
    u32 lpRead = ARG(3);

    int fd = -1;
    if (hFile == HSTD_INPUT) {
        fd = STDIN_FILENO;
    } else {
        void *p = handle_get(hFile, H_FILE);
        if (!p) {
            last_error = ERROR_INVALID_HANDLE;
            if (lpRead) wr32(lpRead, 0);
            return 0;
        }
        fd = (int)(intptr_t)p;
    }

    if (nToRead == 0) {
        if (lpRead) wr32(lpRead, 0);
        last_error = ERROR_SUCCESS;
        return 1;
    }

    if (!lpBuffer) {
        last_error = ERROR_INVALID_PARAMETER;
        if (lpRead) wr32(lpRead, 0);
        return 0;
    }

    u32 total = 0;
    while (total < nToRead) {
        ssize_t n = read(fd, (char *)G(lpBuffer) + total, nToRead - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            last_error = ERROR_READ_FAULT;
            if (lpRead) wr32(lpRead, total);
            return (total > 0) ? 1 : 0;
        }
        if (n == 0) {
            /* EOF reached */
            break;
        }
        total += (u32)n;
    }

    if (lpRead) {
        wr32(lpRead, total);
    }
    last_error = ERROR_SUCCESS;
    return 1;
}

/* BOOL WriteFile(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED) */
static u32 imp_WriteFile(Cpu *c)
{
    u32 hFile = ARG(0);
    u32 lpBuffer = ARG(1);
    u32 nToWrite = ARG(2);
    u32 lpWritten = ARG(3);

    int fd = -1;
    if (hFile == HSTD_OUTPUT) {
        fd = STDOUT_FILENO;
    } else if (hFile == HSTD_ERROR) {
        fd = STDERR_FILENO;
    } else {
        void *p = handle_get(hFile, H_FILE);
        if (!p) {
            last_error = ERROR_INVALID_HANDLE;
            if (lpWritten) wr32(lpWritten, 0);
            return 0;
        }
        fd = (int)(intptr_t)p;
    }

    if (nToWrite == 0) {
        if (lpWritten) wr32(lpWritten, 0);
        last_error = ERROR_SUCCESS;
        return 1;
    }

    if (!lpBuffer) {
        last_error = ERROR_INVALID_PARAMETER;
        if (lpWritten) wr32(lpWritten, 0);
        return 0;
    }

    u32 total = 0;
    while (total < nToWrite) {
        ssize_t n = write(fd, (const char *)G(lpBuffer) + total, nToWrite - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            last_error = ERROR_WRITE_FAULT;
            if (lpWritten) wr32(lpWritten, total);
            return (total > 0) ? 1 : 0;
        }
        if (n == 0) break;
        total += (u32)n;
    }

    if (hFile == HSTD_OUTPUT) fflush(stdout);
    else if (hFile == HSTD_ERROR) fflush(stderr);

    if (lpWritten) {
        wr32(lpWritten, total);
    }
    last_error = ERROR_SUCCESS;
    return 1;
}

/* DWORD SetFilePointer(HANDLE, LONG, PLONG, DWORD) */
static u32 imp_SetFilePointer(Cpu *c)
{
    u32 hFile = ARG(0);
    u32 low_dist = ARG(1);
    u32 p_high = ARG(2);
    u32 method = ARG(3);

    void *p = handle_get(hFile, H_FILE);
    if (!p) {
        last_error = ERROR_INVALID_HANDLE;
        return (u32)-1;
    }
    int fd = (int)(intptr_t)p;

    s64 dist;
    if (p_high) {
        s32 high = (s32)rd32(p_high);
        dist = (s64)(((u64)(u32)high << 32) | (u64)low_dist);
    } else {
        dist = (s32)low_dist;
    }

    int whence;
    switch (method) {
    case 0: whence = SEEK_SET; break;
    case 1: whence = SEEK_CUR; break;
    case 2: whence = SEEK_END; break;
    default:
        plat_log("warn: SetFilePointer unknown method %u", method);
        last_error = ERROR_INVALID_PARAMETER;
        return (u32)-1;
    }

    off_t new_pos = lseek(fd, (off_t)dist, whence);
    if (new_pos == (off_t)-1) {
        if (errno == EINVAL) last_error = ERROR_NEGATIVE_SEEK;
        else last_error = ERROR_INVALID_PARAMETER;
        return (u32)-1;
    }

    if (p_high) {
        wr32(p_high, (u32)((u64)new_pos >> 32));
    }
    last_error = ERROR_SUCCESS;
    return (u32)new_pos;
}

/* BOOL SetEndOfFile(HANDLE) */
static u32 imp_SetEndOfFile(Cpu *c)
{
    u32 hFile = ARG(0);
    void *p = handle_get(hFile, H_FILE);
    if (!p) {
        last_error = ERROR_INVALID_HANDLE;
        return 0;
    }
    int fd = (int)(intptr_t)p;

    off_t cur = lseek(fd, 0, SEEK_CUR);
    if (cur == (off_t)-1) {
        last_error = ERROR_INVALID_HANDLE;
        return 0;
    }

    if (ftruncate(fd, cur) != 0) {
        last_error = ERROR_ACCESS_DENIED;
        return 0;
    }

    last_error = ERROR_SUCCESS;
    return 1;
}

/* BOOL FlushFileBuffers(HANDLE) */
static u32 imp_FlushFileBuffers(Cpu *c)
{
    u32 hFile = ARG(0);
    if (hFile == HSTD_OUTPUT) {
        fflush(stdout);
        last_error = ERROR_SUCCESS;
        return 1;
    }
    if (hFile == HSTD_ERROR) {
        fflush(stderr);
        last_error = ERROR_SUCCESS;
        return 1;
    }

    void *p = handle_get(hFile, H_FILE);
    if (!p) {
        last_error = ERROR_INVALID_HANDLE;
        return 0;
    }
    int fd = (int)(intptr_t)p;

    if (fsync(fd) != 0) {
        if (errno != EINVAL && errno != EROFS) {
            last_error = ERROR_ACCESS_DENIED;
            return 0;
        }
    }

    last_error = ERROR_SUCCESS;
    return 1;
}

/* BOOL CloseHandle(HANDLE) */
static u32 imp_CloseHandle(Cpu *c)
{
    u32 h = ARG(0);
    if (h == HSTD_INPUT || h == HSTD_OUTPUT || h == HSTD_ERROR) {
        last_error = ERROR_SUCCESS;
        return 1;
    }
    if (h == 0 || h == (u32)-1 || (h % 4) != 0 || h < 64 || h >= 4096 * 4) {
        last_error = ERROR_INVALID_HANDLE;
        return 0;
    }

    void *p = handle_get(h, H_FILE);
    if (p) {
        int fd = (int)(intptr_t)p;
        close(fd);
    }
    handle_free(h);
    last_error = ERROR_SUCCESS;
    return 1;
}

const struct ImpDef imp_k32_file[] = {
    IMPORT(CreateFileA, 7),
    IMPORT(ReadFile, 5),
    IMPORT(WriteFile, 5),
    IMPORT(SetFilePointer, 4),
    IMPORT(SetEndOfFile, 1),
    IMPORT(FlushFileBuffers, 1),
    IMPORT(CloseHandle, 1),
};
IMPORT_TABLE(k32_file);
