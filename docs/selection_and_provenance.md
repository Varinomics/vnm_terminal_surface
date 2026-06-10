# Selection And Row Provenance

This document describes how text selection works in the surface and how
selections stay anchored to the right content while the terminal scrolls,
resizes, and evicts scrollback. The contract types live in
`include/vnm_terminal/internal/selection_contract.h`; the drag handling lives
in `src/vnm_terminal_surface.cpp`; the projection into snapshots lives in
`src/terminal_session.cpp` and the snapshot helpers in
`include/vnm_terminal/internal/render_snapshot.h`.

## Selection Lifecycle

A selection is created by a mouse drag. When a mouse-reporting application
owns the unmodified mouse, a Shift-drag selects locally instead of being
forwarded (the modifier check is in `src/vnm_terminal_surface.cpp`). The
drag advances `Terminal_selection_internal_state`:

- `DRAG_ARMED`: button down, no movement yet.
- `DRAG_PREVIEW`: the drag moved; a provisional selection follows the
  pointer.
- `ATTACHED_VISIBLE`: the finished selection is attached to live rows and
  rendered as selection spans.
- `ATTACHED_HIDDEN`: the selection still exists but its rows are not
  currently representable in the viewport.
- `PAYLOAD_ONLY`: the selected text payload is retained for copying, but the
  visual attachment to rows has been dropped.

`Terminal_selection_range` carries grid positions plus a
`Terminal_selection_mode`. The production gesture produces `NORMAL` ranges;
`WORD` and `LINE` exist in the contract and transcript encoding but no
surface gesture maps to them today. `NONE` is invalid in any recorded range
(the transcript reader rejects it).

## Row Provenance

Rows are identified independently of their physical position:

- Every visible row carries `Terminal_render_line_provenance` in the
  snapshot: `logical_row`, a non-zero `retained_line_id`, and a
  `content_generation` that advances when the line's content changes
  (`render_snapshot_contract.md`).
- A selection holds a visual lease (`terminal_selection_visual_lease_t`):
  the source identity (session epoch, buffer, grid size, viewport mapping,
  `row_origin_generation`, content basis) plus one line lease per selected
  row (`terminal_selection_line_lease_t`, a row offset and a history handle
  built from the retained line id and content generation).
- Resolving a lease later can fail with explicit staleness codes
  (`Terminal_history_resolution_status`: stale epoch, stale row sequence,
  content-generation mismatch, and so on); a failed resolution means the
  selection must not silently attach to different content.

The anchor also records its domain (`Terminal_selection_anchor_domain`):
primary backing rows, alternate-screen active-grid rows, or payload-only.
Alternate-screen selections cannot survive into scrollback because the
alternate buffer has none.

During a drag, content drift is validated: if the content generation under
the drag changes incompatibly, the drag is cancelled, or, when the payload
is still trustworthy, the selection detaches to `PAYLOAD_ONLY` instead of
attaching to the wrong rows (`validate_selection_drag_content_drift`,
`cancel_selection_drag_after_content_validation_failure`).

## Projection Into Snapshots

An attached selection appears in render snapshots as
`Terminal_render_selection_span` entries (per-row column ranges plus the
source range). The snapshot contract ties spans to provenance: a snapshot
with selection spans must carry valid visible-line provenance, and producers
use `suppress_selection_spans_without_valid_line_provenance` to drop spans
rather than publish an inconsistent pairing. Selection-driven republication
uses snapshots with `purpose = SELECTION_DERIVED`. The session validates a
selection snapshot against the safe content basis (grid size, viewport
mapping, and lease identity) before projecting it.

## Text Extraction And Copy

`selected_text_from_render_snapshot` extracts the payload: the range is
normalized, each selected row contributes its `[first, end)` columns, absent
cells read as spaces, wide-character continuation cells are skipped (the
base cell contributes the text once), rows selected to the right edge trim
trailing spaces, and rows join with `\n`. The
`Selection_contract_controller` caches the extracted text with a payload
identity, so copying remains possible in `PAYLOAD_ONLY` state; the copy
shortcut is platform-dependent (see the app help text: Ctrl+C when a
selection exists, Command+C on macOS).

## Diagnostics

`VNM_TerminalSurface::set_selection_trace_enabled(bool)` writes selection
diagnostics to stderr (the app flag is `--selection-trace`). In
transcript-enabled builds, every drag phase records a
`surface.selection_drag` event with the phase, whether the drag moved, the
anchor position, and the resulting range and mode; the transcript reader
validates these fields and rejects malformed events
(`src/terminal_transcript.cpp`).
