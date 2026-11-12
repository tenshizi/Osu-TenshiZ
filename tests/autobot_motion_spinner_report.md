# Autobot motion and spinner polish — verification report

This report covers this pass only. Measurements are deterministic samples of the final motion-engine output, in osu! playfield coordinates, before screen projection/pixel rounding. They are not live game judgments or OS capture measurements.

## 1. Hidden future spatial information: audited locations

- `target_point()`: cached future coordinates (including the spinner approach point) without checking appearance time. Hidden targets now return without creating a cache entry.
- `plan_and_reconcile()`: used the next target as an immediate acquisition/recovery destination; also used its distance to choose when break/startup motion should yield. Hidden objects now allow timing-only local continuation/decorative budgeting.
- `arrival_velocity()`: the next target selected the current object's outgoing tangent, leaking information even before the current hit. Hidden-next arrivals now retain a small continuation of the known incoming stroke.
- Dense-flow lookahead inside `arrival_velocity()`: subsequent object positions changed the rolling direction. Lookahead now stops at the first unperceived object.
- `local_motion_pressure()`: future distances and turns altered current slider economy. Cadence remains available, but spatial travel/turn terms require perceived endpoints.
- `eval_slider()`: the next target selected the exit strategy, projected endpoint, and outgoing velocity. Those spatial contributions now require perception.
- Active long-object dispatch now also checks perception. The host passes the actual game clock separately, so the prepared scheduling clock cannot unlock geometry early.

Whole/local coverage uses only the current slider's geometry. Derivative-transfer curves, safe-anchor projection, micro modifiers, and recovery were audited: they receive current state or targets behind the same perception boundary. No hidden slider tangent/body/tail preparation was added.

## 2. Exact perception timing

`perception_time = object.start_time - preempt_game_ms`.

