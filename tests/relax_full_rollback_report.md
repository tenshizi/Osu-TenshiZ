# Full pre-rewrite dispatch restoration

The first recovery build still contained a replacement Normal dispatcher. The
user reported that bursts/sliders continued failing later in the map. That build
therefore did not resolve the reported gameplay regression.

This build restores the pre-cursor-rewrite `flush_queue_at()` body and the original
key-busy/collision path for Normal. The Normal-specific queue-owner dispatcher,
extra foreground/stall branch, and its associated state were removed. Normal,
Smart and Autobot use their original dispatch routes again. The previous stream
timestamp/jitter fixes, user Smart/Autobot changes, input API compatibility,
lazer readers and config text-field fix are preserved.

Tests were adjusted to reflect restoration rather than the abandoned dispatcher's
new behavior: the common mixed slider/burst case is tested; two occupied protected
keys and automatic re-holding after an explicit release are no longer asserted as
features of the restored dispatcher. The stream test checks a delayed first tap
without shifting later taps, matching the original dispatcher rather than the
abandoned expired-event dropping policy.

Validation: normal emitted-input baseline checks pass; independent stream timing
and delayed-first-note recovery pass; Autobot accuracy tests pass. Release x64
build succeeds and the staged/usual executable hashes match. A read-only process
check found no helper instances running, so duplicate helper processes were not
the current cause.

Live gameplay success is not yet confirmed. This is a full dispatch rollback,
not a claim that native Relax has been reproduced or every gameplay miss fixed.
