# Selection Lifetime Design

This document defines the proposed selection lifetime contract for
`vnm_terminal`.
The implementation rollout is described in
[Selection lifetime implementation](selection_lifetime_implementation.md).
Until that rollout is complete and `public_surface.md` is updated, this
document is design intent rather than the current public API contract.

## Problem

The original bug class is selection drift. A user selects text from a visible
terminal snapshot, then the backing terminal rows mutate, scroll, or get reused.
If selection is stored only as mutable row and column coordinates, the highlight
can later cover text that is not the text copied by `selected_text()` or
Ctrl+C.

That behavior violates the user contract. A highlight is a visual claim that the
covered cells are the selected text. Copy payload lifetime and visual highlight
lifetime are related, but they are not the same lifetime.

The rejected alternatives are:

- Comparing `selected_text()` for the full selected range on every render
  snapshot. That avoids stale highlights, but it is O(full selected range) per
  snapshot and is unacceptable for large scrollback selections.
- Fine-grained dirty row or dirty column invalidation. Dirty regions are render
  scheduling hints, not semantic identity. They are too fragile to prove that a
  selection still covers the same text.
- Moving selection semantics into the renderer or QSG node cache. The renderer
  does not own session ordering, hidden output, copy payloads, buffers,
  scrollback, or terminal-cell identity.

## Non-Goals

- Long-lived precise visual tracking across arbitrary row mutation, scrollback
  reuse, alternate-buffer resets, and reflow.
- Render-time full-range text comparison.
- Renderer-owned selection validity.
- A public API expansion unless product use requires host-visible distinction
  between copyability and visual attachment.
- Model-owned row, cell, or cluster provenance tokens as the first mechanism.

## Product Contract

Selection has two separate products:

- The copy payload is the text returned by `selected_text()` and copied by
  Ctrl+C when the copy policy chooses local copy. It is captured from the
  selected source and remains available until an explicit clear, replacement
  selection, or session reset.
- The visual attachment is permission to draw selection spans over a published
  render snapshot. It exists only while the session can prove that the spans
  still cover the same content basis that produced the payload.

When those two products conflict, correctness chooses the copy payload. The
surface must fail closed by hiding selection spans while preserving the payload.
It must not silently draw stale spans over different text.

An active replacement drag is still an in-flight capture, not a committed
selection contract. If a true source or coordinate mismatch occurs during that
drag, such as a buffer switch or incompatible resize/reflow basis, the surface
cancels the drag and clears any stale in-flight payload. Scrollback row-origin
ambiguity is narrower: if a previous move already captured a payload, the
surface detaches the visual attachment and preserves that payload; if no payload
has been captured yet, the drag cancels to `NONE`.

The existing public `selectionState` remains a coarse copyability state:

- `NONE` means there is no copyable local selection payload.
- `ACTIVE` means `selected_text()` and Ctrl+C can use a local selection payload.

`selectionState` does not promise that a visible highlight is attached. If a
host needs that distinction, add a separate visual-selection property rather
than changing the meaning of `selectionState`.

No-payload states are not copyable. In `NONE`, `DRAG_ARMED`, and a cancelled
replacement gesture, `selected_text()` returns an empty string and Ctrl+C must
fall through to the existing no-selection behavior instead of being consumed as
a local copy. This differs from an active empty selection, which is a committed
local copy payload.

Replacement gestures use `mouse-down clears`: arming a new local selection
gesture clears the prior durable payload immediately. That matches common
terminal selection behavior and avoids a hidden old payload while the user is
visibly starting a replacement. A future `commit replaces` policy would be a
separate product decision and requires its own tests.

An empty committed selection is still an active empty payload: `selectionState`
is `ACTIVE`, `selected_text()` returns an empty string, and Ctrl+C performs a
local empty copy instead of sending ETX. A cancelled replacement gesture that
never commits a selection returns to `NONE`.

## Selection States

Internal state should represent copy payload lifetime and visual attachment
explicitly.

| State | Payload | `selected_text()` and Ctrl+C | Public `selectionState` | Render spans |
| --- | --- | --- | --- | --- |
| `NONE` | None | No local copy payload | `NONE` | None |
| `DRAG_ARMED` | None | No local copy payload | `NONE` | None |
| `DRAG_PREVIEW` | Provisional payload from the drag source | Return or copy the provisional payload while the local drag owns selection state | `ACTIVE` | Preview spans only while the lease is compatible |
| `ATTACHED_VISIBLE` | Durable payload | Return or copy the durable payload | `ACTIVE` | Selection spans are emitted |
| `ATTACHED_HIDDEN` | Durable payload | Return or copy the durable payload | `ACTIVE` | No spans for the candidate snapshot |
| `PAYLOAD_ONLY` | Durable payload | Return or copy the durable payload | `ACTIVE` | None |

`PAYLOAD_ONLY` is also called detached when discussing visual lease state. It
means the copy payload is still valid, but there is no live visual claim.

