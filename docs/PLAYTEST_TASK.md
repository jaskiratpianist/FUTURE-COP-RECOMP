# Task: play-test the game and report problems (no code changes)

The game Future Cop: L.A.P.D. (1998) has been translated to C and runs natively on Linux.
The first mission works. Your job: **reach as much of the rest of the game as you can and
report every crash, hang, missing feature or wrong picture**, with the exact steps to
reproduce it. **Do not edit any file.**

## The one command you may run

    .venv/bin/python -I recomp/plat_test.py --seconds 40 --input "<script>" --png build/shots/<name>.png --lines 30

It builds and starts the game headless (nothing on screen), plays the scripted input,
prints the end of the log ("BUILD OK; game still running after 40 s (killed)" is the normal
end; "exited with ..." or a crash/trap/abort line is a problem) and saves the last frame as
a PNG that you can open with your file-reading tool. Add `--api` to log every Windows call
(long), `--audio build/shots/a.raw` to get sound levels.

Script items are `<ms>:<action>` separated by spaces: `click@x,y` (game pixels, 640x480 in
menus), an SDL key name held 100 ms (`Return`, `Escape`, `Down`, `space`, `Tab`, `m`,
`Keypad_8`), or held longer with `*ms` (`Keypad_8*3000`).

## What is known

- Main menu (640x480): NEW GAME 156,47; LOAD GAME 156,77; SAVE GAME 156,107; HOST GAME
  156,161; JOIN GAME 156,191; PREFERENCES 156,246; HELP 156,276; QUIT 156,306; CRIME WAR
  (mode name) 380,47 with "map" 484,47 and "mode" 484,77 buttons; the district map at
  about 300-450 x 97-250; BRIEFING 362,306; START 484,303. The weapon panel at the bottom
  has up/down arrows to choose weapons; NORMAL with left/right arrows is the difficulty.
- START at 9 s starts the first mission (Griffith Park) about 15 s later.
- In a mission: keypad 8/5/4/6 move and turn (arrow keys may also work), Space gun,
  Left_Alt heavy weapon, Left_Ctrl special, z jump, Keypad_2 action, Keypad_0 target,
  m map, Tab camera, Escape pause bar (Resume / Abort / Audio / Controls; Abort returns to
  the menu).
- Dialogs (Preferences, Load/Save, pause, quit) are drawn by us in the game's style.

## What to try (one test run per idea; keep each script short)

1. Every main-menu button, including HELP, BRIEFING, map/mode, LOAD GAME and SAVE GAME
   (pick a slot, OK), HOST/JOIN GAME (network: just check nothing crashes, Cancel).
2. The other mode (mode button) and other districts/missions (click other map regions,
   then START). Try to start at least 4 different missions.
3. In the first mission: walk for a long time, fire every weapon, jump, use the map and
   camera keys, let the robot die or the time run out if possible; pause, Audio, Controls,
   Resume; Abort back to the menu and start again.
4. Two-player mode if the menu offers it.

## Write findings down as you go

After **every** test run, append one short entry to `recomp/platform/playtest_report.txt`
(create it if needed; it is the only file you may write): the script, OK or PROBLEM, and
for a problem what happened and what you expected. Your session may end without warning, so
the file is what counts. Previous runs found menus, two modes, several districts, 2-player,
movement, weapons, map, camera, pause/resume/abort working: concentrate on what is left
(HELP, BRIEFING, save then load a game, long play until death or mission end, the
hovercraft, other districts' missions, Audio and Controls from the pause bar).

## Final reply

A numbered list of problems. For each: the script that shows it, what happened (log lines,
crash address, what the PNG shows) and what you expected. Then a short list of what worked.
Run builds only through the command above; another worker is editing code at the same
time, so a build may briefly fail: just run the test again.

## Rules

- Run the test command in the foreground and wait for its output; never as a background
  task (your session ends while you wait for one).

Read files with your file-reading tool, never with shell commands. Every shell command
except the test command is blocked; if an action is denied, continue.
