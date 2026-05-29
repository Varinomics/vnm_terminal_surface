# Phase R2 recovery delta viewport acceptance

This is the second Phase R gate artifact for
`primary_backing_buffer_consolidated_design.md`.

Phase R2 removes the compatibility source tag and scalar viewport fallback that
were kept while recovery rows lacked explicit provenance. Accepted recovered
rows now carry `RECOVERED_PRIMARY_REPAINT` retained-row provenance from Phase R1,
so their primary-history append deltas can drive viewport synchronization the
same way terminal-scroll append deltas do.

## Scope

Phase R2 owns:

1. Removing `Terminal_backing_delta_source` and the `source` field from
   `terminal_backing_delta_t`.
2. Removing `used_recovery_compatibility_fallback` from
   `terminal_backing_delta_viewport_sync_t`.
3. Letting accepted recovered primary-history append deltas update viewport
   row counts through the normal primary-history delta path.
4. Updating viewport and recovery tests that previously pinned the quarantine
   fallback.
5. Closing the Phase R1 deletion gate for `RECOVERY_COMPATIBILITY`.

## Non-goals

This batch does not remove the repaint recovery heuristic, change production
recovery defaults, change the recovered retained-row provenance source, change
transcript schema, expose provenance source in public render snapshots, or move
the recovery observer out of the model.

## Phase R2 gate table

| Gate entry | Phase R2 value |
| --- | --- |
| Scope | Retire the recovery delta source/fallback scaffold and consume accepted recovered primary-history deltas through normal viewport synchronization. |
| Behavior axis | Recovery publication/viewport reconciliation only: accepted recovery append deltas now update viewport sync from delta evidence instead of scalar fallback. |
| Recovery state | Production defaults unchanged. Recovery remains separately enabled/disabled; core storage gates remain recovery-disabled. |
| Evidence | Passed on 2026-05-29: independent read-only scout, Phase 0B diff guard, Phase 0B full-tree guard, `git diff --check`, direct-owner access search, cleanup-orphan search, R2 orphan-symbol search for `Terminal_backing_delta_source`, `RECOVERY_COMPATIBILITY`, and `used_recovery_compatibility_fallback`, focused build and CTest for `vnm_terminal_render_snapshot`, `vnm_terminal_transcript`, `vnm_terminal_capture_replay_conformance`, `vnm_terminal_viewport`, `vnm_terminal_screen_operations`, `vnm_terminal_backend_session`, and `vnm_terminal_windows_conpty_backend`, plus expanded build and CTest for `vnm_terminal_screen_basic`, `vnm_terminal_screen_operations`, `vnm_terminal_screen_alternate`, `vnm_terminal_render_snapshot`, `vnm_terminal_backend_session`, `vnm_terminal_parser_randomized`, `vnm_terminal_capture_replay_conformance`, `vnm_terminal_transcript`, `vnm_terminal_viewport`, and `vnm_terminal_windows_conpty_backend`. |
| Baseline outcome | Phase R1 accepted recovered rows with explicit retained-row provenance, but the delta stream still carried a recovery-compatibility source that forced scalar viewport fallback. |
| Exit predicate | No `Terminal_backing_delta_source`, `RECOVERY_COMPATIBILITY`, or `used_recovery_compatibility_fallback` symbol remains in code; recovered append deltas are consumed as primary-history deltas; Phase R1 true-positive and false-positive tests still pass. |
| Manual gate | `none`; this is covered by deterministic model and viewport adapter tests. |
| Rollback mechanism | Restore the delta source field, recovery fallback flag, adapter branch, and previous viewport tests only if accepted recovered deltas cannot safely drive viewport sync. |
| Deletion gate | Completed in this batch. |

## Compatibility notes

Recovery remains a policy layer and still stamps recovered retained rows. The
removed source tag was migration scaffold for viewport quarantine, not the
product recovery heuristic.