The preempt calculation is the integer AR difficulty range: AR <= 5 uses `1800 - 120*AR`; AR > 5 uses `1200 - 150*(AR-5)`, with nonnegative duration. Examples: AR0=1800 ms, AR5=1200, AR9=600, AR10=450, AR11=300. This follows the [official osu! hit-object timing model](https://github.com/ppy/osu/blob/master/osu.Game.Rulesets.Osu/Objects/OsuHitObject.cs).

The beatmap's already-adjusted AR is used. HR is applied only when the host's existing flag says it has not been applied. Speed changes wall-clock exposure (`preempt / speed`), not the game-time comparison. Eligibility uses `game.cur_time`, not Relax's prepared time. No global reaction delay was added.

A 500 ms gap is therefore not automatically hidden. At AR5 its future object can already be visible; at AR10 the gap initially contains a 50 ms hidden interval. The tests deliberately exercise real hidden windows.

## 3. Commitment

Before perception, the same object intent carries an analytic decay of the current velocity/acceleration. There is no random fidget destination or hidden target coordinate.

At perception, that intent is promoted once, retaining its plan ID and capturing the current p/v/a. Required travel/braking time relative to available time determines initial commitment. Newly perceived/idle-to-gameplay acquisitions with spare time soften endpoint jerk inside the timed curve; urgent motion retains the original profile. Commitment rises monotonically to one. It does not reroll targets, chase an optimum every frame, or delay the scheduled arrival.

## 4. Left/right leakage results

Maximum difference before appearance, playfield pixels:

| Paired scenario | Before | After |
|---|---:|---:|
| Circle → 500 ms → circle | 25.965 | 0.000000 |
| Circle → 500 ms → slider | 25.274 | 0.000000 |
| Circle → 800 ms → slider | 162.582 | 0.000000 |
| Long break → next circle | 6.571 | 0.000000 |
| Slider → hidden future slider | 15.964 | 0.000000 |

An additional orientation-only pair keeps the future slider head and all subsequent objects identical, changing only its curve. Its pre-appearance difference is also zero. The earlier mixed-lookahead baseline is not claimed as an orientation-only measurement.

Left/right acquisitions first differ by more than 0.001 px about 2–4 ms after appearance at a 2 ms sampling interval. This is trajectory growth/numerical threshold, not an inserted delay. A prepared clock 12 ms ahead and speed factors 0.75/1/1.5/2 do not leak coordinates early.

## 5. Micro-gap behavior

Previously an immediate future-target plan and its outgoing-tangent lookahead could reveal the future object early. Hidden 300–800 ms spaces now preserve the known stroke and decay locally, then bend into acquisition after perception. The promotion is part of the existing intent, not an extra settle/fidget/acquisition chain.

## 6. Emitted dynamics: cause and before/after metrics

The most important snap source was a timestamp mismatch: a new curve started at the current callback time, but its captured position belonged to the previous callback. The first evaluation repeated that position, then resumed movement. Stored analytic p/v/a looked continuous while actual finite-difference output had a stop/restart spike.

New curves now start at the time represented by their captured state. Slider entry/handoff validation uses that same duration. Normal circle acquisition mathematics remain in place. High terminal derivatives from sliders/spinners are transferred locally within the next curve rather than amplified across its entire duration. Spinner endpoint derivatives are similarly localized inside the approach.

Numbers below are before → after. Acceleration is in thousands of px/s²; jerk is in millions of px/s³. Position-derived finite differences use 2 ms samples.

| Scenario | Peak speed | Peak acceleration | Peak jerk | P95 jerk |
|---|---:|---:|---:|---:|
| Circle gap 300 ms | 1186 → 1342 | 236.34 → 13.60 | 236.27 → 0.45 | 0.177 → 0.227 |
| Circle gap 500 ms | 1029 → 1042 | 141.81 → 8.30 | 141.79 → 0.34 | 0.112 → 0.128 |
| Circle gap 800 ms | 1029 → 1042 | 88.66 → 8.29 | 88.65 → 0.34 | 0.104 → 0.119 |
| Circle → short slider | 1294 → 1277 | 108.48 → 26.99 | 108.47 → 9.90 | 0.918 → 0.843 |
| Slider → circle | 1671 → 1677 | 105.55 → 19.54 | 105.54 → 2.30 | 0.432 → 0.378 |
| Slider → slider | 1530 → 1517 | 92.16 → 42.03 | 92.09 → 21.26 | 0.624 → 0.601 |
| Dense mixed | 482 → 517 | 106.44 → 12.41 | 106.41 → 6.21 | 0.696 → 0.448 |
| Break 3 seconds | 1126 → 1138 | 75.16 → 9.04 | 75.05 → 0.31 | 0.150 → 0.128 |
| Break 5 seconds | 1126 → 1124 | 75.27 → 8.72 | 75.14 → 0.34 | 0.122 → 0.107 |
| Break 6 seconds | 1126 → 1125 | 79.19 → 8.74 | 78.60 → 0.48 | 0.134 → 0.116 |
| Spinner entry/body/exit | 3152 → 3668 | 1015.45 → 449.59 | 1012.94 → 54.89 | 6.80 → 42.30 |

Acceleration-direction reversals fell from 2 to 0 in each circle-gap case, 125 to 1 in the dense sample, 18/28/30 to 0 across the break samples, and 2 to 0 in the spinner sample. Plan counts stayed unchanged: 4 in each circle-gap case, 125 dense, 16/26/30 for breaks, 5 in the spinner sample. Same-intent perception promotions are separately counted, not disguised as per-frame replanning.

Not every metric improved: P95 jerk is slightly higher in circle gaps and substantially higher in this spinner sample. Wider, more eccentric orbits at genuinely bounded polar speed require more sustained radial/centripetal motion. The removed stop/restart spikes are not the same thing as that sustained motion. The broadest spinner styles remain a priority for visual evaluation; this report does not claim they are universally softer.

## 7. Slider economy and breaks remain intact

The 120 ms tiny curve uses 13.02 px of hand travel for 55 px of ball travel; the loop uses 13.94/81.97; the repeat uses 13.94/76. Long tracking remains substantial at 386.21/439.99. The dense mixed-slider sample uses 162.87/671.96; consecutive short sliders use 63.50/659.96.

The new dense stress completes 1,200/1,200 sliders across 3,600 objects with zero predicted head/follow/exit/release failures and zero discontinuities; minimum follow margin is 24.30 px. Timing/accuracy and key-hold handling were not changed.

At AR5, nearby break acquisition starts 278 ms before the next note for 3/5/6 second breaks; far targets start 574/598/606 ms before. Quiet mode starts at 558 ms for the five-second far-target case. All have zero early camping and continuous handoff. At higher AR, acquisition cannot begin before appearance even if more travel time would otherwise be desirable.

## 8–9. Spinner direction

The old session sign was sampled but never actually inverted. Also, the UI named positive angles counter-clockwise even though playfield Y increases downward: positive is visually clockwise.

`begin_session()` now explicitly multiplies the legacy selected session sign by `-1` once. Acquisition and all spinner bodies consume only `m_session_spinner_direction`. The UI labels are corrected. Tests reproduce the old seed draw and verify its opposite across all four spinners in each of 24 sessions, with no within-spinner or between-spinner flips.

This is an inversion of the old **seeded session choice**, not a fixed clockwise/counter-clockwise choice across different sessions. Fresh sessions still have fresh seeds.

## 10–12. Stable shape character and measured diversity

`session_seed + object identity` produces one cached style. It includes a continuous radius personality, eccentricity, full-angle orientation, small centroid bias/drift, slow orientation change, radial asymmetry, and independently shaped but softly correlated radius/aspect/RPM knot profiles. These knots interpolate smoothly; the trajectory does not repeat one fixed period or reroll each frame.

Across 512 emitted eight-second spinners:

- Mean radius: **40.22–102.98 px**.
- Radius buckets (<52 / 52–65 / 65–78 / 78–91 / >=91 px): **101 / 109 / 110 / 130 / 62**.
- Radial standard deviation: **3.36–26.47 px**.
- Observed covariance major/minor ratio: **1.070–1.504**.
- Principal-axis orientation: **−89.7° to +89.6°**.
- Centroid offset: **0.12–3.38 px**.
- Clear net expansion/contraction: **210 / 207 styles**.

Normal generated styles stay inside the 20–162 px safety region without touching the guards. A starting cursor outside that region is brought inward continuously rather than snapped to the boundary.

## 13–14. RPM UI/config and migration

The compact existing controls now show **Spinner RPM Minimum** and **Spinner RPM Maximum**, retaining the 200–477 UI span. Raising the minimum beyond the old maximum raises the maximum; the maximum slider cannot cross below the minimum. Equal endpoints intentionally permit fixed speed.

Fresh defaults are **400–440 RPM**. New keys are `autobot.spinner_rpm_min` and `autobot.spinner_rpm_max`. An old `autobot.spinner_rpm` migrates to ±5%, capped at ±20 RPM and the existing runtime bounds. Thus 360 becomes 342–378; 420 becomes 400–440; 477 becomes 457–477. Explicit new endpoints win regardless of file order. Partial, reversed, non-finite, and out-of-range values are tested. Other profile settings are unchanged.

The active range/style is captured when that spinner is first acquired, allowing the approach to match it; changing UI settings affects the next uncached spinner. Active range and observed polar RPM min/max are shown in the existing diagnostic fold.

## 15–17. RPM behavior, bounds, and RNG lifetime

Each spinner has a stable base within the range and smooth non-periodic drift around it. Radius expansion has a small calming contribution, but an independent style sample dominates: radius does not determine RPM. All interpolation stays inside the range. Integration uses `RPM * 2*pi / 60` and real controller seconds.

The orbit is evaluated as a ray/ellipse intersection around the actual spinner center. Therefore ellipse shape/centroid changes cannot multiply actual polar angular velocity. Diagnostics and tests measure angles from emitted positions; the integrated reference is checked separately.

| Requested range | Duration | Measured emitted RPM |
|---|---:|---:|
| 330–390 | 0.35 s | 364.224–364.340 |
| 330–390 | 20 s | 362.677–384.543 |
| 200–240 | 6 s | 217.618–223.271 |
| 430–477 | 6 s | 439.880–440.900 |
| 359–361 | 8 s | 359.280–359.619 |
| 200–477 | 8 s | 271.752–356.413 |

Instantaneous commanded RPM stays in range; emitted-angle measurements also pass the bounds, allowing 0.08 RPM of floating-point measurement tolerance. No per-frame RNG is used. The tests verify the style arrays remain unchanged throughout evaluation.

## 18–20. Stress, regressions, build

Seven suites pass with **91,206 checks, zero failures**: motion 597; slider hand/break 128; perception/dynamics 56; spinner matrix 90,097; config migration 13; accuracy 303; Aim Assist sliders 12. Many spinner checks are per emitted frame, not separate scenarios.

Spinner coverage includes short/long, consecutive, low/high/narrow/wide ranges, 512 styles, 96 session spinners, 0.32/1/4/8/16 ms updates, and 0.75/1/1.5/2 speed factors. Observed revolutions match integrated RPM; representative error stays below 0.001 revolution. No direction reversal or detected discontinuity occurred. Prepared spinner entry differs from the incoming second-order p/v/a prediction by at most 0.278 px in the session matrix. Outside-radius entry is tested separately.

The existing 23,988-transition stress remains free of suspicious transitions. The same-Y arrival-cone regression still reproduces the old failure when deliberately disabled and clears it when enabled. Its test now uses AR5 so the relevant lookahead is visible. The large-loop assertion allows a legitimate short safe-tail hold while requiring tracking dominance and substantial emitted travel. Spinner first-step tests use a prepared bounded-RPM entry rather than assuming a low-speed ramp from an incompatible initial velocity.

Release x64 builds successfully to `TenshiZ/builds/x64/Release/TenshiZ.exe` with no compiler warnings or errors. All seven tests were rebuilt with the x64 C++ toolchain. Accuracy, controlled 100/50 behavior, Perfect Play tests, and Aim Assist tests pass. No accuracy/Relax, Aim Assist, input, resolver, or anti-cheat code was modified in this pass.

## 21. Live checks still needed

1. Compare the displayed AR preempt with the actual map/mod setup on Stable and Lazer, especially high AR and speed changes. This uses the supplied beatmap AR, not visual framebuffer detection; renderer-specific visibility/occlusion mods are not newly modeled.
2. Record 300/500/800 ms gaps before sliders and a 3–6 second break. Check that the cursor waits for appearance, then acquires without missing the head.
3. Inspect the widest/highest-eccentricity spinner styles at high RPM: their sustained jerk increased in the measured sample, even though transition spikes fell sharply.
4. Confirm the direction visually. The inversion is per seeded session, and the UI's clockwise labels now match Y-down screen coordinates.
5. Check very short spinners, immediate spinner-to-note exits, and pause/resume or manual reanchor mid-spinner. OS pixel quantization, input stalls, and live scoring were not exercised by these offline tests.

## Files changed in this pass

- `core/autobot/motion_core.hxx`: perception, commitment, timestamp alignment, spinner style/RPM/direction and diagnostics.
- `core/autobot/autobot.hxx`: new range settings and actual game-time perception argument; output/input handling unchanged.
- `core/autobot/spinner_settings.hxx`: shared range validation/migration helpers.
- `impl/config/config_store.hxx`: range keys and migration; stream reader permits isolated tests.
- `impl/ui/overlay.cxx`: range controls and compact existing-fold diagnostics.
- `tests/motion_tests.cpp`: updated isolation/prepared-entry assertions described above.
- New `tests/perception_motion_tests.cpp`, `tests/spinner_polish_tests.cpp`, `tests/spinner_config_tests.cpp`, and this report.
