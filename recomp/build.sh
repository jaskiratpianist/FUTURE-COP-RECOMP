#!/bin/sh
# Build the game: ./fcop, one self-contained executable (SDL2 is linked in; it needs only the C
# library). Put it anywhere: it uses game_files/ and user_info/ next to itself.
#
#   ./recomp/build.sh                 optimised, stripped
#   DEBUG=1 ./recomp/build.sh         with debug info (for gdb)
#   SYSTEM_SDL=1 ./recomp/build.sh    link the system's shared SDL2 instead of building it
#   OPT=-O1 ./recomp/build.sh         other optimisation flags
#
# The first build downloads SDL2's source (once) and translates the EXE into C. A platform file
# that fails to compile keeps its last good object (and its errors are printed), so one broken
# file does not block everyone else.
set -e
cd "$(dirname "$0")/.."
B=build/recomp
OUT=${OUT:-fcop}

# ---- SDL2, static, loading X11/Wayland/PulseAudio/ALSA at run time when present ----
SDL_VER=2.32.10
SDL_SHA=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
SDL=build/sdl2
if [ -n "$SYSTEM_SDL" ]; then
    SDL_CFLAGS=$(sdl2-config --cflags)
    SDL_LIBS=$(sdl2-config --libs)
else
    if [ ! -f $SDL/lib/libSDL2.a ]; then
        echo "building SDL2 $SDL_VER (once)"
        mkdir -p $SDL/src
        [ -f $SDL/src/SDL2.tar.gz ] || curl -fsSL -o $SDL/src/SDL2.tar.gz \
            https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-$SDL_VER.tar.gz
        echo "$SDL_SHA  $SDL/src/SDL2.tar.gz" | sha256sum -c --quiet
        tar xzf $SDL/src/SDL2.tar.gz -C $SDL/src
        cmake -S $SDL/src/SDL2-$SDL_VER -B $SDL/src/build -DCMAKE_BUILD_TYPE=Release \
            -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF -DCMAKE_INSTALL_PREFIX="$PWD/$SDL" > $SDL/src/cmake.log
        cmake --build $SDL/src/build -j"$(nproc)" > $SDL/src/build.log 2>&1
        cmake --install $SDL/src/build > /dev/null
        rm -rf $SDL/src/SDL2-$SDL_VER $SDL/src/build
    fi
    SDL_CFLAGS=$($SDL/bin/sdl2-config --cflags)
    SDL_LIBS="$($SDL/bin/sdl2-config --static-libs) -ldl"
fi

# ---- the game ----
[ -d $B/gen ] && [ $B/gen/table.c -nt recomp/x86c.py ] || .venv/bin/python -I recomp/x86c.py
if [ -n "$DEBUG" ]; then OPT=${OPT:--O1} DBG=-g STRIP=; else OPT=${OPT:--O2} DBG= STRIP=-s; fi
CFLAGS="$OPT $DBG -w -Irecomp/rt -Irecomp/platform -I$B/gen $SDL_CFLAGS"
mkdir -p $B/obj_game
# other flags: rebuild everything
[ "$(cat $B/obj_game/flags 2>/dev/null)" = "$CFLAGS" ] || { rm -f $B/obj_game/*.o; echo "$CFLAGS" > $B/obj_game/flags; }
HDR=$(ls -t recomp/rt/*.h recomp/platform/*.h | head -1)  # newest header
export B CFLAGS HDR
ls $B/gen/*.c recomp/rt/rt.c recomp/platform/*.c | xargs -P "$(nproc)" -I{} sh -c '
    o=$B/obj_game/$(basename {} .c).o
    case {} in $B/gen/*) dep=recomp/rt/cpu.h ;; *) dep=$HDR ;; esac
    if [ "$o" -nt {} ] && [ "$o" -nt $dep ]; then exit 0; fi
    if gcc $CFLAGS -c {} -o $o.tmp 2> $o.err; then mv $o.tmp $o; else echo "COMPILE ERROR in {}:"; head -20 $o.err; fi'
OBJS=$(for f in $B/gen/*.c recomp/rt/rt.c recomp/platform/*.c; do echo $B/obj_game/$(basename $f .c).o; done)
gcc $STRIP -o $OUT $OBJS $SDL_LIBS -lm -lpthread
echo "built $OUT"
