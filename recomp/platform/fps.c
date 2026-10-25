/* Frame rate: the game's own mission frame limit, the player's limit (Preferences, F11) and
 * the weapon fire rate that depended on the frame rate. */
#include <stdlib.h>
#include <string.h>
#include <math.h>
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
    const char *e = getenv("FCOP_FRAME_TICKS"), *l = getenv("FCOP_FPS_LIMIT");
    int fps = l ? atoi(l) : fps_limit_pref();  /* FCOP_FPS_LIMIT: 0 = none (not saved) */
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

/* 0x446e00(actor, dt) updates a player robot. Its physics runs in whole 8-tick steps (the
   original frame): steps = (dt + remainder at actor+0xb6) / 8, the rest stays in +0xb6, and
   0x442120 moves the robot once per step. Above 37.5 fps most frames have no step or one,
   so the robot, and the camera that follows it (0x4456a0), judder. Shown instead: the
   robot between its last two steps, by the ticks since the last one (at most one step
   behind the physics), set right after 0x442120 so that everything the update derives from
   it afterwards (its parts, the point at +0x5c, the camera) follows. The real values come
   back at the next update, unless the game has moved the robot itself in between. Values:
   the robot's matrix at +0x2c (3x3, 1.0 = 0x10000) and its position at +0x50. */
#define LERP_N 12
static struct {
    u32 actor, steps;
    s32 before[LERP_N], step[LERP_N], shown[LERP_N], off[LERP_N];
} lerp[4];

static int lerp_slot(u32 actor)
{
    int k;
    for (k = 0; k < 3 && lerp[k].actor && lerp[k].actor != actor; k++) {}
    if (lerp[k].actor != actor) memset(&lerp[k], 0, sizeof lerp[k]), lerp[k].actor = actor;
    return k;
}

void G_446e00(Cpu *c);
void F_446e00(Cpu *c)
{
    u32 actor = rd32(c->r[4] + 4);
    s32 dt = (s32)rd32(c->r[4] + 8);
    int k = lerp_slot(actor), i;
    for (i = 0; i < LERP_N; i++) {
        u32 a = actor + 0x2c + 4 * i;
        if (lerp[k].off[i] && (s32)rd32(a) == lerp[k].shown[i]) wr32(a, (u32)(lerp[k].shown[i] + lerp[k].off[i]));
        lerp[k].off[i] = 0;
        lerp[k].before[i] = (s32)rd32(a);
    }
    lerp[k].steps = frame_ticks < 8 && dt > 0 ? (u32)(dt + (rd8(actor + 0xb6) & 7)) >> 3 : ~0u;
    G_446e00(c);
}

static void lerp_show(u32 actor)
{
    int k = lerp_slot(actor), i;
    if (lerp[k].steps != ~0u) {
        s32 left = 8 - (rd8(actor + 0xb6) & 7);  /* ticks until the next step */
        for (i = 0; i < LERP_N; i++) {
            u32 a = actor + 0x2c + 4 * i;
            s32 now = (s32)rd32(a);
            if (lerp[k].steps) lerp[k].step[i] = (now - lerp[k].before[i]) / (s32)lerp[k].steps;
            if (lerp[k].step[i] > 0x4000 || lerp[k].step[i] < -0x4000) lerp[k].step[i] = 0;  /* a jump */
            lerp[k].off[i] = lerp[k].step[i] * left / 8;
            lerp[k].shown[i] = now - lerp[k].off[i];
            wr32(a, (u32)lerp[k].shown[i]);
        }
        lerp[k].steps = ~0u;
    }
}

/* 0x442120(actor, dt, steps, ...): the robot's physics */
void G_442120(Cpu *c);
void F_442120(Cpu *c)
{
    u32 actor = rd32(c->r[4] + 4);
    G_442120(c);
    lerp_show(actor);
}

static s32 cam_dt = -1;  /* ticks of this frame while 0x4456a0 runs */

/* the camera of 0x446e00's robot (when 0x442120 did not run, the robot is moved here) */
void G_4456a0(Cpu *c);
void F_4456a0(Cpu *c)
{

    lerp_show(rd32(c->r[4] + 4));
    cam_dt = frame_ticks < 8 ? (s32)rd32(c->r[4] + 8) : -1;
    G_4456a0(c);
    cam_dt = -1;
}

/* 0x445eb0(angle, target, n): the camera turns toward the target angle (4096 units a turn) by
   n times angle = (31 * angle + target) / 32, at least n units. n counts 2-tick units of the
   frame time (the rest carries over), so at 5 ticks a frame it alternates 2 and 3: the turn
   jerked. Above 37.5 fps: the same curve for the frame's exact ticks, keeping the fraction
   the integer angle drops. */
void G_445eb0(Cpu *c);
void F_445eb0(Cpu *c)
{
    static struct { u32 site; s32 out; double frac; } turn[4];  /* per call site (yaw, pitch) */
    u32 sp = c->r[4], site = rd32(sp);
    double exact;
    int k;
    s32 cur = (s32)(rd32(sp + 4) & 0xfff), to = (s32)(rd32(sp + 8) & 0xfff), n = (s32)rd32(sp + 12), d, r;
    if (cam_dt <= 0) { G_445eb0(c); return; }
    if (cur >= 0x800) cur -= 0x1000;
    if (to >= 0x800) to -= 0x1000;
    if (to - cur >= 0x800) cur += 0x1000;
    else if (cur - to >= 0x800) cur -= 0x1000;
    d = to - cur;
    for (k = 0; k < 3 && turn[k].site && turn[k].site != site; k++) {}
    turn[k].site = site;
    exact = turn[k].out == cur ? cur + turn[k].frac : cur;  /* our own result: keep its fraction */
    exact = to - (to - exact) * pow(31.0 / 32.0, cam_dt / 2.0);
    r = (s32)floor(exact + 0.5);
    if (r == cur && d && n > 0) {  /* the game's minimum step */
        r = cur + (d > 0 ? (d < n ? d : n) : (-d < n ? d : -n));
        exact = r;
    }
    turn[k].out = r;
    turn[k].frac = exact - r;
    c->r[0] = (u32)r;
    c->r[4] = sp + 4;  /* ret */
}

