# Selection Lifetime Phase 4 Payload Source

## Chosen approach

Phase 4 uses direct payload extraction from the published
`Terminal_render_snapshot` source for snapshot-visible selections and all
surface drag selections. Backend/session selections that name published
logical rows outside the visible snapshot retain a bounded active-model
fallback only after an explicit source proof.

## Payload-producing paths

- Backend/session range setting calls
  `Terminal_session::set_selection_range()`. The method resolves the current
  published source identity and passes it as an explicit session-internal
  source proof to the same capture path as surface drag. If the range is
  outside the visible snapshot, it can use active-model extraction only when
  the active model is proven to be the same published content source.
- Surface drag selection records the published source identity used for
  hit-testing, verifies Phase 3 content compatibility through the drag, and calls
  `Terminal_session::set_selection_range_from_published_source()`.
- Synchronized-output-related selection can only capture from the last
  published snapshot while output is hidden. Hidden model mutations are not a
  payload source.
- Alternate-buffer selection captures from a published snapshot whose source
  identity names the active buffer.
- Scrollback selection captures from visible published scrollback rows through
  the snapshot viewport mapping, or from the active model only for
  backend/session non-visible rows with the source proof below.
- When the selected range is present in the published snapshot, wide,
  trailing-wide, combining-mark, and variation-sequence payloads are read from
  `Terminal_render_cell` clusters in that snapshot. Wide continuation cells
  emit no independent text; base cells carry combining marks and variation
  selectors in `Terminal_render_cell::text`. Fallback paths use the proven
  model extraction semantics instead.

## Source identity proof

The checked identity is `terminal_selection_source_identity_t`:

- `source_content_basis`
- `session_epoch`
- `buffer_id`
- `grid_reflow_basis`
- `grid_size`
- `viewport_mapping`

`Terminal_session::published_selection_source_identity_unlocked()` derives this
identity from `m_latest_render_snapshot` and the session content basis. Before
capturing a payload from a snapshot,
`set_selection_range_from_published_source_locked()` checks that any
caller-supplied source identity matches the current published snapshot identity
exactly, including viewport mapping. Payload text is then extracted by
`selected_text_from_render_snapshot()` from that same snapshot only when the
full selected range is present in the snapshot. The snapshot helper returns
`INVALID_RANGE` instead of truncating to visible rows.

The selected row and cluster descriptors are therefore the snapshot cells used
for extraction, not a later model view.

The backend/session and surface-drag active-model fallback is allowed only for
logical ranges that are not fully present in the current snapshot. There is no
model fallback without an explicit expected source identity: backend/session
range setting uses the current published source as a session-internal expected
identity, and surface drag supplies the Phase 3 drag source. For a
caller-supplied drag source, the supplied identity must remain
content-compatible with the current published source; viewport mapping may
differ because local scrollback drag can intentionally extend a logical
selection beyond the current visible rows. All of these checks must hold:

- render publication is not blocked;
- the published source content basis equals `m_selection_content_basis`;
- the source session epoch equals `m_selection_session_epoch`;
- the source buffer equals `Terminal_screen_model::active_buffer_id()`;
- the source grid/reflow basis equals the current grid/reflow basis;
- the source grid equals `Terminal_screen_model::grid_size()`;
- the source viewport mapping equals the current viewport-controller state;
- the selected range is valid for the active model.

The active model may contain hidden synchronized-output mutations, but
`model_allows_render_snapshot()` rejects that state, so hidden cells are not
consulted while the selection payload is captured.

## Rendered-frame binding note

Surface hit-testing currently uses the GUI-thread installed published snapshot
stored in `VNM_TerminalSurfacePrivate::render_snapshot`. `sync_from_session()`
installs that snapshot and marks the session snapshot generation synced before
the next `updatePaintNode()` call has necessarily painted it. The surface keeps
`last_rendered_snapshot_sequence` for diagnostics, but it does not retain a
last-painted `Terminal_render_snapshot` handle that selection hit-testing can
bind to without changing render ownership.

Phase 4 therefore binds payload capture to the installed published snapshot,
not a separately retained last-painted frame. This remains source-safe for
payload extraction because the installed snapshot is a published source and
hidden synchronized output is still blocked. A later visual-validation phase
should add a painted-frame identity or an explicit gate if product validation
requires mouse hit-testing to ignore installed-but-not-yet-painted snapshots.
Manual validation should include dragging while backend output publishes between
GUI sync and QSG paint, then confirming the selected payload source matches the
user-visible frame or that selection is delayed until the installed snapshot is
painted.

## Phase 5 status

Phase 4 left stale-span predicates to the visual lease phase. A retained payload
may outlive visual attachment; Phase 4 only proves the copied payload source.
Phase 5 has since implemented the compatibility predicate that suppresses stale
highlights after row mutation, synchronized-output release, resize/reflow,
alternate-buffer switch, and scrollback eviction.

## Render-time cost

There is no render-time full selected-text comparison. Snapshot payload
extraction runs only when a selection payload is created or replaced, and its
work is bounded to the selected range.

## Phase 4 gate evidence

This evidence records the Phase 4 payload-source gate. Source, internal-header,
test, and phase-evidence documentation changes are expected for this phase. The
no-public-doc constraint means no public API or `Q_PROPERTY` changes and no
`docs/public_surface.md` changes.

- Focused build:
  `cmake --build build\validation-surface --target vnm_terminal_backend_session vnm_terminal_surface_host --config Debug`
  passed. MSBuild printed the non-fatal post-build message
  `pwsh.exe is not recognized as an internal or external command`.
- Default focused tests:
  `ctest --test-dir build\validation-surface -C Debug -R "vnm_terminal_(backend_session|surface_host)$" --output-on-failure`
  passed `2/2`.
- Strict expected-red mode at the Phase 4 gate:
  `VNM_TERMINAL_STRICT_PHASE1_RED_REPROS=1` with the same focused CTest command
  failed only the then-deferred Phase 5 stale-span predicates:
  `surface mutation-detach emits no stale highlight over replacement text`,
  `mutating selection detaches spans instead of highlighting replacement text`,
  and `sync-release detaches spans instead of highlighting published replacement text`.
- Phase 5 follow-up status: those stale-span predicates are normal green tests,
  and strict focused backend/session plus surface-host tests pass.
