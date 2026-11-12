# Native Relax duplicate press recovery

An ordinary tap could be released on a temporarily unreadable native frame, then
pressed again by the generic hold recovery on the next valid frame. The same
recovery also re-armed ordinary taps after pause/focus interruption. An extra
physical press could hit another visible note early without being counted as a
new scheduled head press.

The regression reproduces a missing frame between the first press and its
release, followed by a valid judgment and pause/resume. Before the fix, four
assertions fail: uninterrupted tap, no extra recovery press, no pause re-arm,
and the following burst note's independent press count.

Transient reads now preserve every active tap only until its existing release
time, under the same 50 ms wall / 32 ms game-time freshness bounds. Only protected
duration holds can be re-established after an interruption. Initial head
thresholds, native hover gating, Great windows, variation settings and slider
durations are unchanged.

Validation: 1,263 live coordination checks, 33,086 legacy emitted-input checks,
and 14 Aim input checks pass. Release x64 builds with v143. The main Release
executable was replaced and its hash matched the staged build.

Cursor freeze investigation is still open. A prior run reproduced it with Aim
disabled and zero Aim corrections. The reader now yields for 1 ms and uses
above-normal priority; Aim disabled removes its mouse hook. The installed build
also logs mouse-hook presence, received/replaced movement counts, report age,
and the other cursor/input feature states. Await a capture of the cursor-only
freeze with this instrumentation; do not claim that the freeze is fixed.
