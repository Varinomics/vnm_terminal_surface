# Phase 0B primary backing guards

This is the Phase 0B gate artifact for
`primary_backing_buffer_consolidated_design.md`.

Phase 0B depends on `recovery_baseline_correction.md` and Phase 0A. The guard
must be evaluated against the recovery policy/config surface recorded by that
baseline correction, not against stale historical recommendations to delete or
permanently reject recovery.

Phase 0B is behavior-preserving. It adds no production observer, no runtime
diagnostic event, no storage owner, no viewport panning, no row promotion, and
no recovery change. The executable artifacts in this batch are a static review
guard and a test-only observation seam:

`tools/primary_backing_phase_0b_guard.py`

`tests/helpers/primary_backing_observation.h`

The guard is a CTest static check and production code does not include it. The
observation seam is included only by model tests and cannot change production
runtime behavior.

Without `--diff`, the guard scans the current tree for recovery-as-storage-
evidence vocabulary only. With `--diff`, it scans added diff lines for both
recovery-as-storage-evidence vocabulary and common current-storage mutation
shapes. It deliberately does not claim a full-tree storage-mutation policy.

## No recovery-as-storage-evidence foundation rule

Until deferred Phase R recovery-policy work, primary backing work must not use
repaint inference, transcript replay, text matching, source switches, or
fallback row producers as storage evidence. The existing audited
repaint-recovery heuristic remains in place with the Phase 0A defaults, but it
is not a foundation for storage, viewport, resize, selection, or publication
correctness.

Allowed existing recovery references are limited to exact baseline lines in the
Phase 0A audited paths. The full-tree guard also checks occurrence counts for
those allowed lines, so copying an allowed line elsewhere in the same file is a
guard failure rather than a back door.

1. `include/vnm_terminal/internal/session_contract.h`
2. `include/vnm_terminal/internal/terminal_screen_model.h`
3. `src/terminal_screen_model.cpp`
4. `src/terminal_session.cpp`
5. `src/terminal_transcript.cpp`
6. `src/vnm_terminal_surface.cpp`
7. `tools/transcript_replay/terminal_transcript_replay.cpp`
8. `tests/helpers/primary_backing_test_config.h`
9. `tests/transcript/transcript_tests.cpp`, only for transcript schema and
   replay compatibility tests of the recorded recovery flag.

Post-Phase R amendments extend the allowlist only for the lines owned by their
gate artifacts. Phase R1-R4 own the recovered-row provenance, delta acceptance,
proposal metadata, and shift-helper extraction lines. Phase R5 owns the public
disable switch, live session/model propagation, pending-candidate cancellation
on disable, and transcript replay default consistency lines. The guard remains
a recovery-creep review tool, not evidence that recovery is absent from the
tree.

Any new recovery, repaint-inference, transcript-replay, text-matching,
mirror-storage, fallback-source, fallback-row-producer, `_legacy`, or `_v2`
vocabulary outside an owning future phase must be treated as a guard violation.
This includes space, hyphen, and underscore spellings such as
`repaint inference`, `repaint_inference`, `text matching`, `text_matching`,
`fallback row producer`, and `fallback_row_producer`. A later phase can amend
this allowlist only by publishing its gate table, behavior axis, recovery state,
and deletion or promotion gate first.

## Current storage member diff mutation guard

During no-behavior phases, added code must not extend mutation of the current
storage members unless the batch is explicitly an extraction phase with a named
owner and rollback path:

1. `m_scrollback`
2. `m_rows`
3. `m_primary_buffer`
4. `m_alternate_buffer`

In `--diff` mode, the Phase 0B guard scans added diff lines for these member
names in common mutating shapes: direct assignment, assignment through simple
`[]` indexing, and mutating container calls such as `assign`, `clear`, `erase`,
`insert`, `push_back`, `pop_back`, `resize`, and `swap`. Read-only indexing and
comparisons are not mutation violations by themselves.

A tree scan does not run this storage mutation predicate; without `--diff`, it
checks vocabulary only. This is a review predicate, not semantic proof. If it
flags a line, the batch must either remove the line, classify it under an
owning extraction phase, or update this document with the exact gate that owns
it.

The guard is intentionally not a whole-file recovery allowlist. Audited
recovery compatibility files may contain only their recorded baseline recovery
lines. New recovery vocabulary in the same files must still fail unless a later
phase updates this document with an owning gate.

## Minimal observation vocabulary

This batch records only the vocabulary for future read-only observations. No
production code emits these observations in Phase 0B.

`Primary_backing_boundary` names stable boundaries where a later test-only
observer may sample state:

1. `ingest`
2. `resize`
3. `alternate_enter`
4. `alternate_leave`
5. `scrollback_clear`
6. `scrollback_limit_change`

`Primary_backing_observation` is a documentation-only schema placeholder:

| Field | Meaning |
| --- | --- |
| `boundary` | One `Primary_backing_boundary` value. |
| `active_buffer` | Primary or alternate at the sample point. |
| `scrollback_rows_before` | Retained primary history count before the boundary. |
| `scrollback_rows_after` | Retained primary history count after the boundary. |
| `offset_from_tail_before` | Viewport tail offset before the boundary when available. |
| `offset_from_tail_after` | Viewport tail offset after the boundary when available. |
| `recovery_enabled` | Exact recovery flag value used by the fixture. |
| `classification` | `read_only`, `test_only`, or `debug_only`. |

The schema is intentionally not a runtime contract. The current test-only
observer in `tests/helpers/primary_backing_observation.h` samples model state
before and after a caller-provided operation and returns only the model result
and sampled counts. It may be promoted beyond tests only if a later phase gate
proves the observer remains read-only and cannot alter release behavior.

## Phase 0B gate table

| Gate entry | Phase 0B value |
| --- | --- |
| Scope | Phase 0B smallest unambiguous slice: written recovery-as-storage-evidence guard predicates, diff-only current storage member mutation predicate, test-only observation seam, and a CTest-wired static review guard. |
| Behavior axis | `none`. |
| Recovery state | Production defaults unchanged; Phase 0A recovery-disabled core-test helper unchanged; existing repaint-recovery heuristic unchanged. |
| Evidence | This document, `tools/primary_backing_phase_0b_guard.py`, its CTest entry, and `tests/helpers/primary_backing_observation.h`. The guard is a static review predicate, and the observer is a test-only seam. |
| Baseline outcome | No production source, test fixture behavior, recovery default, transcript replay behavior, storage owner, viewport behavior, resize behavior, selection policy, renderer behavior, DSR behavior, or public scroll behavior is changed. |
| Exit predicate | No recovery-as-storage-evidence foundation rule is documented; guard violations point to the audited recovery paths or an owning future phase; observation schema remains inert. |
| Manual gate | `none`. |
| Rollback mechanism | Remove this document, `tools/primary_backing_phase_0b_guard.py`, and `tests/helpers/primary_backing_observation.h` plus tests that depend on that seam. Reliable ledger entries remain. |
| Deletion gate | Phase R may replace recovery allowlists with a recovery-policy area. Phase 10 may remove or promote the static guard after final storage, viewport, publication, selection, and Phase R recovery-policy boundaries are stable. |

## Deferred Phase 0B work

This batch deliberately does not add a production runtime observer or diagnostic
event. A future phase may promote the test-only observation vocabulary only with
a gate table that proves the promoted observer is read-only and names its
deletion or promotion rule.
