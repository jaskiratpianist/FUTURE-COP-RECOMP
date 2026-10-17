#!/usr/bin/env python3
"""Build the native game and run it briefly: the one command platform workers need.

    .venv/bin/python -I recomp/plat_test.py            # build, run 15 s, show the end of the log
    .venv/bin/python -I recomp/plat_test.py --api      # also log every Windows call
    .venv/bin/python -I recomp/plat_test.py --seconds 40
"""
import argparse, fcntl, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "recomp"))
from paths import game_dir  # noqa: E402
ap = argparse.ArgumentParser()
ap.add_argument("--seconds", type=int, default=15)
ap.add_argument("--api", action="store_true", help="log every import call (FCOP_TRACE_API)")
ap.add_argument("--lines", type=int, default=40)
a = ap.parse_args()
lock = open(os.path.join(ROOT, "build", "plat_test.lock"), "w")
fcntl.flock(lock, fcntl.LOCK_EX)  # several workers share one build directory: take turns
b = subprocess.run([os.path.join(ROOT, "recomp", "build.sh")], cwd=ROOT, capture_output=True, text=True)
for l in (b.stdout + b.stderr).splitlines():
    if l.startswith("COMPILE ERROR"):
        print("\n".join(x for x in (b.stdout + b.stderr).splitlines() if "error" in x.lower())[:4000])
        break
if b.returncode:
    errs = [l for l in (b.stdout + b.stderr).splitlines() if "error" in l or "undefined reference" in l]
    print("BUILD FAILED\n" + "\n".join((errs or (b.stdout + b.stderr).splitlines())[:a.lines]))
    sys.exit(1)
env = dict(os.environ, FCOP_TRACE_API="1") if a.api else dict(os.environ)
try:
    r = subprocess.run([os.path.join(ROOT, "build", "recomp", "fcop"), game_dir()],
                       cwd=ROOT, capture_output=True, text=True, timeout=a.seconds, env=env)
    out, status = r.stdout + r.stderr, f"exited with {r.returncode}"
except subprocess.TimeoutExpired as e:
    out = (e.stdout or b"").decode(errors="replace") + (e.stderr or b"").decode(errors="replace")
    status = f"still running after {a.seconds} s (killed)"
lines = out.splitlines()
print(f"BUILD OK; game {status}; last {min(a.lines, len(lines))} of {len(lines)} log lines:")
print("\n".join(lines[-a.lines:]))
