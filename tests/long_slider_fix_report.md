# Long slider hold fix

Historical cursor-path repair. The reader fixes remain, but production Normal
has since returned to duration-based holding; see `relax_recovery_report.md`.

The cursor-aware Normal controller depended on geometry that the live reader did
not fully populate. Lazer repeat counts were never decoded, so reverse sliders
were evaluated as one span. The sampled-path list also had a 500-point cap, and
samples could be reinterpreted as control points using an unrelated field.

The installed 2026.1005.0 assemblies confirm Slider.repeatCount at `0x130` and
SliderPath.calculatedPath at `0x20`. The reader now converts repeat count to span
count (`repeats + 1`), preserves paths up to 65,536 vertices, and treats calculated
samples as a polyline. Missing geometry holds an already acquired head through
its known end time; it never acquires an unaimed head automatically.

For known geometry, the existing follow-area release/reacquisition rules remain
active. No additional timeout or extension past the decoded tail was added.

Validation:
- Normal Relax: 4,684 checks passed, including an eight-second four-span slider,
  every reversal, missing geometry, overlaps and release at the tail.
- Reader: 24 checks passed using synthetic memory in the test's own process,
  covering 2, 500, 501 and 2,048 sampled points and repeat-to-span conversion.
- Release x64 built with v143 and no debug symbols (local PDB tool mismatch).
- Updated executable copied to the usual Release path and hashes matched.

These tests verify decoding and key-state decisions. A live long-slider gameplay
test has not yet confirmed osu!'s resulting judgments.
