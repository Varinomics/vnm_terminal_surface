# Flat Ring History Phase 3 Evidence: Active-To-Retained Record Producer

## Phase 3 Scope

Phase 3 consolidates active-grid scrollout and accepted repaint recovery behind
one retained-row producer while current deque-backed retained storage remains
authoritative.

Implemented producer contract:

1. `seal_retained_row_record` is the single producer for retained row records.
2. Normal scrollout and accepted recovery append through `append_scrollback_row`.
3. Produced records carry canonical cells, retained provenance, row-local
   hyperlink identity keys, source width, wrap metadata, and style lifetime
   metadata.

Non-goals preserved:

1. No byte ring.
2. No authoritative storage cutover.
3. No viewport, selection, public projection, resize projection, or recovery
   acceptance-policy change.

## Style Lifetime Policy

Phase 3 chooses session-lifetime style ids for produced retained-row values.
The existing screen model interns styles into `m_styles`, appends new styles,
and does not reclaim or mutate style entries. Retained cells therefore keep
`Terminal_style_id` references and records advertise
`SESSION_LIFETIME_STYLE_ID`.

Phase 4B owns encoding those ids into the ring row codec. If a future codec
needs row-local styles or a versioned append-only style catalog instead, that is
a format decision for Phase 4B and must not reintroduce a second producer.

## Wrap Metadata Policy

Phase 3 records the current retained-storage wrap contract as
`HARD_BOUNDARY`. Current retained rows are stored and materialized as
independent terminal rows; no row-local soft-wrap continuation signal exists in
current storage. The producer also records the source width at seal time.

Resize projection migration remains out of scope for Phase 3.

## Phase Gate

| Gate | Phase 3 outcome |
|---|---|
| Scope | Active-grid scrollout and accepted repaint recovery now use one retained-row producer targeting current storage. No byte ring, storage cutover, viewport/selection/public projection policy change, or resize projection migration was implemented. |
| Behavior axis | Normal scrollout keeps terminal-storage provenance and existing retained identity; accepted recovery receives recovered provenance inside the shared producer. Produced records carry canonical cells, row-local hyperlink identity keys, session-lifetime style ids, source width, and hard-boundary wrap metadata. |
| Recovery state | Accepted recovery preserves candidate row-local hyperlink identity keys before repaint mutation can prune active hyperlink maps, then passes those keys into the shared producer. Recovery-disabled behavior remains covered by existing screen-operations recovery-disabled tests. |
| Evidence | `cmd.exe /d /s /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"" x64 && cmake --build build --target vnm_terminal_backend_session vnm_terminal_screen_operations --config Debug && ctest --test-dir build -R ""vnm_terminal_(backend_session\|screen_operations)"" --output-on-failure"` passed on 2026-05-30; `vnm_terminal_screen_operations` and `vnm_terminal_backend_session` both passed. |
| Baseline outcome | Focused backend/session coverage stayed green. The new screen-operations Phase 3 test proves normal scrollout and recovered rows round-trip equivalent content, hyperlink metadata, style ids, source width, wrap metadata, and source-specific provenance through current storage. Existing recovery-disabled tests in the same target stayed green. |
| Exit predicate | No recovery-only append path bypasses the producer. Normal and recovered retained rows seal through `seal_retained_row_record`; producer output contains every Phase 3 field needed by the later row codec while current storage remains authoritative. |
| Deletion ownership | Phase 3 deletes the old `make_scrollback_row` builder path and removes recovery's separate pre-append provenance stamping path. Current-storage hyperlink refcount maps remain because retained deque storage still needs them; Phase 5B owns deletion when row-local retained hyperlink metadata becomes authoritative. |
| Rollback mechanism | Revert this Phase 3 change set: `terminal_screen_model.h`, `terminal_screen_model.cpp`, `model_ops_tests.cpp`, this evidence artifact, and the README entry. |
| Split triggers | If source width or wrap metadata needs user-visible projection behavior, split to Phase 5A. If row-local style tables or a versioned style catalog replace session-lifetime style ids, split to Phase 4B. If byte-ring storage or authoritative storage cutover becomes necessary, stop and move to Phase 4A or Phase 6B respectively. |
