# Cursor freeze investigation

The reported freeze persists with Aim Assist disabled. The recent Release log confirms `aim=0`, `mouse_hook=0`, `replaced=0`, `tap_assist=0`, and `overlay_visible=0` during gameplay. This rules out the active Aim mouse transformation in that run; it does not identify the exact instant or cause of the freeze.

The keyboard hook was nevertheless installed unconditionally at startup. Its callback called `smart_owns_input()` even in Normal Relax, taking the Relax mutex. That mutex is also held during generated keyboard input. Waiting in a low-level input callback can stall input processing. Removing that unnecessary callback from Normal Relax eliminates this possible contention path without changing base hit scheduling or slider holds.

The overlay now starts with neither hook installed and enables each only when its feature requires interception:

- Aim Assist requests the mouse hook.
- Smart Relax or Tap Assist requests the keyboard hook.
- Normal Relax alone requests neither.

The keyboard callback also bypasses module locks if interception has been disabled before the next pump removes the hook. Logs now record keyboard-hook state alongside existing mouse and overlay diagnostics.

Validation: Release build succeeded; input-hook policy 8 checks, live Relax coordination 1,414 checks, Normal Relax emitted input 33,086 checks, Autobot accuracy 303 checks, density/configuration 48 checks; all passed. These simulated-input checks do not prove that the reported hardware/gameplay freeze is resolved. Gameplay confirmation is outstanding.

Windows callback guidance: https://learn.microsoft.com/en-us/windows/win32/winmsg/lowlevelkeyboardproc
