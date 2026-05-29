# Phase 0A primary backing baseline

This is the durable Phase 0A gate artifact for
`primary_backing_buffer_consolidated_design.md`.

Phase 0A depends on `recovery_baseline_correction.md`, which is now closed by
Phase R1-R5. The recovery-default audit below reflects the current recovery
policy/config surface.

Phase 0A is behavior-preserving. It records the current recovery default split,
names the recovery-disabled core-test configuration, and fixes the vocabulary
that later phases must use. It does not change production runtime behavior,
storage ownership, viewport behavior, resize behavior, public projection,
selection policy, DSR behavior, public scroll behavior, or the repaint-recovery
heuristic.

## Vocabulary and boundaries

The primary row universe is retained primary history followed by exactly the
active primary grid tail. `scrollback_rows` means retained primary history rows,
not visible rows, vector capacity, public projection rows, render-snapshot rows,
or text recovered from repaint inference.

These row domains must remain separate:

1. Active-grid row: cursor-addressable primary grid row in `[0, grid_size.rows)`.
2. Primary-backing row: row in retained primary history plus the active grid
   tail.
3. Viewport row: row in the current live or public viewport projection.
4. Snapshot row: row local to one immutable render snapshot.
5. Public-projection row: row in user-visible published content, especially
   during synchronized output.
6. Selection row: selection anchor or payload row bound to an explicit domain.

Publication is not storage. During synchronized output, live storage may mutate
while public snapshots remain sourced from public data until release.

Recovery is not storage ownership. `recover_scrollback_from_primary_repaints`
is a separately gated recovery policy over ambiguous repaint evidence. Phase 0A
preserves the existing production default and heuristic. Core storage, viewport,
resize, selection, and publication tests must use an explicit
recovery-disabled configuration.

## Recovery-default audit

The current audited propagation path is:

1. `include/vnm_terminal/internal/session_contract.h`:
   `Terminal_session_config::recover_scrollback_from_primary_repaints` defaults
   to `false`.
2. `include/vnm_terminal/internal/terminal_screen_model.h`:
   `Terminal_screen_model_config::recover_scrollback_from_primary_repaints`
   defaults to `false`.
3. `include/vnm_terminal/vnm_terminal_surface.h`: the public surface owns the
   platform default, enabled on Windows and disabled elsewhere.
4. `src/vnm_terminal_surface.cpp`: process start copies the current public
   surface recovery setting into `Terminal_session_config`.
5. `src/terminal_session.cpp`: session construction copies the session config
   flag into `Terminal_screen_model_config`.
6. `src/terminal_screen_model.cpp`: resize repaint clear guarding and primary
   repaint recovery candidate capture are gated by
   `m_config.recover_scrollback_from_primary_repaints`.
7. `src/terminal_transcript.cpp`: transcript session configuration records and
   validates the recovery flag.
8. `tools/transcript_replay/terminal_transcript_replay.cpp`: replay uses the
   recorded flag when a start event supplies session configuration; the inferred
   replay configuration path defaults recovery to disabled.

Phase 0A records these defaults without changing them. On Windows public-surface
production sessions remain recovery-enabled by default. Core model/session
configs remain recovery-disabled by default unless an outer caller explicitly
enables recovery.

## Named recovery-disabled core-test configuration

The named configuration seam is
`recovery_disabled_primary_backing_session_config` and
`recovery_disabled_primary_backing_screen_model_config` in
`tests/helpers/primary_backing_test_config.h`.

The backend-session test factory now normalizes factory-constructed
backend-session tests through `recovery_disabled_primary_backing_session_config`
before enabling test traces. This is behavior-equivalent to the previous
backend-session test configuration because the session-config field already
defaulted to `false`; the helper makes the off-state explicit so future
production default changes cannot silently mask factory-path core tests.

Direct `term::Terminal_session` construction remains outside that factory
normalization. The known backend-session direct-construction exceptions are
`test_backend_output_updates_latest_render_snapshot` and the no-trace session
branch in `test_write_user_bytes_limits_and_backend_failures`. Later phases
must either route those cases through the recovery-disabled helper when that is
behavior-preserving, or keep an explicit gate that prevents production recovery
default changes from silently changing those test assumptions.

Future core storage, viewport, resize, selection, and publication tests should
use this helper or a narrower fixture that delegates to it.

## Phase 0A gate table

| Gate entry | Phase 0A value |
| --- | --- |
| Scope | Phase 0A: documentation, recovery-default audit notes, failure ledger, test configuration, and test/debug-only invariant scaffolding. |
| Behavior axis | `none`. |
| Recovery state | Production defaults unchanged; factory-path core-test override disabled through `tests/helpers/primary_backing_test_config.h`. |
| Evidence | This audit document, `primary_backing_failure_ledger.md`, and the named recovery-disabled helper used by the backend-session factory path in `tests/backend_session/backend_session_tests.cpp`. |
| Baseline outcome | Windows public surface remains default-on; core session/model configs remain default-off; factory-constructed backend-session tests now state the same default-off recovery setting explicitly. Direct-construction exceptions remain recorded as follow-up gates. Tests were not run as part of this Phase 0A edit. |
| Exit predicate | Audit records the current default path, the helper names the recovery-disabled core-test configuration, and the failure ledger assigns motivating scenarios to an owning phase or regression-only classification. |
| Manual gate | `none`. |
| Rollback mechanism | Clean revert of this document, the failure ledger, the helper, and the backend-session test-factory normalization. Production recovery defaults remain unchanged. |
| Deletion gate | Phase 10 may remove or promote the helper only after final storage, viewport, publication, selection, and Phase R recovery-policy boundaries have stable tests that keep core recovery disabled by construction. |

## Phase 0A non-changes

Phase 0A deliberately does not:

1. Change `recover_scrollback_from_primary_repaints` defaults.
2. Remove or disable the repaint-recovery heuristic.
3. Add primary backing storage.
4. Add viewport panning or a nonzero viewport origin.
5. Change resize, selection, public projection, transcript, DSR, or public
   scroll behavior.
6. Add a production mirror, fallback source switch, or operation-trace
   framework.
