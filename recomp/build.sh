#!/bin/sh
# Build the native recompiled game: build/recomp/fcop <game dir>
# A platform file that fails to compile keeps its last good object (and its errors are
# printed), so one broken file does not block everyone else.
set -e
cd "$(dirname "$0")/.."
B=build/recomp
PY=.venv/bin/python
DLL=
LIBS="$(sdl2-config --libs) -lm -lpthread"
case "$(uname -s)" in
MINGW*|MSYS*)  # Windows (MSYS2 MinGW shell): console program, 16 MB main stack like Linux's 8 MB+
    PY=.venv/Scripts/python.exe
    LIBS="$(sdl2-config --libs | sed 's/-mwindows//') -lm -lpthread -static-libgcc -Wl,--stack,16777216"
    DLL="$(dirname "$(command -v sdl2-config)")/SDL2.dll" ;;
esac
[ -d $B/gen ] && [ $B/gen/table.c -nt recomp/x86c.py ] || $PY -I recomp/x86c.py
mkdir -p $B/obj_game
OPT=${OPT:--O1}
CFLAGS="$OPT -g -w -Irecomp/rt -Irecomp/platform -I$B/gen $(sdl2-config --cflags)"
export B CFLAGS
ls $B/gen/*.c recomp/rt/rt.c recomp/platform/*.c | xargs -P "$(nproc)" -I{} sh -c '
    o=$B/obj_game/$(basename {} .c).o
    if [ "$o" -nt {} ] && [ "$o" -nt recomp/rt/cpu.h ] && [ "$o" -nt recomp/platform/host.h ] && [ "$o" -nt recomp/platform/win.h ] && [ "$o" -nt recomp/platform/dialog.h ]; then exit 0; fi
    if gcc $CFLAGS -c {} -o $o.tmp 2> $o.err; then mv $o.tmp $o; else echo "COMPILE ERROR in {}:"; head -20 $o.err; fi'
OBJS=$(for f in $B/gen/*.c recomp/rt/rt.c recomp/platform/*.c; do echo $B/obj_game/$(basename $f .c).o; done)
gcc -o $B/fcop $OBJS $LIBS
[ -z "$DLL" ] || cp -u "$DLL" $B/  # the game finds SDL2.dll next to fcop.exe
echo "built $B/fcop"
