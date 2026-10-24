/* Windows host: the few POSIX calls MinGW lacks (os_win.c). Kept apart from <windows.h>,
 * whose names clash with the Win32 definitions in host.h. */
#ifndef OS_WIN_H
#define OS_WIN_H
#include <stddef.h>

void *os_reserve(size_t n);         /* reserve address space, no access */
int os_commit(void *p, size_t n);   /* make it readable and writable; 0 on failure */
int setenv(const char *name, const char *val, int overwrite);
int unsetenv(const char *name);
int fsync(int fd);
#endif
