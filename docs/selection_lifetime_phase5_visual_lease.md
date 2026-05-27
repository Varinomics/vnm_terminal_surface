# Selection Lifetime Phase 5 Visual Lease Evidence

## Compatibility predicate

Render snapshots emit `selection_spans` only when the current internal
selection state is `DRAG_PREVIEW` or `ATTACHED_VISIBLE`, a visual lease is
present, and the lease is compatible with the source being published.

The predicate requires all of these to match:

- selection content basis (`content_generation` and `grid_reflow_generation`);
- session epoch;
- active buffer id;
- grid/reflow basis;
- grid size;
- selected range;
- durable and provisional payload identities.

Viewport mapping is part of the default span-emission compatibility predicate.
The Phase 5 viewport-projection path is explicitly gated by
`Terminal_session_config::selection_viewport_projection_enabled`. When that
gate is enabled, viewport scroll is a projection change: the same compatible
lease can emit spans when selected rows are visible, emit no spans while those
rows are offscreen, and emit spans again when they become visible. Content
mutation, synchronized-output release with changed content, active-buffer
switch, scrollback eviction that crosses the selected range, and resize/reflow
detach the lease or otherwise fail closed before spans are emitted.

Synchronized resize geometry snapshots derived from the previous public
snapshot clear selection spans rather than adapting them across an unproven
grid/reflow basis.

## Performance property

Phase 5 does not compare `selected_text()` across the selected range during
render snapshot creation. The render request path compares constant-size lease,
source, range, and payload identity fields before passing a selection range to
the model for span projection.

The snapshot payload helper now builds one cell-position index per extraction
and reuses it for every selected row, instead of scanning snapshot cells once
per selected row.

## Tests

Promoted Phase 5 predicates to normal green tests:

- `test_selection_spans_detach_when_selected_row_mutates`
- `test_selection_spans_preserve_after_unchanged_synchronized_output_release`
- `test_selection_spans_detach_when_synchronized_release_mutates_selected_row`
- `test_selection_spans_detach_when_synchronized_release_moves_retained_row`
- `test_selection_visual_detach_after_row_mutation`

Added Phase 5 backend coverage:

- cursor-only updates preserve the compatible visual lease and spans;
- paint-only cursor-visibility updates preserve the compatible visual lease and
  spans;
- default viewport remapping does not project spans before the Phase 5 gate is
  enabled;
- Phase 5 viewport projection hides offscreen spans, preserves payload and
  lease, and re-emits spans when the selected rows become visible through a new
  viewport mapping.
- row-mutation visual detach publishes a later cursor-only snapshot without
  reattaching stale spans, while retained payload and public copyability remain
  correct.

Added Phase 5 surface-host coverage:

- row-mutation visual detach publishes a later cursor-only snapshot without
  reattaching stale spans, while public `selectionState`, `selected_text()`, and
  Ctrl+C keep using the retained payload.

Existing resize coverage remains green:

- `test_selection_spans_detach_when_resize_invalidates_selected_columns`

## Results

Focused build:

```bat
cmd.exe /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"" x64 && cmake --build build\validation-surface --target vnm_terminal_backend_session vnm_terminal_surface_host --config Debug"
```

Result: passed. MSBuild printed the existing non-fatal post-build message
`'pwsh.exe' is not recognized as an internal or external command`.

Default focused tests:

```bat
cmd.exe /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"" x64 && ctest --test-dir build\validation-surface -C Debug -R ""^vnm_terminal_(backend_session|surface_host)$"" --output-on-failure"
```

Result: passed `2/2`.

Strict expected-red mode:

```bat
cmd.exe /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"" x64 && set VNM_TERMINAL_STRICT_PHASE1_RED_REPROS=1&& ctest --test-dir build\validation-surface -C Debug -R ""^vnm_terminal_(backend_session|surface_host)$"" --output-on-failure"
```

Result: passed `2/2`. The Phase 5 stale-span predicates are normal green tests,
so strict mode has no remaining expected-red Phase 1 predicates in the focused
backend/surface pair.

## Residual risks

- The predicate is conservative. Any published terminal content-basis change
  detaches the visual lease unless a future row/cell descriptor proof narrows
  compatibility for unrelated mutations.
- Viewport hide/show is covered at the backend/session render-snapshot boundary;
  surface-host viewport hide/show coverage can be added if a public harness needs
  to observe that projection directly.
- Painted-frame identity remains the Phase 4 noted limitation: surface
  hit-testing binds to the installed published snapshot, not a separately
  retained last-painted QSG frame.
