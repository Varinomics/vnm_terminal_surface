# Recovery baseline correction

Status: closed by Phase R1-R5.

This artifact records the top-level amendment that changed the original
primary-backing plan from "delete the repaint recovery path" to "preserve and
restructure it as optional Phase R policy". It is no longer an open gate.
Current implementation guidance lives in:

1. `phase_r1_recovery_provenance_and_resize_guard.md`
2. `phase_r2_recovery_delta_viewport_acceptance.md`
3. `phase_r3_recovery_proposal_metadata.md`
4. `phase_r4_recovery_shift_helper_extraction.md`
5. `phase_r5_recovery_disable_switch.md`

## Decision

The primary repaint recovery policy is preserved as product behavior, but it is
not canonical protocol scrollback and is not storage evidence for core backing,
viewport, resize, selection, or publication correctness.

Phase R restructures the policy on top of the canonical backing model:

1. Accepted recovered rows carry `RECOVERED_PRIMARY_REPAINT` retained-row
   provenance.
2. Accepted recovered appends flow through the normal primary-history delta
   path.
3. Recovery proposals are explicit internal metadata.
4. Shifted-repaint matching lives in `terminal_repaint_recovery.h/.cpp`.
5. The public surface owns the platform default and exposes an explicit disable
   switch; the app supplies only an override.

## Defaults

`Terminal_session_config::recover_scrollback_from_primary_repaints` and
`Terminal_screen_model_config::recover_scrollback_from_primary_repaints`
default to `false`. `VNM_TerminalSurface` enables primary repaint recovery by
default on Windows and disables it by default elsewhere.

Transcript replay preserves a recorded recovery flag. If a transcript lacks
the flag, replay defaults recovery to disabled rather than inferring the
surface platform default.

## Guard Rule

The Phase 0B guard remains a static CTest check for accidental
recovery-as-storage-evidence creep. It is allowed to contain Phase R-owned
recovery vocabulary, but new recovery vocabulary outside a named Phase R gate
is still a review failure.

## Exit Predicate

This amendment is complete when the Phase R artifacts remain the current
source of truth, core backing tests can run with recovery disabled, and any new
recovery behavior is explicitly owned by a Phase R gate.
