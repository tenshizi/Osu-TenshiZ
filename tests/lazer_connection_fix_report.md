# Connected client with inactive input: diagnosis and fixes

Reproduced against the installed osu!lazer 2026.1005.0 client using the read-only
`lazer_connection_probe.cpp` (no input injection or game-memory writes).

Confirmed faults:

- With `input::init()` enabled, `input::get_cursor_pos()` returned false and
  `(0,0)`, while `GetCursorPos()` succeeded at `(1459,727)`. Normal Relax returns
  before its controller when that cursor read fails. The corrected wrapper and
  live binding return success and `(1459,727)` through both paths.
- win32u functions were bound by adding on-disk export offsets to a loaded module.
  Bindings now use `GetProcAddress()` on the loaded module. Cursor reads/writes
  use documented Windows APIs; failed cursor calls fall back to those APIs.
  Failed/partial mouse sends retry only unsent events through `SendInput()`.
- The live difficulty reader used obsolete field locations. Reflection on the
  installed managed assemblies verified CS/OD/AR at `0x2c/0x30/0x34`.
  The same live map changed from incorrectly decoded CS 2 / OD 0 to CS 3.3 / OD 10.
- The fullscreen SDL window had a zero-size client area while minimized and was
  discarded by window discovery. The finder now retains the recognized window;
  geometry still must be valid before input is emitted.

The sampled game clock advances during foreground play. The player clock and
background beatmap clock agreed during these checks. No clock offset change was
needed. The user's active device is a tablet; mouse relative-mode configuration
alone was not treated as proof of a tablet input problem.

Logs now allow concurrent readers. A throttled module status records clock,
focus, geometry, cursor-read success, Aim reports/corrections and Relax progress
so a remaining gate is visible without shutting the application down.

Verification: Release x64 built with v143 (`_LINK_=/DEBUG:NONE` for the local PDB
tool mismatch). Normal Relax: 3,875 checks passed. Aim slider behavior: 12 checks
passed. Live cursor and corrected difficulty were verified against the running
client. The rebuilt helper's tablet test logged `cursor_ok=1`, 513 Aim reports,
3 Aim corrections, and Normal Relax advancing to object index 2. These counters
confirm the feature paths execute; they do not independently confirm osu!'s hit
judgments. After reopening the rebuilt helper and testing with the tablet, the
user confirmed: "Both are working".
