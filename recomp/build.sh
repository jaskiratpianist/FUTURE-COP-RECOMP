#!/bin/sh
# Build the game: ./fcop, one self-contained executable (SDL2 is linked in; it needs only the C
# library). Put it anywhere: it uses game_files/ and user_info/ next to itself.
#
#   ./recomp/build.sh                 optimised, stripped
#   DEBUG=1 ./recomp/build.sh         with debug info (for gdb)
#   SYSTEM_SDL=1 ./recomp/build.sh    link the system's shared SDL2 instead of building it
#   OPT=-O1 ./recomp/build.sh         other optimisation flags
#   TARGET=windows ./recomp/build.sh  fcop.exe for 64-bit Windows (cross-compiled; downloads the
#                                     llvm-mingw toolchain once, into build/llvm-mingw)
#
# The first build downloads SDL2's source (once) and translates the EXE into C. A platform file
# that fails to compile keeps its last good object (and its errors are printed), so one broken
# file does not block everyone else.
set -e
cd "$(dirname "$0")/.."
B=build/recomp
CC=gcc OBJ=$B/obj_game SDL=build/sdl2 LDFLAGS="-lm -lpthread"
if [ "$TARGET" = windows ]; then
    # llvm-mingw: clang with the MinGW-w64 headers and libraries (UCRT), self-contained
    TC_VER=20261020
    TC_SHA=55064b60b991a1e1e8090b8c2f84e82b22cfe5903d12c159d65064cf5e8ae335
    TC=build/llvm-mingw
    if [ ! -x $TC/bin/x86_64-w64-mingw32-clang ]; then
        echo "downloading llvm-mingw $TC_VER (once)"
        mkdir -p $TC
        [ -f $TC/llvm-mingw.tar.xz ] || curl -fsSL -o $TC/llvm-mingw.tar.xz \
            https://github.com/mstorsjo/llvm-mingw/releases/download/$TC_VER/llvm-mingw-$TC_VER-ucrt-ubuntu-22.04-x86_64.tar.xz
        echo "$TC_SHA  $TC/llvm-mingw.tar.xz" | sha256sum -c --quiet
        tar xJf $TC/llvm-mingw.tar.xz -C $TC --strip-components=1
        rm $TC/llvm-mingw.tar.xz
    fi
    CC="$PWD/$TC/bin/x86_64-w64-mingw32-clang" OBJ=$B/obj_win SDL=build/sdl2-win
    # a windowed program (no console); everything static, so fcop.exe needs only Windows' own DLLs
    LDFLAGS="-static -mwindows -lcomdlg32 -lpthread"
    OUT=${OUT:-fcop.exe}
fi
OUT=${OUT:-fcop}

# ---- SDL2, static, loading X11/Wayland/PulseAudio/ALSA at run time when present ----
SDL_VER=2.32.10
SDL_SHA=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
if [ -n "$SYSTEM_SDL" ] && [ "$TARGET" != windows ]; then
    SDL_CFLAGS=$(sdl2-config --cflags)
    SDL_LIBS=$(sdl2-config --libs)
else
    if [ ! -f $SDL/lib/libSDL2.a ]; then
        echo "building SDL2 $SDL_VER${TARGET:+ for $TARGET} (once)"
        mkdir -p $SDL/src
        [ -f $SDL/src/SDL2.tar.gz ] || curl -fsSL -o $SDL/src/SDL2.tar.gz \
            https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-$SDL_VER.tar.gz
        echo "$SDL_SHA  $SDL/src/SDL2.tar.gz" | sha256sum -c --quiet
        tar xzf $SDL/src/SDL2.tar.gz -C $SDL/src
        CROSS=
        [ "$TARGET" = windows ] && CROSS="-DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=$CC -DCMAKE_RC_COMPILER=$PWD/$TC/bin/x86_64-w64-mingw32-windres"
        cmake -S $SDL/src/SDL2-$SDL_VER -B $SDL/src/build -DCMAKE_BUILD_TYPE=Release $CROSS \
            -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF -DCMAKE_INSTALL_PREFIX="$PWD/$SDL" > $SDL/src/cmake.log
        cmake --build $SDL/src/build -j"$(nproc)" > $SDL/src/build.log 2>&1
        cmake --install $SDL/src/build > /dev/null
        rm -rf $SDL/src/SDL2-$SDL_VER $SDL/src/build
    fi
    SDL_CFLAGS=$(sh $SDL/bin/sdl2-config --cflags)
    if [ "$TARGET" = windows ]; then  # our main() is a plain one: no SDL2main
        SDL_CFLAGS="$(echo "$SDL_CFLAGS" | sed 's/-Dmain=SDL_main//') -DSDL_MAIN_HANDLED"
        SDL_LIBS=$(sh $SDL/bin/sdl2-config --static-libs | sed 's/-lmingw32 //; s/-lSDL2main //; s/-mwindows //')
    else
        SDL_LIBS="$(sh $SDL/bin/sdl2-config --static-libs) -ldl"
    fi
fi

# ---- the game ----
[ -d $B/gen ] && [ $B/gen/table.c -nt recomp/x86c.py ] || .venv/bin/python -I recomp/x86c.py
if [ -n "$DEBUG" ]; then OPT=${OPT:--O1} DBG=-g STRIP=; else OPT=${OPT:--O2} DBG= STRIP=-s; fi
CFLAGS="$OPT $DBG -w -Irecomp/rt -Irecomp/platform -I$B/gen $SDL_CFLAGS"
mkdir -p $OBJ
# other flags: rebuild everything
[ "$(cat $OBJ/flags 2>/dev/null)" = "$CFLAGS" ] || { rm -f $OBJ/*.o; echo "$CFLAGS" > $OBJ/flags; }
HDR=$(ls -t recomp/rt/*.h recomp/platform/*.h | head -1)  # newest header
export B OBJ CC CFLAGS HDR
ls $B/gen/*.c recomp/rt/rt.c recomp/platform/*.c | xargs -P "$(nproc)" -I{} sh -c '
    o=$OBJ/$(basename {} .c).o
    case {} in $B/gen/*) dep=recomp/rt/cpu.h ;; *) dep=$HDR ;; esac
    if [ "$o" -nt {} ] && [ "$o" -nt $dep ]; then exit 0; fi
    if $CC $CFLAGS -c {} -o $o.tmp 2> $o.err; then mv $o.tmp $o; else echo "COMPILE ERROR in {}:"; head -20 $o.err; fi'
OBJS=$(for f in $B/gen/*.c recomp/rt/rt.c recomp/platform/*.c; do echo $OBJ/$(basename $f .c).o; done)
$CC $STRIP -o $OUT $OBJS $SDL_LIBS $LDFLAGS
echo "built $OUT"
