#!/usr/bin/env python3
"""Build the native game and run it briefly: the one command platform workers need.

    .venv/bin/python -I recomp/plat_test.py            # build, run 15 s, show the end of the log
    .venv/bin/python -I recomp/plat_test.py --api      # also log every Windows call
    .venv/bin/python -I recomp/plat_test.py --seconds 40
    .venv/bin/python -I recomp/plat_test.py --input "9000:click@156,306" --png build/shots/quit.png
        (scripted input, see FCOP_INPUT in CLAUDE.md; --png saves the last frame shown)
"""
import argparse, fcntl, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "recomp"))
from paths import game_dir  # noqa: E402
ap = argparse.ArgumentParser()
ap.add_argument("--seconds", type=int, default=15)
ap.add_argument("--api", action="store_true", help="log every import call (FCOP_TRACE_API)")
ap.add_argument("--lines", type=int, default=40)
ap.add_argument("--input", help="scripted input (FCOP_INPUT), e.g. \"9000:click@156,306 12000:Escape\"")
ap.add_argument("--png", help="save the last frame shown as this PNG (needs ffmpeg)")
ap.add_argument("--show", action="store_true", help="show the window (default: headless, nothing appears on screen)")
ap.add_argument("--audio", help="record the game's sound output to this raw file (SDL disk audio driver) "
                "and print its level per second")
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
if a.input:
    env["FCOP_INPUT"] = a.input
if not a.show:
    env["SDL_VIDEODRIVER"] = "offscreen"  # no window on the desktop; frames still go to --png
if a.audio:
    env["SDL_AUDIODRIVER"] = "disk"
    env["SDL_DISKAUDIOFILE"] = a.audio
elif not a.show:
    env["SDL_AUDIODRIVER"] = "dummy"
shots = os.path.join(ROOT, "build", "plat_test_shots")
if a.png:
    os.makedirs(shots, exist_ok=True)
    for f in os.listdir(shots):
        os.remove(os.path.join(shots, f))
    env["FCOP_SHOT"] = os.path.join(shots, "f%05d.ppm")
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
if a.png:
    frames = sorted(os.listdir(shots))
    if frames:
        os.makedirs(os.path.dirname(os.path.abspath(a.png)), exist_ok=True)
        for f in frames[::-1][:2]:  # the very last one may be cut off when the game is stopped
            ok = subprocess.run(["ffmpeg", "-loglevel", "quiet", "-y", "-i", os.path.join(shots, f), a.png]).returncode == 0
            if ok:
                break
        print(f"last frame ({f}) saved as {a.png}" if ok else "could not write " + a.png)
    for f in frames:  # about 1 MB each: do not keep them
        os.remove(os.path.join(shots, f))
    else:
        print("no frame was shown")
if a.audio and os.path.exists(a.audio):
    # dsound.c logs "audio device: <rate> Hz <channels> ch" when it opens SDL audio (16-bit samples)
    import array, re
    m = re.search(r"audio device: (\d+) Hz (\d+) ch", out)
    rate, ch = (int(m.group(1)), int(m.group(2))) if m else (22050, 2)
    data = array.array("h", open(a.audio, "rb").read()[: (os.path.getsize(a.audio) // 2) * 2])
    step = rate * ch
    levels = [max((abs(x) for x in data[k:k + step]), default=0) for k in range(0, len(data), step)]
    print(f"audio: {len(data) / step:.1f} s at {rate} Hz {ch} ch"
          f"{'' if m else ' (assumed: no audio device line in the log)'}; peak per second (0-32767): {levels}")
