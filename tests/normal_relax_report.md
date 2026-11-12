# Normal Relax: cursor-aware behavior

Historical report: this implementation was retired after live gameplay regressions.
See `relax_recovery_report.md` for the current production behavior.

Normal now reacts to cursor position instead of pre-scheduling randomized clicks.
Each head becomes eligible 12 game milliseconds before its timestamp and stays
eligible through its OD-dependent hit window. A late or missed arrival does not
move later notes. Presses alternate and create a fresh edge during existing holds.
Sliders use the existing curve sampler and a follow radius that expands while
tracking; spinners hold through their duration. Idle taps release after 50 game
milliseconds. Normal bypasses the old timing, stamina and hold-shape settings.

References checked:
- https://github.com/ppy/osu/blob/2026.921.0-lazer/osu.Game.Rulesets.Osu/Mods/OsuModRelax.cs
- https://github.com/ppy/osu/blob/master/osu.Game/Rulesets/Replays/AutoGenerator.cs
- https://github.com/ppy/osu/blob/master/osu.Game.Rulesets.Osu/Objects/Drawables/SliderInputManager.cs
- https://github.com/ppy/osu/blob/master/osu.Game.Rulesets.Osu/Objects/Drawables/DrawableSliderBall.cs
- https://github.com/ppy/osu/blob/master/osu.Game.Rulesets.Osu/Scoring/OsuHitWindows.cs

The external adapter cannot be identical to the built-in mod. It receives sampled
clock values and OS cursor coordinates, not drawable hit judgments or the game's
input state. It records emitted heads rather than confirmed judgments, uses the
project's existing playfield/stack projection and approximate curve sampler, and
cannot identify zero-required-spin spinners. Input processing delay and custom
playfield/raw-input transforms can therefore still affect results. No injected game
code, game-memory writes or automatic score/mod changes were added.

`normal_relax_stream_tests.cpp` replaces the old randomized-schedule expectations
with deterministic cursor/timing tests: late aim, expiry, streams, missed-first-note
recovery, overlapping slider heads, follow-area release/reacquisition, spinners,
Hard Rock, stacking, rewind, suspension, duplicate clock samples and mode routing.
`accuracy_tests.cpp` covers the existing Autobot scheduler separately.

Live gameplay equivalence has not been verified; osu! was not running during this
change. Automated tests verify decision rules and routing, not actual game judgments.

Validation: 3,875 Normal Relax checks and 303 Autobot accuracy checks passed.
Release x64 built with v143 and the build-process environment `_LINK_=/DEBUG:NONE`
because the installed debug-symbol components otherwise fail with LNK1101.
This executable has no matching newly generated PDB; the project configuration
was not changed.
