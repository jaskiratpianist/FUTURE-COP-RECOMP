/* The few things the port needs from the host operating system that differ between Linux and
 * Windows (host.h "os_"). windows.h is included only here: it defines hundreds of names (RECT,
 * CreateFileA, min/max...) that clash with the Win32 the other files emulate for the game. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <direct.h>
#include <shellapi.h>
#else
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#endif

/* reserve address space (no memory yet); NULL on failure */
void *os_reserve(u64 size)
{
#ifdef _WIN32
    return VirtualAlloc(NULL, (SIZE_T)size, MEM_RESERVE, PAGE_NOACCESS);
#else
    void *p = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}

/* make reserved space usable, zero-filled; 0 on success. Linux backs the pages only when they are
   touched; Windows counts committed memory against the page file, so callers commit as they go. */
int os_commit(void *p, u64 size)
{
#ifdef _WIN32
    return VirtualAlloc(p, (SIZE_T)size, MEM_COMMIT, PAGE_READWRITE) ? 0 : -1;
#else
    return mprotect(p, size, PROT_READ | PROT_WRITE);
#endif
}

/* create a folder; 0 if it exists afterwards */
int os_mkdir(const char *path)
{
#ifdef _WIN32
    if (CreateDirectoryA(path, NULL)) return 0;
    return GetLastError() == ERROR_ALREADY_EXISTS ? 0 : -1;
#else
    return mkdir(path, 0755) && errno != EEXIST ? -1 : 0;
#endif
}

/* The log goes to stderr. A Windows program started from Explorer has none: then it goes to
   fcop.log in the settings folder. */
void os_log_init(const char *user_dir)
{
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (!h || h == INVALID_HANDLE_VALUE || GetFileType(h) == FILE_TYPE_UNKNOWN) {
        char path[1200];
        snprintf(path, sizeof path, "%s/fcop.log", user_dir);
        if (freopen(path, "w", stderr)) setvbuf(stderr, NULL, _IONBF, 0);
    }
#else
    (void)user_dir;
#endif
}

/* the desktop's file chooser for the CD image: 1 chosen, 0 cancelled, -1 none available */
int os_pick_image(char *out, size_t n)
{
#ifdef _WIN32
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    out[0] = 0;
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFilter = "CD images (*.iso, *.cue, *.bin, *.img)\0*.iso;*.cue;*.bin;*.img\0All files\0*.*\0";
    ofn.lpstrFile = out;
    ofn.nMaxFile = (DWORD)n;
    ofn.lpstrTitle = "Choose your Future Cop CD image";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    return GetOpenFileNameA(&ofn) && out[0] ? 1 : 0;
#else
    static const char *const cmds[] = {
        "zenity --file-selection --title='Choose your Future Cop CD image' "
        "--file-filter='CD images | *.iso *.ISO *.cue *.CUE *.bin *.BIN *.img *.IMG' 2>/dev/null",
        "kdialog --title 'Choose your Future Cop CD image' --getopenfilename . "
        "'*.iso *.ISO *.cue *.CUE *.bin *.BIN *.img *.IMG' 2>/dev/null",
    };
    for (unsigned k = 0; k < sizeof cmds / sizeof cmds[0]; k++) {
        FILE *p = popen(cmds[k], "r");
        int status;
        if (!p) continue;
        if (!fgets(out, (int)n, p)) out[0] = 0;
        status = pclose(p);
        out[strcspn(out, "\n")] = 0;
        if (WIFEXITED(status) && WEXITSTATUS(status) == 127) continue;  /* not installed */
        return out[0] ? 1 : 0;  /* chosen, or cancelled */
    }
    return -1;
#endif
}

/* show a folder in the file manager; 0 on success */
int os_open_folder(const char *dir)
{
#ifdef _WIN32
    return (INT_PTR)ShellExecuteA(NULL, "open", dir, NULL, NULL, SW_SHOWNORMAL) > 32 ? 0 : -1;
#else
    char cmd[1200];
    if (strchr(dir, '\'')) return -1;
    snprintf(cmd, sizeof cmd, "xdg-open '%s' >/dev/null 2>&1 &", dir);
    return system(cmd) == 0 ? 0 : -1;
#endif
}
