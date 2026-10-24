/* Windows host: the few POSIX calls MinGW lacks. Empty on other hosts. */
#ifdef _WIN32
#include <io.h>
#include <stdlib.h>
#include <windows.h>
#include "os_win.h"

void *os_reserve(size_t n) { return VirtualAlloc(NULL, n, MEM_RESERVE, PAGE_NOACCESS); }

int os_commit(void *p, size_t n) { return VirtualAlloc(p, n, MEM_COMMIT, PAGE_READWRITE) != NULL; }

int setenv(const char *name, const char *val, int overwrite)
{
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, val);
}

int unsetenv(const char *name) { return _putenv_s(name, ""); }

int fsync(int fd) { return _commit(fd); }
#endif
