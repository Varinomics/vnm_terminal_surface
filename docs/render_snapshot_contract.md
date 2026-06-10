# Render Snapshot Contract

This document is the central contract for `Terminal_render_snapshot`, defined
in `include/vnm_terminal/internal/render_snapshot.h`. Everything stated here is
enforced by `validate_render_snapshot` (same header) unless noted otherwise.
`docs/architecture.md` describes how snapshots flow through the QSG render
path; this document specifies what a valid snapshot is.

## Producers And Consumers

Two producers build snapshots:

- The live-content producer: `Terminal_screen_model` assembles a snapshot of
  the visible viewport in `src/terminal_screen_model_snapshot.cpp` (a per-row
  loop emitting columns in ascending order).
- The public-projection producer: the public scroll path assembles snapshots
  from a retained projection in `src/terminal_session.cpp` and
  `src/terminal_public_projection.cpp` (`capture_from_safe_model` records the
  basis snapshot's validation status).

Consumers are the frame builder (`src/qsg_terminal_renderer.cpp`), which
iterates `snapshot.cells` directly, the selection-extraction helpers in
`render_snapshot.h`, and the session publication channel toward
`VNM_TerminalSurface`.

## Immutability And Lifetime

A published snapshot is immutable. The session stores the latest snapshot as a
`std::shared_ptr<const Terminal_render_snapshot>` and the surface copies that
shared handle through the render bridge (`docs/architecture.md`, "Render
Snapshot And QSG Path"). Derived snapshots are new values: dirty-row
coalescing (`snapshot_with_coalesced_dirty_rows`,
`coalesced_dirty_row_snapshot_handle`) returns a new snapshot or a new shared
handle and never mutates the previously published object. A retained handle
stays valid for as long as the holder keeps it, independent of later
publications.

## Cell Order

`snapshot.cells` is row-major with strictly ascending columns within each row:

- Every position lies inside the grid (`INVALID_CELL_POSITION`).
- Each cell sorts strictly after its predecessor, first by row, then by
  column (`INVALID_CELL_ORDER`).
- No two cells share a position (`INVALID_CELL_OVERLAP`).

This ordering is an architecture contract, not an incidental production
detail: the frame builder relies on it instead of rebuilding a position
table. Both producers emit in this order, and the validator rejects any
snapshot that violates it. The contract governs the cells that are present;
it does not require a cell for every grid position.

## Wide Cells

A character wider than one column is represented as one base cell plus
continuation cells:

- The base cell carries the text and `display_width > 1`, and the width must
  fit inside the row (`INVALID_CELL_WIDTH`).
- Every column covered by the base must be present as a continuation cell
  with `wide_continuation = true`, empty text, `display_width == 0`, and a
  column greater than zero (`INVALID_WIDE_CELL_CONTINUATION`).
- A continuation must be covered by exactly one base cell on its row, and a
  base must be followed by exactly `display_width - 1` continuations
  (`INVALID_WIDE_CELL_CONTINUATION`).
- Continuations must repeat the base cell's `style_id` and `hyperlink_id`
  (`INVALID_WIDE_CELL_STYLE`).
- A non-continuation cell inside a base cell's span is an overlap
  (`INVALID_CELL_OVERLAP`).

## Dirty Row Ranges

`snapshot.dirty_row_ranges` names the viewport rows whose content may differ
from the previously published snapshot; rows not covered are guaranteed
unchanged within the same row-identity space (below). Consumers may bound
per-row repaint work by these ranges.

The normalized form is validated: ranges are sorted, non-overlapping,
non-empty, and inside the grid (`INVALID_DIRTY_ROW_RANGE`).
`compact_dirty_row_ranges` produces this form: it sorts, deduplicates, drops
out-of-viewport rows, and merges adjacent rows; a full repaint is the single
range `{0, visible_rows}`.

Dirty rows from consecutive snapshots may be merged with
`snapshot_with_coalesced_dirty_rows` only when both snapshots share a
physical row-identity space, decided by `snapshots_share_row_identity_space`:
same grid size, same `metadata.row_origin_generation`, and the same viewport
mapping (`active_buffer`, `visible_rows`, `scrollback_rows`,
`offset_from_tail`). `row_origin_generation` advances when scrollback
eviction shifts which logical line each physical row index names, so
coalescing across it would attribute a dirty row to the wrong line; the
helper instead falls back to a full-viewport repaint range. The fallback is
purely conservative: it can only add repaint work, never skip a changed row.

## Viewport Consistency

The viewport must describe the grid (`INVALID_VIEWPORT`):

- `visible_rows` equals `grid_size.rows` and is positive.
- `scrollback_rows >= 0` and `0 <= offset_from_tail <= scrollback_rows`.
- The alternate buffer has no scrollback: `active_buffer == ALTERNATE`
  implies `scrollback_rows == 0` and `offset_from_tail == 0`.

## Basis And Purpose

`basis` records what the snapshot was built from (`LIVE_CONTENT` or
`PUBLIC_PROJECTION`); `purpose` records why it was built (`CONTENT`,
`SELECTION_DERIVED`, `GEOMETRY_DERIVED`, `SCROLL`). The pairing is a
biconditional: `basis == PUBLIC_PROJECTION` exactly when
`purpose == SCROLL` (`INVALID_SNAPSHOT_BASIS_PURPOSE`). The transcript
reader enforces the same pairing on recorded snapshot events
(`src/terminal_transcript.cpp`).

Public-projection snapshots additionally must use the primary buffer and must
carry exactly one full-viewport dirty range, because a projection snapshot
republishes the whole projected viewport.

## Line Provenance And Selections

`visible_line_provenance`, when present, identifies each visible row: it must
hold exactly one entry per grid row, each entry's `logical_row` must equal
the first visible logical row plus the row index, and `retained_line_id`
must be non-zero (`INVALID_LINE_PROVENANCE`). `content_stamp_ms` rides along
for timestamp display and is not part of row identity (`operator==` ignores
it).

Selection spans require provenance: a snapshot with selection spans and empty
provenance is invalid, and producers use
`suppress_selection_spans_without_valid_line_provenance` to drop spans rather
than publish an inconsistent snapshot. Each span must lie inside the grid
(`INVALID_SELECTION_SPAN`).

## Styles, Hyperlinks, Cursor

- `styles` is non-empty and `styles[0]` equals the default style; every
  `cell.style_id` indexes into `styles` (`INVALID_STYLE_ID`).
- Hyperlink metadata ids are non-zero and unique, and every non-zero
  `cell.hyperlink_id` resolves to a metadata entry
  (`INVALID_HYPERLINK_METADATA`).
- A visible cursor lies inside the grid (`INVALID_CURSOR_POSITION`).

## Where Validation Runs

`validate_render_snapshot` runs in production on the public-projection scroll
path: `src/terminal_session.cpp` rejects an invalid assembled snapshot
(publication returns no snapshot rather than publishing a bad one), and
`Terminal_public_projection::capture_from_safe_model` records the basis
snapshot's validation status. The test suites (`tests/render_snapshot`,
`tests/backend_session`, `tests/behavior_smoke`, `tests/screen_sgr`,
`tests/terminal_modes`, conformance and randomized-parser suites) validate
snapshots produced by the real producers, including dedicated
`INVALID_CELL_ORDER` coverage in `tests/render_snapshot/render_snapshot_tests.cpp`.
