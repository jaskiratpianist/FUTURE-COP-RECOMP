#!/usr/bin/env python3
"""Report for recomp/prof/sampler.c (perf is blocked on this machine).

  gcc -O2 -shared -fPIC -o build/sampler.so recomp/prof/sampler.c
  DEBUG=1 OPT=-O2 OUT=build/fcop_prof ./recomp/build.sh      # optimised, with symbols
  PROF_OUT=build/samples.bin LD_PRELOAD=build/sampler.so SDL_VIDEODRIVER=offscreen ... build/fcop_prof game_files/FCopLAPD
  recomp/prof/report.py build/samples.bin                     # time per function
  recomp/prof/report.py build/samples.bin F_45b6db            # time per generated line of one function
"""
import bisect, collections, struct, subprocess, sys

EXE = "build/fcop_prof"
data = open(sys.argv[1], "rb").read()
pcs = struct.unpack("<%dQ" % (len(data) // 8), data)
syms = []
for line in subprocess.run(["nm", "--defined-only", EXE], capture_output=True, text=True).stdout.split("\n"):
    f = line.split()
    if len(f) == 3 and f[1] in "tTwW":
        syms.append((int(f[0], 16), f[2]))
syms.sort()
addrs = [a for a, _ in syms]

def name(pc):
    if pc >> 63:
        return "<outside the executable: libc, GPU driver>"
    i = bisect.bisect_right(addrs, pc) - 1
    return syms[i][1] if i >= 0 else "?"

if len(sys.argv) < 3:
    c = collections.Counter(name(pc) for pc in pcs)
    for n, k in c.most_common(30):
        print(f"{100 * k / len(pcs):5.1f}%  {n}")
    print(len(pcs), "samples")
    sys.exit()
sel = collections.Counter(pc for pc in pcs if name(pc) == sys.argv[2])
tot = sum(sel.values()) or 1
pcl = list(sel)
out = subprocess.run(["addr2line", "-e", EXE] + [hex(p) for p in pcl], capture_output=True, text=True).stdout.split("\n")
byline = collections.Counter()
for p, l in zip(pcl, out):
    byline[l.split(" (")[0]] += sel[p]
files = {}
for l, k in byline.most_common(25):
    f, _, n = l.rpartition(":")
    try:
        files.setdefault(f, open(f).read().split("\n"))
        txt = files[f][int(n) - 1].strip()[:140]
    except (OSError, ValueError, IndexError):
        txt = ""
    print(f"{100 * k / tot:5.1f}% {l.split('/')[-1]:16s} {txt}")
