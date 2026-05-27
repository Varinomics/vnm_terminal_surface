# Selection Lifetime Phase 2 Content Basis

## Owner and producer

- Owner field: `Terminal_session::m_selection_content_basis`
- Type: `terminal_selection_content_basis_t`
- Producer modules: `src/terminal_screen_model.cpp` and `src/terminal_session.cpp`
- Phase 2 consumer: `Selection_contract_controller` visual leases

The basis is internal selection metadata. It is not public API, and it is not
owned by renderer or QSG code.

## Fields

- `content_generation`: advances when the session publishes terminal content
  with a different text/source identity.
- `grid_reflow_generation`: advances when the session publishes a grid or
  reflow basis change.

`Terminal_screen_model_result` supplies explicit semantic producer flags:

- `terminal_content_changed`
- `active_buffer_changed`
- `grid_reflow_changed`

Dirty rows are paint invalidation data only. They are not a content-basis input
and do not prove terminal content changed.

The session consumes the explicit model flags in
`Terminal_session::advance_selection_content_basis_for_model_result()` before
publishing backend-output, synchronized-release, resize/reflow, and scrollback
limit snapshots. `Terminal_session::set_selection_range()` then stamps new
leases from the current basis through `make_selection_visual_lease()`.
`test_selection_phase2_session_lease_basis_advances` proves that published
content mutation, active-buffer transition, normal grid reflow, and
synchronized-output resize geometry snapshots are visible in newly captured
lease bases.

`terminal_selection_visual_lease_t` records the source content basis, session
epoch, buffer id, grid/reflow basis, viewport mapping, selected range,
endpoints, and durable/provisional payload identities. Durable selected text is
stored separately from this visual attachment data.

## Increment and preserve matrix

| Event | Content basis rule |
| --- | --- |
| Terminal content mutation | Increment `content_generation` when published. |
| Cursor-only updates | Preserve both fields. |
| Viewport scroll | Preserve both fields; the lease records viewport mapping separately. |
| Synchronized-output hold | Preserve held content basis while output is unpublished; synchronized resize geometry snapshots still follow the resize/reflow rule. |
| Synchronized-output release | Increment `content_generation` if the release publishes held content. |
| Resize/reflow | Increment `grid_reflow_generation` when the grid/reflow snapshot is published, including synchronized-output resize geometry snapshots; increment `content_generation` only for a model content mutation. |
| Font/DPR changes | Preserve both fields; these are render geometry/cache concerns. |
| Alternate-buffer transitions | The model reports `active_buffer_changed`; the session increments `content_generation`. The lease also records `buffer_id`, which Phase 5 will consume for compatibility. |
| Scrollback append | Increment `content_generation` when published. |
| Scrollback eviction | Increment `content_generation`; selected rows evicted from the lease become payload-only internally. |
| Reset/new session | Clear durable payload and visual lease data, reset the basis fields, and advance the internal session epoch. |

Conservative no-op erases may advance `content_generation` in Phase 2. This is
allowed fail-closed behavior: an over-advanced basis can detach visuals later,
but it cannot preserve stale attachment over incompatible content. A narrower
no-op proof belongs with the Phase 5 compatibility predicate if needed.

## Phase 2 deferrals

Phase 2 recorded state and lease data only. Phase 3 later bound drag
hit-testing to a published snapshot source, and Phase 5 later consumed the
basis for compatibility checks and span filtering. The Phase 1 stale-span
repros that were expected-red during Phase 2 have since been promoted to normal
green coverage.

Historical Phase 2 status: the custom backend/session and surface-host runners
executed the stale-span repros in expected-red evidence mode while later phases
owned the visual compatibility predicate. Their wrappers tolerated only the
named intended red predicate. Setup, payload-retention, copyability, and
publication prerequisites remained ordinary failures in default mode.

Phase 5 status: the stale-span repros were promoted to normal green tests:

- `mutating selection detaches spans instead of highlighting replacement text`
- `sync-release detaches spans instead of highlighting published replacement text`
- `surface mutation-detach emits no stale highlight over replacement text`

Strict focused backend/session and surface-host tests now pass with
`VNM_TERMINAL_STRICT_PHASE1_RED_REPROS=1`. Phase 3 already promoted the
snapshot-changing drag repro by binding surface drag hit-testing to a published
snapshot source. Phase 4 implemented payload extraction/source proof, and
Phase 5 filters committed spans through the lease compatibility predicate.