The optional future visual state should expose the internal visual attachment
without changing copyability. A minimal host-facing shape would distinguish
`NONE`, `DRAG_PREVIEW`, `ATTACHED_VISIBLE`, `ATTACHED_HIDDEN`, and
`PAYLOAD_ONLY`.

During an in-progress local drag, `selected_text()` and Ctrl+C operate on the
current provisional payload because the drag has already cleared/replaced any
prior durable payload. If host applications need to distinguish provisional
from committed payloads, that requires a future public visual or lifecycle
state; the first implementation keeps the existing coarse `ACTIVE` state.

## Source-Snapshot Visual Lease

A selection visual lease binds a selection to the published render snapshot or
content basis used for hit-testing. Published means the snapshot delivered to
the surface/render bridge and eligible for drawing. It does not mean hidden
model state waiting behind synchronized output, backend output that has not
been processed, or renderer-local cache state.

The lease records enough source information to decide whether a later published
snapshot can still receive spans:

- source content-basis identifier;
- session epoch and active buffer;
- grid dimensions and reflow basis relevant to row and column mapping;
- viewport mapping from visible rows to source logical rows;
- selected range and normalized endpoint cell boundaries;
- selected payload captured from the source or from a source proven
  bit-equivalent to it;
- row or cell content descriptors for the selected visible spans when such
  descriptors are available.

The first implementation may be conservative. It may require exact source
matching, or an explicit bit-equivalence proof, before drawing spans. Exact
source matching is acceptable only for a content-basis generation: cursor-only,
overlay-only, renderer-cache, font/DPR, and pixel-geometry-only generations must
not be treated as content changes. If the implementation only has a whole render
frame generation, exact generation matching is too broad and must be paired with
a separate content-basis identifier or documented as a temporary UX limitation.
If compatibility cannot be proven cheaply from snapshot metadata, the lease is
not compatible.

Payload extraction must come from the same published source used for hit
testing, or from a model path proven bit-equivalent to that source. A model
extraction is bit-equivalent only when the model state, buffer, viewport
mapping, grid, and selected cell content are known to match the published
source. Hidden synchronized output is not bit-equivalent to the visible
published source.

Render spans are emitted only when the visual lease is attached and visible.
The renderer receives spans as passive frame input and draws them. It does not
decide whether the lease is valid.

## Lease Compatibility

A lease is compatible with a candidate published snapshot only when all
required facts are true:

- The selection belongs to the same session epoch and has not been explicitly
  cleared or replaced.
- The candidate snapshot is published and visible to the user.
- The active buffer matches the lease source for any spans that would be drawn.
- The selected endpoints still map to valid terminal-cell boundaries.
- The selected rows that would be drawn map to the same source rows or to rows
  explicitly proven content-equivalent.
- The selected cell clusters that would be drawn have the same text and cell
  occupancy as the source.
- Any resize or reflow change is proven not to change the selected row/column
  mapping.
- Scrollback rows needed for visual attachment have not been evicted.

This proof must not call `selected_text()` over the full selected range on each
render snapshot. It may use content-basis identifiers, row descriptors, or exact
source identity. Whole render-frame generation is not sufficient when it changes
for cursor-only or paint-only updates.

## Invalidation Rules

Synchronized output:

- Selection hit-testing and payload capture use the published visible snapshot.
- Hidden synchronized output must not be selected unless it becomes explicitly
  visible through snapshot publication.
- While synchronized output is held, a lease may remain attached to the
  published visible snapshot.
- When held output is released and a new content snapshot is published,
  compatibility is re-evaluated. If the selected source cannot be proven
  equivalent, the state becomes `PAYLOAD_ONLY`.

Viewport scroll:

- Scrolling the viewport does not by itself change the copy payload.
- If the selected source rows are outside the candidate viewport, the state is
  `ATTACHED_HIDDEN`.
- If the selected source rows re-enter the viewport and compatibility is proven,
  the state can return to `ATTACHED_VISIBLE`.
- A viewport change must never cause spans to cover different logical rows.

Active output:

- Output that mutates selected text, selected cell occupancy, selected row
  identity, or scrollback mapping invalidates visual attachment.
- Output outside the selected source may preserve visual attachment only when
  metadata proves the selected source is unchanged.
- Without proof, active output fails closed to `PAYLOAD_ONLY`.

Row mutation:

- Character changes, wide-cell occupancy changes, combining-cluster changes,
  erase operations, line wrapping, and row reuse invalidate affected selected
  visual spans.
- Style-only changes may preserve attachment when text, cluster boundaries, and
  cell occupancy are unchanged.
- Dirty rows do not prove either mutation or non-mutation.

Scrollback eviction:

- Eviction of any row needed for visual attachment detaches the visual lease.
- During an active drag, eviction at the scrollback cap is row-origin ambiguous:
  after a previous valid move it preserves the already captured payload as
  `PAYLOAD_ONLY`, and before any valid move it clears the no-payload drag.
