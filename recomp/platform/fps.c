/* Frame rate: the game's own mission frame limit, the player's limit (Preferences, F11) and
 * the weapon fire rate that depended on the frame rate. */
#include <stdlib.h>
#include <SDL.h>
#include "host.h"
#include "renderer.h"

/* The mission loop waits until at least 8 timer ticks (1/300 s) have passed since the last
   frame: at most 37.5 fps. x86c reads these immediates from guest memory (FRAME_LIMIT).
   The game moves everything by the measured frame time, so 1 tick (up to 300 fps) plays at
   the same speed. */
static int frame_ticks = 8;
static int fps_limit = FPS_ORIGINAL;  /* in effect: FPS_ORIGINAL, 0 (none) or frames per second */
static int fps_unlocked;              /* what F11 switches back to from the original limit */

static void frame_ticks_set(int t)
{
    frame_ticks = t;
    MEM[0x406a81] = MEM[0x40727a] = MEM[0x407297] = (u8)t;  /* cmp reg, imm8 */
    wr32(0x406b31, (u32)t);                                 /* mov edi, imm32 */
    wr32(0x4071ab, (u32)t);
}

void fps_limit_apply(int fps)
{
    fps_limit = fps;
    if (fps != FPS_ORIGINAL) fps_unlocked = fps;
    frame_ticks_set(fps == FPS_ORIGINAL ? 8 : 1);
    if (fps == FPS_ORIGINAL) plat_log("frame limit: original (37.5 fps in missions)");
    else if (fps) plat_log("frame limit: %d fps", fps);
    else plat_log("frame limit: none");
}

int fps_limit_current(void) { return fps_limit; }

/* at start-up, after the EXE is loaded: the player's choice; FCOP_FRAME_TICKS=8 starts with
   the original limit (not saved) */
void fps_init(void)
{
    const char *e = getenv("FCOP_FRAME_TICKS");
    int fps = fps_limit_pref();
    fps_unlocked = fps == FPS_ORIGINAL ? 0 : fps;
    fps_limit_apply(e && atoi(e) >= 8 ? FPS_ORIGINAL : fps);
}

/* F11: the original limit, or back to the player's (no limit if that is the original) */
void frame_limit_toggle(void)
{
    fps_limit_apply(fps_limit == FPS_ORIGINAL ? fps_unlocked : FPS_ORIGINAL);
}

/* Before each present: wait for the frame's due time. Sleeps in 1 ms steps, then spins the
   last 2 ms; a frame that is late by more than a whole period restarts the schedule. */
void frame_pace(void)
{
    static Uint64 due;
    Uint64 now = SDL_GetPerformanceCounter(), freq = SDL_GetPerformanceFrequency(), period;
    if (!fps_limit || fps_limit == FPS_ORIGINAL) { due = 0; return; }
    period = freq / (Uint64)fps_limit;
    if (!due || now > due + period) { due = now + period; return; }
    while (now < due) {
        if (due - now > freq / 500) SDL_Delay(1);
        now = SDL_GetPerformanceCounter();
    }
    due += period;
}

/* 0x458ed0(actor, weapon 0..2) fires a player weapon while its button is held. Each weapon
   has a reload timer (dword at +0x78 + 4 * weapon in the struct at actor+0xac) that a shot
   raises by the reload time and 0x45a2a0 lowers by the frame time. The machine gun's reload
   time is 0, so it fired once per frame: 37.5 shots per second in the original, more at a
   higher frame rate; other reload times were rounded up to whole 8-tick frames. After a
   shot, set the timer to the ticks left until the reload time, so rounded, after the
   previous shot's due time: held fire keeps the original rate at any frame rate (the timer
   is lowered before the next frame's shot). */
void G_458ed0(Cpu *c);
void F_458ed0(Cpu *c)
{
    static struct { u32 timer; s32 due; } gun[8];  /* per actor and weapon: when it last should have fired */
    u32 weapon = rd32(c->r[4] + 8), b = rd32(rd32(c->r[4] + 4) + 0xac);
    u32 timer = b + 0x78 + 4 * weapon, ammo = b + 0x84 + 2 * weapon, ammo2 = b + 0x8a + 2 * weapon;
    u32 before = (u32)rd16(ammo) << 16 | rd16(ammo2);
    s32 now = (s32)rd32(0x511ee4);  /* timer ticks at this frame */
    int k;
    s32 period;
    G_458ed0(c);
    if (frame_ticks >= 8 || weapon >= 3 || before == ((u32)rd16(ammo) << 16 | rd16(ammo2)))
        return;
    period = ((s32)rd32(timer) + 7) / 8 * 8;  /* the reload time in whole original frames */
    if (period < 8) period = 8;
    for (k = 0; k < 7 && gun[k].timer && gun[k].timer != timer; k++) {}
    if (gun[k].timer != timer || now - gun[k].due >= 2 * period || now < gun[k].due) gun[k].timer = timer, gun[k].due = now;
    else gun[k].due += period;
    wr32(timer, gun[k].due + period > now ? (u32)(gun[k].due + period - now) : 0);
}
