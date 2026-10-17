# Task: controls (key rebinding, arrow keys, gamepads)

The game (FCopLAPD.exe, 1998) has been translated to C and runs natively on Linux with SDL2;
the Windows functions it imports are implemented in `recomp/platform/`. Read
`recomp/platform/host.h` and `recomp/platform/win.h` first, with your file-reading tool.

## Facts

- In missions the game polls `GetAsyncKeyState` (u32_misc.c) for its bound keys; the
  default bindings are the numeric keypad: NumPad8 forward, NumPad5 back, NumPad4/6 turn,
  Space gun, Alt heavy weapon, Ctrl special, Z jump, NumPad2 action, NumPad0 change target,
  NumPad9/7 jink, M map, Tab camera, Esc pause. Bindings are edited in the game's
  "Controller Configuration" dialog (Preferences > Controller Configuration, dialog 113,
  drawn by dialog.c) and saved to `controls.txt` in the game folder.
- SDL key events become Windows virtual-key state (`vk_down[]`, `dik_down[]`) and window
  messages in u32_win.c (`handle_sdl_event`, `sdl_to_vk`, `sdl_to_dik`).
- DirectInput (dinput.c) is only used for joysticks: the game calls
  `EnumDevices(DIDEVTYPE_JOYSTICK 4, ..., DIEDFL_ATTACHEDONLY 1)`; today no joystick is
  ever found, so gamepads do nothing.

**Run the test command in the foreground and wait for its output. Never start it as a
background task: your session ends while you wait for a background task.**

**Already done** (by an earlier worker, keep it): item 2, the arrow-key aliases in
u32_misc.c (`key_aliases`, `bindings_use_arrow_keys` reading the binding tables at 0x511c20
and 0x511ca0: verify those addresses are right while you work on item 1).

**Read only files inside this repository.** System headers (/usr/include/SDL2/...) are
blocked and reading one ends your session. The SDL calls you need (SDL 2.32):
`SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER)`, `SDL_NumJoysticks()`,
`SDL_IsGameController(i)`, `SDL_GameControllerOpen(i)`, `SDL_GameControllerName(gc)`,
`SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX / LEFTY / RIGHTX / RIGHTY /
TRIGGERLEFT / TRIGGERRIGHT)` (Sint16), `SDL_GameControllerGetButton(gc,
SDL_CONTROLLER_BUTTON_A / B / X / Y / BACK / GUIDE / START / LEFTSTICK / RIGHTSTICK /
LEFTSHOULDER / RIGHTSHOULDER / DPAD_UP / DPAD_DOWN / DPAD_LEFT / DPAD_RIGHT)` (Uint8),
`SDL_GameControllerUpdate()`. Virtual pad: `int i = SDL_JoystickAttachVirtual(
SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);`
(axes and buttons in the SDL_GameControllerAxis / SDL_GameControllerButton order, so SDL
maps it as a game controller automatically), `SDL_Joystick *j = SDL_JoystickOpen(i);`
`SDL_JoystickSetVirtualAxis(j, axis, Sint16)`, `SDL_JoystickSetVirtualButton(j, button,
Uint8)`.

## What to do

1. **Key rebinding must work.** Open Controller Configuration and click a key box (the
   boxes are STATIC controls with SS_NOTIFY; dialog.c sends STN_CLICKED). Find out with
   `--api` what the game then does to read the new key (GetAsyncKeyState polling, window
   messages to the dialog, DirectInput...) and make it work in our platform. If it needs
   something from dialog.c (for example key messages sent to the dialog procedure while a
   dialog is open), describe exactly what in your final reply instead of editing dialog.c.
   Then check the new binding is used in a mission.
2. **Arrow keys** (laptops have no keypad): by default the arrow keys also act as
   NumPad8/5/4/6 (Up forward, Down back, Left/Right turn) in `GetAsyncKeyState`, unless the
   game's current bindings use an arrow key itself (then leave arrows alone). Do it in
   u32_misc.c's GetAsyncKeyState/GetKeyState (an alias table), not by faking events.
3. **Gamepads** with SDL's GameController API, exposed to the game as a DirectInput
   joystick (DirectX 5 IDirectInputDevice / IDirectInputDevice2: Acquire, Unacquire,
   GetCapabilities, EnumObjects, SetDataFormat (c_dfDIJoystick: DIJOYSTATE, 80 bytes:
   lX lY lZ lRx lRy lRz rglSlider[2] rgdwPOV[4] rgbButtons[32]), SetProperty DIPROP_RANGE
   (axis ranges the game asks for) and DIPROP_DEADZONE, GetDeviceState, Poll,
   SetCooperativeLevel, GetDeviceInfo). Left stick = X/Y, right stick = Rx/Ry (or Z/Rz),
   D-pad = POV 0 (hundredths of degrees, 0xFFFFFFFF centred) and also X/Y when the stick is
   centred, A/B/X/Y/LB/RB/Back/Start = buttons 0..7, triggers = buttons 8/9. Enumerate every
   connected game controller (hot-plugging: enumerate what is connected at EnumDevices time).
   Check the game's joystick code with `--api` to see which calls and formats it uses.
   **Testing without hardware:** if `FCOP_VIRTUAL_PAD=1` is set, create an SDL virtual
   joystick (`SDL_JoystickAttachVirtual`, SDL >= 2.0.14) that SDL treats as a game
   controller, and let the scripted input `FCOP_INPUT` drive it: items like
   `20000:pad_a*500` (button A held 500 ms), `20000:pad_up*1000` (D-pad), and
   `20000:pad_lx=-32768*1000` (left stick X held at a value). The scripted input parser is
   `scripted_input()` in u32_win.c: extend it there.
4. Log one line per gamepad found and when the game acquires it.

## Files

You may edit `recomp/platform/dinput.c`, `recomp/platform/u32_misc.c` (key functions only)
and `recomp/platform/u32_win.c` (key mapping and `scripted_input` only). Another worker is
editing dsound.c and gdi_misc.c at the same time: do not touch them, and do not touch
dialog.c, ddraw.c, core.c or any header.

## Testing (the only shell command you may run)

    .venv/bin/python -I recomp/plat_test.py --seconds 32 --input "9000:click@484,303 26000:Up*3000" --png build/shots/c.png

starts the first mission (headless) and walks forward with the Up arrow; compare the PNG
with a run without input (the robot moves forward into a parking lot). Preferences:
`9000:click@156,246`, Controller Configuration button at `208,256` (check with a PNG), the
Move Forward key box for player 1 at about `164,190` (check with a PNG). Gamepad:
`FCOP_VIRTUAL_PAD=1` cannot be set through the test command, so instead make the virtual pad
also switch on when `FCOP_INPUT` contains a `pad_` item. `--api` logs every Windows call.

## Rules

- Plain C99 + SDL2 + POSIX. Helpers `static`. No empty stubs.
- Read files with your file-reading tool, never with shell commands. Every shell command
  except the test command is blocked; if an action is denied, continue.

## Final reply

What works, what is partial, what (if anything) dialog.c needs, and the tests you ran.