- A durable copy payload remains available until explicit clear, replacement,
  or session reset.

Alternate buffer:

- Primary and alternate buffers have separate selection source identities.
- A buffer mismatch emits no spans.
- If the original buffer remains retained and later becomes visible with a
  compatible content basis, `ATTACHED_HIDDEN` may become `ATTACHED_VISIBLE`.
- Alternate-buffer clear, reset, or replacement detaches visual leases sourced
  from the old alternate content.

Resize and reflow:

- Column-count changes and reflow invalidate visual attachment unless an
  explicit equivalence proof shows unchanged selected row and cell mapping.
- During an active drag, an incompatible coordinate/source mismatch cancels the
  replacement and clears the in-flight payload instead of preserving it.
- Geometry-only changes that preserve terminal rows, columns, content, and
  viewport mapping do not invalidate the lease.
- Font, device-pixel-ratio, and pixel-size changes affect drawing geometry, not
  selection text identity, when the terminal grid and content basis are
  unchanged.

Cursor-only and paint-only updates:

- Cursor movement, cursor visibility, and cursor blinking do not invalidate a
  visual lease by themselves.
- Cursor inverse-text overlays remain separate render inputs.
- Renderer-cache rebuilds, font/DPR changes, and pixel-geometry-only changes do
  not invalidate a visual lease when terminal grid, content basis, and viewport
  mapping are unchanged.

Wide cells and combining marks:

- Compatibility is based on terminal cell clusters, not only UTF-16 indexes.
- A wide glyph is selected once even though it occupies two cells.
- Combining marks remain attached to their base cluster.
- If a base, combining sequence, variation selector, or width classification
  changes, the affected visual lease is incompatible.

Drag across snapshot changes:

- `DRAG_ARMED` records the published snapshot source used for the anchor
  hit-test.
- `DRAG_PREVIEW` may extend only through hit-tests compatible with that source.
- If the published snapshot changes during a drag and compatibility is not
  proven, the surface must not combine the old anchor with a new unrelated
  extent.
- If a valid preview payload exists, including a valid empty payload from an
  empty committed selection, it may be promoted to `PAYLOAD_ONLY`. "No preview
  payload" means the gesture never produced a valid source/range payload; only
  that case returns to `NONE`.
- With the `mouse-down clears` policy, cancelled replacement gestures leave no
  prior payload behind and return to `NONE`.
- Empty committed selections produce an active empty payload rather than
  falling through to terminal Ctrl+C behavior.

## Renderer Boundary

The renderer remains passive. It consumes immutable render snapshots, converts
`selection_spans` into frame rectangles, and updates QSG nodes. It must not own
selection state, payload text, lease compatibility, hidden-output decisions, or
buffer identity decisions.

Renderer caches and QSG node reuse are optimizations. They cannot establish
that a selection is still semantically attached. A cached row descriptor may
help avoid rebuilding geometry, but it is not the product contract for copy
payload lifetime.

## Dirty Rows Are Not Semantic Identity

Dirty rows answer a renderer scheduling question: which visible rows need
updated drawing work. They do not answer whether selected text is still the
same text.

Dirty row data can be coarse, suppressed by clean-row reuse, set for changes
that do not affect selected text, or unrelated to scrollback identity changes.
Viewport movement, scrollback eviction, alternate-buffer switching, and reflow
can break visual attachment even when a dirty-row comparison looks harmless.

Selection validity must use explicit selection state and source compatibility,
not dirty-row absence.

## Deferred Model Provenance Tokens

Model-owned row, cell, or cluster provenance tokens would give each rendered
text unit an identity that can survive some movement and mutation. That is the
more precise long-lived tracking option, but it is deferred.

The source-snapshot lease satisfies the required safety contract with less
model complexity: the highlight is shown only while the selected source is
known to match, and the payload survives visual detachment.

Provenance tokens should be reconsidered if:

- conservative detachment is too frequent for product use;
- hosts require long-lived visible selections across active output and
  scrollback movement;
- accessibility, annotation, search, or hyperlink features require stable
  text-object identity beyond selection;
- benchmarks show snapshot metadata checks cannot meet large-selection
  performance targets without stronger identity;
- product requirements demand precise visual reattachment after resize or
  reflow.

## Risks And Open Questions

- Conservative detachment can surprise users who expect a highlight to remain
  visible while the copied payload is still available.
- `selectionState` conflates copyability with visual attachment. A separate
  public visual state may be needed after host feedback.
- Snapshot payload extraction may duplicate model text extraction unless the
  bit-equivalence proof is explicit and tested.
- Unicode cluster handling must stay aligned with the terminal width policy and
  render snapshot cell representation.
- Style-only mutations need a clear descriptor boundary so they do not detach
  text-identical selections unnecessarily.
- Drag behavior during rapid output needs manual validation for user feel.
- Public docs that describe selection highlight behavior should be updated when
  this contract is implemented.
