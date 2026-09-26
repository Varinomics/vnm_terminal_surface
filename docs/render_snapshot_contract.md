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

Consumers are the frame builder (`src/qsg_terminal_renderer.cpp`), which reads
snapshots through `Terminal_render_snapshot_row_content_view`, the
selection-extraction helpers in `render_snapshot.h`, and the session publication
channel toward `VNM_TerminalSurface`.

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
detail: row-content views rely on it to expose row ranges without rebuilding a
position table. The frame builder uses the row-content view, both producers emit
in this order, and the validator rejects any snapshot that violates it. The
contract governs the cells that are present; it does not require a cell for
every grid position.

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
`SELECTION_DERIVED`, `SEARCH_DERIVED`, `GEOMETRY_DERIVED`, `SCROLL`). The pairing is a
biconditional: `basis == PUBLIC_PROJECTION` exactly when
`purpose == SCROLL` (`INVALID_SNAPSHOT_BASIS_PURPOSE`). The transcript
reader enforces the same pairing on recorded snapshot events
(`src/terminal_transcript.cpp`).

Public-projection snapshots additionally must use the primary buffer and must
carry exactly one full-viewport dirty range, because a projection snapshot
republishes the whole projected viewport.

## Line Provenance, Selections, And Search

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

The session emits spans only from an attached lease that passed the model's
complete retained-row proof. An unchanged-column height resize may therefore
publish translated spans when the selected handles and endpoints remain exact;
width reflow, an incompatible buffer, or any missing or mutated selected row
suppresses spans and leaves only the retained selection payload. Replacement
session initialization is stronger than an attachment-proof failure: it clears
both the attachment and payload while advancing the selection epoch.

Search match spans have the same provenance requirement but remain a separate
overlay contract. Each `Terminal_render_search_match_span` lies within one
visible row, identifies its column extent, and marks whether it is the current
match (`INVALID_SEARCH_MATCH_SPAN`). Producers use
`suppress_search_match_spans_without_valid_line_provenance` when provenance is
not available. Selection paint takes precedence over search paint where spans
overlap.

## Row Images

`visible_row_images` carries the sixel image each visible row shows, as the
row's immutable `Terminal_image_slice`. It is presence-tagged: empty unless a
visible row shows an image, then one entry per grid row, null for a row
without one, so a snapshot without images has exactly the fields it would
have without the capability. Producers set entries only through
`set_render_snapshot_row_image`, and consumers read them through the row
content view (`image()`, `image_at()`).

Slices are shared, never copied or mutated: the live-content producer hands
out the live row's own slice, and a history row's slice is decoded from its
record with the revision stored there. Revisions are unique in the process,
so a revision identifies a slice's pixels across snapshots, sessions, and
decodes. The public-projection producer copies each row's slice pointer into
its projection rows and back into scroll snapshots; a full-row capture
compares rows by revision. A geometry-derived snapshot keeps the images of the
rows that remain, like their cells.

A row whose image changes is a dirty row, so the dirty-range guarantee covers
images: a row not covered by a dirty range shows the same slice. An image
change alone does not advance the row's `content_generation`.

Images are an optional capability, so `validate_render_snapshot` ignores them
and a bad image never rejects the text of its snapshot. A consumer checks a
row's image with `validate_render_snapshot_row_image`, which returns its own
`Terminal_render_image_status`: the field size must be zero or the row count
(`INVALID_ROW_COUNT`), the revision non-zero, since renderers key cached texels
on it (`INVALID_REVISION`), the pixels non-null RGBA8888 premultiplied
(`INVALID_PIXELS`), the placing cell size positive
(`INVALID_CELL_PIXEL_SIZE`), the height at most one cell
(`INVALID_PIXEL_SIZE`), and the columns inside `[0, 4096]`
(`INVALID_COLUMN_SPAN`). A slice may start or end past a grid that narrowed
after it was placed; the renderer clips it. The frame builder drops only the
image of a row that fails and counts it (`images_rejected`).

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
snapshot's validation status. The frame builder runs `validate_render_snapshot_row_image` on every row
that shows an image. The test suites (`tests/render_snapshot`,
`tests/backend_session`, `tests/behavior_smoke`, `tests/screen_sgr`,
`tests/terminal_modes`, conformance and randomized-parser suites) validate
snapshots produced by the real producers, including dedicated
`INVALID_CELL_ORDER` coverage in `tests/render_snapshot/render_snapshot_tests.cpp`.
