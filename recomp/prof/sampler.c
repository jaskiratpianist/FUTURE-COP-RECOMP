/* Sampling profiler for the game (perf is blocked on this machine), loaded with LD_PRELOAD:
   SIGPROF every 0.5 ms of CPU time records the program counter (offset into the executable, or the
   raw address with the top bit set when elsewhere) to $PROF_OUT. Usage: recomp/prof/report.py. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>
#include <fcntl.h>
static unsigned long lo, hi;
static unsigned long buf[4096];
static volatile int n;
static int fd = -1, flushes;
static char mapsname[600];
static void h(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)si;
    unsigned long pc = ((ucontext_t *)uc)->uc_mcontext.gregs[REG_RIP];
    int i = __sync_fetch_and_add(&n, 1);
    if (i >= 4096) return;
    buf[i] = (pc >= lo && pc < hi) ? pc - lo : (pc | 1ul << 63);
    if (i == 4095) {
        (void)!write(fd, buf, sizeof buf); n = 0;
        if (++flushes == 8) {  /* libraries are loaded by now: save the memory map */
            char b[4096]; ssize_t r; int in = open("/proc/self/maps", O_RDONLY), o = open(mapsname, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            while ((r = read(in, b, sizeof b)) > 0) (void)!write(o, b, r);
            close(in); close(o);
        }
    }
}
__attribute__((constructor)) static void init(void)
{
    const char *out = getenv("PROF_OUT");
    if (!out) return;
    char line[512], exe[512] = "";
    ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (k > 0) exe[k] = 0;
    FILE *m = fopen("/proc/self/maps", "r");
    while (m && fgets(line, sizeof line, m)) {
        unsigned long a, b;
        if (strstr(line, exe) && sscanf(line, "%lx-%lx", &a, &b) == 2) { if (!lo) lo = a; hi = b; }
    }
    if (m) fclose(m);
    snprintf(mapsname, sizeof mapsname, "%s.maps", out);
    fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    struct sigaction sa = {0};
    sa.sa_sigaction = h; sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, 0);
    struct itimerval it = {{0, 500}, {0, 500}};
    setitimer(ITIMER_PROF, &it, 0);
}
