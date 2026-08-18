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
- `ATTACHED`: the finished selection is attached to live rows. Its intersection
  with the current viewport is rendered as selection spans.
- `PAYLOAD_ONLY`: the selected text payload is retained for host access, but
  the visual attachment to rows has been dropped.

`Terminal_selection_range` carries grid positions plus a
`Terminal_selection_mode`. The production gesture produces `NORMAL` ranges;
`WORD` and `LINE` exist in the contract and transcript encoding, but surface
gestures do not map to them. `NONE` is invalid in any recorded range
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
- Resolving a lease later produces a
  `Terminal_selection_attachment_resolution_status` such as buffer, epoch,
  reflow, missing-line, content-generation, ordering, or endpoint failure. A
  failed resolution means the selection must not silently attach to different
  content.

The anchor also records its domain (`Terminal_selection_anchor_domain`): an
active-grid anchor not yet resolved to backing rows, primary backing rows,
alternate-screen active-grid rows, or payload-only.
Alternate-screen selections cannot survive into scrollback because the
alternate buffer has none.

At mouse-down, the session records the retained handle for the press row, its
original logical row and column, and the published source basis. Movement and
release resolve that provenance through the same model-owned attachment
resolver used by committed selections. Exact successor edges compose across
publications between pointer events, and within one publication when a
recovery supersedes a replacement the same publication already recorded. A missing, ambiguous, mutated, or evicted
handle permanently cancels the gesture; an established drag may retain its
trusted payload as `PAYLOAD_ONLY`, but it cannot reattach to the row currently
occupying the press-time viewport cell.

## Attachment Reconciliation

`Terminal_screen_model::resolve_selection_attachment` is the authoritative
attachment proof. It resolves every selected retained-line handle by exact
identity and content generation, using a model-issued successor only when the
old handle no longer exists. One publication can carry more than one accepted
repaint recovery, in which case a replacement is itself replaced before the
publication is observed; the resolver therefore follows the exact successor
chain to the retained handle rather than stopping after one edge. Every edge of
that walk is taken on the same evidence a single edge is: an exact old handle,
an unambiguous relation, an unchanged content generation, and a logical row
that continues where the previous edge landed. A walk longer than the number of
published relations has reused one, which is reported as a duplicate
resolution. The resolved rows must be unique, ordered, contiguous, and shifted
by one uniform logical-row delta, and every translated endpoint must remain
representable. The session installs the translated lease
atomically only after the complete proof succeeds; otherwise it drops the
visual lease and retains only the immutable payload.

The same rule covers the lifecycle boundaries:

- Primary scrolling and alternate-screen scrolling preserve an attachment
  only while every selected row survives exactly. An alternate row scrolled
  out of its fixed grid detaches because the alternate buffer has no history.
- Clearing primary scrollback preserves selected active-grid rows after their
  exact logical-row translation, but detaches a selection whose retained
  history row was cleared.
- A height-only resize with unchanged columns may translate an exact lease.
  A width change advances the reflow basis and fails closed; the session does
  not infer preservation from resize shape. Each accepted resize transition is
  evaluated independently, including public-projection geometry, so an
  incompatible intermediate transition remains detached even if a later resize
  returns to the original dimensions.
- Primary/alternate transitions, session-epoch changes, and session reset do
  not carry an attachment across the boundary. Reset clears the selection and
  its retained payload.

During synchronized output, the session composes only exact selected-handle
successors while content is hidden. Natural reset and forced stale-hold release
use the same proof under both deferred and immediate-public scroll policies.
Any missing, ambiguous, mutated, incompatible-buffer, epoch, or width-reflow
evidence fails closed for that hold and cannot be repaired by a later
publication. Release therefore installs an exact translated attachment or
retains only the immutable payload.

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
identity, so hosts can still read retained text through `selected_text()` in
`PAYLOAD_ONLY` state. The built-in copy shortcut is narrower: it copies only an
attached selection, including one scrolled offscreen but still backed by a
visual lease. Detached payload-only text is host-accessible retained state, not
plain-Ctrl+C copyable selection state.

## Diagnostics

`VNM_TerminalSurface::set_selection_trace_enabled(bool)` writes selection
diagnostics to stderr (the app flag is `--selection-trace`). In
transcript-enabled builds, every drag phase records a
`surface.selection_drag` event with the phase, whether the drag moved, the
anchor position, and the resulting range and mode; the transcript reader
validates these fields and rejects malformed events
(`src/terminal_transcript.cpp`).

Session trace lines record the attachment-resolution outcome and the lease
before and after reconciliation, making an atomic translation distinguishable
from a payload-only detach without rerunning selection logic.
