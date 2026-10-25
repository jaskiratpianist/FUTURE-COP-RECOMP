#!/usr/bin/env python3
"""Summarise an FCOP_PROF profile (Windows): self and inclusive time per function.

usage: prof_report.py [build/prof.txt] [--exe build/recomp/fcop.exe] [--top 40]
Functions in fcop.exe are named from its symbol table (nm); other modules by DLL name.
"""
import argparse, bisect, collections, os, re, subprocess

ap = argparse.ArgumentParser()
ap.add_argument("prof", nargs="?", default="build/prof.txt")
ap.add_argument("--exe", default="build/recomp/fcop.exe")
ap.add_argument("--top", type=int, default=40)
a = ap.parse_args()

mods, samples = [], []
for line in open(a.prof):
    if line.startswith("M "):
        _, base, size, name = line.rstrip("\n").split(" ", 3)
        mods.append((int(base, 16), int(size, 16), name))
    elif line.startswith("S"):
        samples.append([int(x, 16) for x in line.split()[1:]])
mods.sort()

out = subprocess.run(["objdump", "-p", a.exe], capture_output=True, text=True).stdout
link_base = int(re.search(r"ImageBase\s+([0-9a-fA-F]+)", out).group(1), 16)
syms = []
for line in subprocess.run(["nm", "-n", a.exe], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 3 and p[1] in "tT":
        syms.append((int(p[0], 16) - link_base, p[2]))
addrs = [s[0] for s in syms]
exe_name = os.path.basename(a.exe).lower()

cache = {}
def name(pc):
    if pc in cache:
        return cache[pc]
    r = "?"
    for base, size, m in mods:
        if base <= pc < base + size:
            mb = os.path.basename(m)
            if mb.lower() == exe_name:
                k = bisect.bisect_right(addrs, pc - base) - 1
                r = syms[k][1] if k >= 0 else "?exe"
            else:
                r = "[" + mb + "]"
            break
    cache[pc] = r
    return r

self_c, incl_c = collections.Counter(), collections.Counter()
for s in samples:
    names = [name(pc) for pc in s]
    if names:
        self_c[names[0]] += 1
    for n in set(names):
        incl_c[n] += 1

n = len(samples) or 1
def show(title, c):
    print(f"\n{title} ({len(samples)} samples)")
    for k, v in c.most_common(a.top):
        print(f"{100.0 * v / n:6.1f}%  {v:6d}  {k}")
show("self", self_c)
show("inclusive", incl_c)

# guest code (F_/G_ recompiled functions) vs. everything else, by the innermost frame
guest = sum(v for k, v in self_c.items() if re.match(r"[FG]_[0-9a-f]+", k))
print(f"\nguest code self: {100.0 * guest / n:.1f}%")
