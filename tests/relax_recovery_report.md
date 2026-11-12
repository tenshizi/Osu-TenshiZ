# Normal Relax recovery

Superseded: the user reported further live failures. The replacement dispatcher
described below was subsequently removed; see `relax_full_rollback_report.md`.

The cursor-aware experiment was removed from the production route after the user
reported missed bursts and broken slider holds. Its geometric approximation and
per-clock-sample head filtering were not reliable enough for external Relax.
`normal_relax.hxx` remains only as a retired test fixture; `relax.hxx` owns Normal
again. This restores scheduled Normal behavior, not native-mod equivalence.

Normal once again anchors each target to its own beatmap timestamp and applies
the configured timing/variation/tap settings. Slider holding uses the decoded
duration, so short, large, repeating and long sliders do not depend on estimated
cursor/ball overlap. The corrected live geometry/difficulty readers and Windows
input compatibility changes remain in place.

The Normal output dispatcher now expires old holds before new heads, creates a
fresh press edge for every head, preserves a key while another active slider owns
it, and restores active holds after focus recovery. Collision handling no longer
truncates Normal tap entries into zero-duration taps when both keys are occupied.
Smart and Autobot retain their existing dispatch/collision paths.

Validation:
- Actual captured key events: 33,087 checks passed, covering 1/20/50/75/100/150 ms
  note intervals, short through 12-second sliders, two overlapping sliders plus
  a burst, and focus-loss release/resume.
- A 2,000-object stream retains independent jitter. Dropping the first tap does
  not change any of the following 1,999 dispatch timestamps.
- Autobot accuracy controller: 303 checks passed.
- Mode routing and historical cursor tests: 4,684 checks passed. These historical
  cursor tests do not establish production gameplay correctness.
- Release x64 rebuilt with v143 and no debug symbols, then copied to Release.

Live confirmation on the user's failing map is pending.
