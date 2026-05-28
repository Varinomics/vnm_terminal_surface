# Immediate public scrolling during synchronized output

## Status

Draft design investigation.

This document captures the architectural options for making scroll input visibly
effective while DEC synchronized output is active, without publishing hidden live
terminal content. It intentionally includes rejected designs and open risks.

## Problem

DEC synchronized output lets an application update the terminal without exposing
intermediate frames. `vnm_terminal_surface` currently honors that by blocking
normal render snapshot publication while synchronized output is active.

The current implementation now preserves local scroll intent during such a
block, but the visible scroll may be deferred until synchronized output releases.
That is safe, but it feels like wheel input is delayed.

The target behavior is:

- wheel or scrollbar scroll should visibly move immediately;
- hidden synchronized-output content must not become visible;
- public scrollbar properties must not expose hidden scrollback growth;
- selection, transcript replay, and snapshot validation must remain correct;
- release must reconcile the user's visible scroll intent with the live model.

## Core constraint

The live terminal model is not a safe render source while synchronized output is
active. It may already contain hidden row rewrites, scrollback growth, cursor
state changes, mode changes, and dirty ranges that must remain unpublished until
release.

Therefore immediate scrolling cannot be implemented by bypassing the current
publication gate or by calling the normal live-model render path during a hold.

The design must separate:

- live terminal state, which may include hidden updates;
- public content, which is safe to show;
- live viewport state, which follows the terminal model;
- public viewport state, which represents what the user is currently viewing.

## Existing useful precedent

The code already has the concept of derived public snapshots in narrower forms.
Resize and selection paths can derive a safe publication from the last public
content basis rather than from hidden live content.

That precedent is useful, but not sufficient by itself. A scroll can reveal rows
that were not part of the last visible snapshot. Immediate scrolling therefore
needs a public row source, not only a copy of the last visible frame.

## Required invariants

- A snapshot published while synchronized output is active must contain only
  public content.
- Public viewport bounds must be computed from public scrollback depth, not from
  hidden live scrollback growth.
- Public viewport movement must be allowed independently from live content
  publication.
- Hidden live output must be published exactly once, at release or forced
  release.
- Release must reconcile visible scroll intent onto the live model before the
  release snapshot is published.
- Detached release reconciliation should prefer retained row identity over raw
  `offset_from_tail`.
- Selection text must be copied from the visible public content basis, never from
  hidden live rows.
- If visual selection spans cannot be proven against the current visible basis,
  spans must be suppressed while preserving any already-captured payload.
- Transcript replay must be able to distinguish public-view publications from
  live-content publications.

## Recommended architecture

Introduce a first-class public projection.

## MVP contract decisions

This section fixes the first implementation choices. An implementation plan
should not leave these points to local judgment.

### Projection row universe

The full immediate-scroll projection ultimately needs to contain both:

- public primary scrollback rows;
- the last safely published active grid rows.

The active grid is not optional. A viewport near the public tail can contain a
mixture of scrollback and active-grid rows, and synchronized output commonly
rewrites active-grid rows. Rendering those rows from the live model would leak
hidden output.

Phase 1 is not that full row universe. Phase 1 is an inert copied-projection
scaffold captured from the safe-basis viewport only. It records safe-basis
scrollback depth and active-grid metadata so later phases can reason about the
public bounds, but it stores only the rows that were already present in the
safe-basis viewport. Stored row count and safe-basis scrollback depth are
separate concepts.

The target full projection coordinate space is:

```text
public row 0                         oldest retained public scrollback row
...
public row N - 1                     newest retained public scrollback row
public row N                         active grid row 0
...
public row N + visible_rows - 1      active grid row visible_rows - 1
```

`scrollbackRows` remains the count of public scrollback rows. The public tail is
the last public active-grid row. Public projection snapshots must use the public
screen-buffer identity that was active at the safe publication boundary.

Before any Phase 2 or Phase 4 behavior scrolls, anchors, or renders outside the
copied safe-basis viewport, a later phase must either add a safe full public row
source or invalidate/defer off-viewport requests. Phase 4 cannot render public
scrollback from the Phase 1 viewport-only scaffold.

### Complete public snapshot basis

A public-projection snapshot must source every user-visible and validator-visible
field from the public projection or public viewport. This includes:

- cells;
- retained line provenance;
- styles;
- hyperlinks and hyperlink identity keys;
- cursor shape, position, visibility, and blink-relevant public state;
- public mode state;
- color state;
- active-buffer identity;
- dirty metadata;
- selection spans;
- grid size and geometry generation;
- snapshot validation metadata.

"Do not read live model rows" is not enough. Hidden live cursor movement, hidden
mode changes, hidden hyperlinks, hidden style changes, and hidden buffer
transitions are also content publication.

### Projection ownership and lifetime

For the first implementation, the public projection is session-owned and
immutable while synchronized output is active.

The projection is captured at synchronized-output entry from the last safe public
content state. The Phase 4 implementation materializes copied primary public
rows from the entry-boundary model only after verifying the live row source still
matches the latest safe content basis for the visible safe rows. If that
defensive check fails, capture falls back to the safe-basis viewport projection
instead of exposing hidden hold content.

If an input segment contains safe content immediately before the `DECSET 2026`
entry sequence, the MVP contract is to split processing at the entry sequence
and publish the safe prefix before synchronized-output mode becomes active. The
entry projection is then captured from that published safe-prefix basis.

Do not capture the entry projection from the live model after parsing bytes that
belong to the synchronized-output hold. Do not silently drop the safe prefix by
capturing only from an older published snapshot.

The same boundary rule applies at release. If an input segment contains
`DECRST 2026` followed by additional post-release bytes, the MVP contract is to
split processing at the release sequence:

- process and publish the synchronized-output release first;
- run public-to-live viewport reconciliation before that release snapshot;
- refresh the public projection from the release snapshot;
- then process any post-release suffix as ordinary live content.

Forced release follows the same logical ordering: force-release and publish the
release snapshot before later queued bytes are allowed to coalesce into the
release basis.

The projection does not grow during a hold. Hidden live scrollback growth and
eviction do not change public projection bounds.

The projection may be implemented by copied row records or by immutable retained
row leases. Whichever storage is chosen must provide transitive immutability for
cells, style references, hyperlink references, provenance, public mode metadata,
and geometry metadata. References into mutable live structures are not allowed.

Memory bound for the Phase 1 scaffold:

- one active public projection per session;
- at most the safe-basis viewport rows copied from the last safe publication;
- safe-basis scrollback depth and active-grid row count are metadata only and do
  not imply those rows are stored;
- live scrollback may also continue to hold its own configured limit during a
  long hold.

Memory bound for the later full row source remains one copied public projection:
public scrollback limit at projection entry plus public active-grid rows. That
later source is required before immediate public scrolling can expose
off-viewport rows.

If memory pressure requires evicting the public projection before release,
immediate public scrolling must disable itself for that hold and report an
explicit diagnostic. It must not fall back to live model rendering.

The session must still retain enough release-intent metadata to reconcile
deterministically:

- sticky-tail intent;
- detached anchor tuple, if one had been captured;
- projection generation and disable reason.

If the projection rows are gone, exact retained-row validation cannot use the
projection content. Release must then use the retained anchor tuple if it can be
mapped into the released live model; otherwise it follows the deterministic
fallback order below.

After projection invalidation from memory pressure, resize, or another explicit
disable reason:

- public viewport properties remain frozen at the last public projection state
  until release;
- scrollbar range and thumb position remain frozen at that public state;
- later wheel/track/thumb scroll input during the hold is accepted only as
  deferred intent metadata, never applied against hidden live bounds;
- deferred intent updates sticky-tail intent and desired release intent, but does
  not publish a public-projection snapshot;
- at release, screen-buffer compatibility is checked before sticky-tail action,
  retained detached anchors reconcile before offset-only deferred intent, and
  deferred offset is used only when no retained anchor is available;
- diagnostics must identify `public_projection_invalidated_deferred_intent`.

### Controller ownership

The MVP uses two explicit viewport controllers:

- the live controller tracks the live model and hidden scrollback changes;
- the public controller tracks the visible public projection.

Outside synchronized output the controllers are synchronized from each safe live
publication. During synchronized output:

- parser/model mutations update only the live controller;
- user-visible scrolling mutates only the public controller;
- public snapshots are generated from the public controller and public
  projection;
- normal live snapshots remain gated until release.

At synchronized-output entry, the public controller is initialized from the last
safe public viewport. If the user was already detached before entry, the public
controller remains detached.

### Sticky-tail intent

The public viewport controller must carry explicit sticky-tail intent. Numeric
equality to the public tail is not enough.

States:

- `sticky_tail = true`: the user has not intentionally detached from tail in this
  public basis.
- `sticky_tail = false`: the user intentionally scrolled or otherwise detached.

If `sticky_tail` is true at release, the release snapshot follows the live tail.
If `sticky_tail` is false at release, the detached anchor reconciliation path is
used even if the public viewport offset happens to equal the public tail after
clamping.

User actions that set `sticky_tail=false`:

- wheel, track, page, keyboard, or thumb movement away from the public tail;
- any explicit scroll-to-offset that does not request the public tail.

User actions that set `sticky_tail=true`:

- explicit scroll-to-bottom/end action;
- thumb drag or track action that requests the public tail;
- API scroll-to-offset request for public offset `0`;
- release reconciliation that follows live tail because sticky-tail was already
  true.

Clamping to public offset `0` by itself does not set `sticky_tail=true`.

### Detached anchor tuple

Detached release reconciliation must anchor by a visual-row tuple, not just by a
retained line id.

The MVP anchor tuple is:

- public projection generation;
- public screen-buffer identity;
- public geometry generation;
- retained line id;
- retained line content generation;
- visual fragment index within the retained line;
- viewport row index where the fragment appeared;
- sticky-tail intent.

The anchor is captured when the public viewport becomes detached and refreshed
when public scrolling changes the first visible row.

### Release reconciliation fallback

Release reconciliation order:

1. If the public buffer kind or epoch no longer matches the released live
   buffer, do not apply sticky-tail or retained-anchor actions; record
   `buffer_transition_released` or `screen_buffer_epoch_changed`.
2. If `sticky_tail` is true and the buffer identity is compatible, follow live
   tail.
3. If the detached anchor exists in the released live primary buffer with the
   same retained line id, content generation, screen-buffer identity, and
   compatible geometry, place the same visual fragment at the same viewport row.
4. If the retained line id and content generation survive but geometry changed,
   keep the retained row as a best-effort viewport anchor and record
   `detached_anchor_geometry_changed` with result `retained_id_best_effort`.
5. If the exact anchor is gone, use the nearest surviving successor retained line
   in the released live primary buffer.
6. If no successor survives, use the nearest surviving predecessor retained
   line.
7. If no retained identity can be mapped, clamp to the oldest available live
   primary scrollback position.
8. If no detached anchor exists, apply the deferred offset intent and record
   `deferred_offset` or `oldest_available_live` after live-boundary clamping.

Do not fall back to live tail for a detached viewport unless sticky-tail intent
is true. Every non-exact fallback must record a diagnostic reason so transcript
replay and tests can assert deterministic behavior.

### Screen-buffer transitions

Immediate public scrolling in the MVP is primary-screen only.

If hidden synchronized output switches into or out of alternate screen, the
public projection remains on the public screen buffer until release. At release:

- if the released live buffer is still primary, normal retained-row
  reconciliation applies;
- if the released live buffer is alternate, primary retained-row anchors are not
  applied to the alternate buffer;
- the release snapshot uses the released live buffer and reports
  `buffer_transition_released`;
- public projection scrolling is disabled until the next safe primary-buffer
  publication.

### Resize during a hold

The MVP does not resize the public projection during synchronized output.

If geometry changes while synchronized output is active:

- immediate public scrolling is disabled for the rest of that hold;
- future scroll intents may still be preserved as deferred live viewport intents;
- no public-projection snapshot is generated after the geometry change;
- controller-side invalidation records `public_projection_geometry_invalidated`;
- release reconciliation records `detached_anchor_geometry_changed` with
  `retained_id_best_effort` when the detached retained row still survives.

A later phase may add a public-data-only resize projection. It is not part of
the MVP.

### Selection policy

The MVP does not add new projection-backed selection mutation during
synchronized-output holds.

Policy:

- an existing selection payload remains copyable;
- existing visual spans may be re-emitted in public-projection snapshots only if
  their leases prove compatibility with the public projection basis and viewport
  mapping;
- if proof fails, visual spans are suppressed while the payload is preserved;
- selection creation or mutation during an active public-projection hold is
  ignored with diagnostic reason `selection_public_projection_unsupported`;
- projection-backed selection creation and extraction are a later phase.

This is intentionally conservative. It prevents hidden live rows from becoming a
selection text source.

The ignored mutation set includes:

- mouse-down selection creation;
- drag continuation;
- shift-extension;
- keyboard selection extension;
- double-click or word selection;
- triple-click or line selection;
- select-all;
- programmatic selection API mutation;
- clearing, replacing, or modifying the active selection.

Existing payload copy remains allowed because it does not query hidden live
content.

### Snapshot basis, purpose, and transcript schema

Snapshot basis and purpose belong on the render snapshot, not only in transcript
output.

Add a snapshot-basis enum and snapshot-purpose enum before implementing replay
changes:

```cpp
enum class Terminal_render_snapshot_basis
{
    LIVE_CONTENT,
    PUBLIC_PROJECTION,
};

enum class Terminal_render_snapshot_purpose
{
    CONTENT,
    SCROLL,
    SELECTION_DERIVED,
    GEOMETRY_DERIVED,
};
```

Transcript output must use these same basis and purpose values, not a separate
taxonomy.

The only public-projection purpose in the MVP is `SCROLL`. Geometry-derived and
selection-derived snapshots remain based on the last safe live/public content
basis as defined by their existing contracts. If a future phase supports
public-data-only resize projection, it should use
`basis=PUBLIC_PROJECTION, purpose=GEOMETRY_DERIVED`.

Transcript schema must be versioned. Compatibility records written before the
snapshot-basis field default to `basis=LIVE_CONTENT`. Records written before the
snapshot-purpose field default to `purpose=CONTENT`, except for the documented
producer-owned snapshot reasons that exactly identify selection-derived or
geometry-derived publications. Compatibility inference must not inspect
incidental free-form uses of words such as `selection` or `geometry`. Records
with explicit basis or purpose fields are validated strictly against the enum
values above.

The documented compatibility reason mapping is:

- `selection changed`, `selection cleared`, and `selection visual detached` map
  to `purpose=SELECTION_DERIVED`;
- `resize geometry snapshot ready` maps to `purpose=GEOMETRY_DERIVED`.

Minimum new transcript/replay fields for the MVP:

- schema version;
- snapshot basis;
- snapshot purpose;
- effective synchronized-output scroll policy for the hold/generation;
- policy change event and diagnostic reason if a host changes the property during
  a hold;
- public projection generation;
- public viewport before and after;
- live viewport before and after when release occurs;
- visible scroll applied;
- live content publication blocked;
- release reconciliation result;
- hidden-row eligibility and clamp reason;
- public-projection disable reason, if any.

The existing implication
`visible_scroll_applied == local_scroll_applied && !render_publication_blocked`
must be removed. In immediate public scrolling,
`visible_scroll_applied=true` and `live_content_publication_blocked=true` is a
valid state.

Phase 3 kept visible public scroll publication deferred, so older wheel traces
used the legacy inferred value by default. Phase 4 routes an explicit
visible-scroll value for public-projection scroll, including the surface wheel
trace helper, without reintroducing the blocked-publication implication.

Policy changes during an active synchronized-output hold are sampled but do not
retroactively change projection capture:

- the effective policy for a hold is sampled at synchronized-output entry;
- changing the property mid-hold records diagnostic reason
  `synchronized_output_scroll_policy_changed_mid_hold` and affects only the next
  hold;
- if the effective policy is `DEFER_UNTIL_CONTENT_PUBLICATION`, no public
  projection is created for that hold;
- if the effective policy is `IMMEDIATE_PUBLIC_PROJECTION`, projection capture
  and public scrolling follow the MVP contract above for the whole hold unless
  invalidated.

### Public projection snapshot validation

Existing render snapshot validation must explicitly support
`basis=PUBLIC_PROJECTION, purpose=SCROLL`.

Validation rules that still apply:

- grid dimensions and visible row count are internally consistent;
- viewport fields are valid for the public projection bounds;
- visible line provenance maps every visible row to the public projection basis;
- cells, style references, hyperlinks, cursor metadata, and mode metadata are
  internally consistent with the projection generation;
- selection spans, if emitted, prove compatibility with the projection basis;
- dirty row ranges cover the full visible viewport for the MVP.

Validation rules that must not apply as if the snapshot were live content:

- the snapshot does not advance live-content basis state;
- the snapshot is allowed while live content publication is blocked;
- dirty rows describe public viewport reprojection, not live model damage.

### Public API semantics

The MVP is opt-in.

Policy location:

- session configuration stores the default;
- `VNM_TerminalSurface` exposes an additive property;
- app hosts can leave the default deferred behavior unchanged.

Proposed policy enum:

```cpp
enum class Synchronized_output_scroll_policy
{
    DEFER_UNTIL_CONTENT_PUBLICATION,
    IMMEDIATE_PUBLIC_PROJECTION,
};
```

In `IMMEDIATE_PUBLIC_PROJECTION` mode, public viewport properties mean visible
public viewport state while synchronized output is active:

- `scrollbackRows`: public projection scrollback rows;
- `viewportOffsetFromTail`: public viewport offset from public tail;
- `viewportAtTail`: geometric public state, true when
  `viewportOffsetFromTail == 0` over the public projection;
- `viewportVisibleRows`: public projection visible rows.

These properties must not expose hidden live scrollback growth before release.

If the public projection becomes invalidated during a hold, these properties
remain frozen at the last public projection values until release. Direct API
readers and the app scrollbar observe the same frozen public basis.

When a user scroll moves outside the copied public rows after invalidation, the
recorded `deferred_offset_from_tail` is the authoritative release intent. Any
detached anchor captured from the stale public viewport is superseded and must
not win release reconciliation over that deferred offset.

Sticky-tail intent is internal state and diagnostics state. It is not equivalent
to `viewportAtTail`. A detached public viewport can have
`viewportAtTail == true` after clamping while `sticky_tail == false`.

Existing boolean scroll APIs keep their shape initially:

- return `true` when visible public scroll was applied;
- return `false` when neither visible public scroll nor accepted deferred live
  intent was applied;
- richer diagnostics carry the basis and deferral details.

The public API comments must explicitly document the two policy modes.

### Scrollbar contract

In immediate mode during synchronized output:

- scrollbar range is based on public projection scrollback depth;
- scrollbar thumb position is based on public viewport state;
- wheel, track click, page scroll, and thumb drag mutate the public viewport;
- thumb drag clamps to public projection bounds;
- hidden live scrollback growth does not change range or thumb position until
  release;
- release updates range and thumb position after reconciliation;
- if public projection is invalidated by resize or memory pressure, scrollbar
  operations keep the last public range/thumb state, record deferred intent
  metadata only, and report the disable reason.

The app must not compute a scrollbar range from hidden live state.

### Rapid synchronized-output toggles

Every synchronized-output entry creates a new public projection generation from
the current safe public state. Every release discards the hold projection after
the release snapshot refreshes the public basis.

Rapid enter/release/enter sequences are treated as separate generations. Public
anchors do not cross projection generations unless the release reconciliation
has already mapped them into the live/public basis.

### Public snapshot versus live content basis

Public-projection snapshots become the latest visible snapshot, but they must
not become the live-content basis.

The session must distinguish at least:

- latest visible render snapshot;
- latest safe live-content snapshot basis;
- active public projection snapshot, if synchronized output is active.

Release dirty-row coalescing and live-content reconciliation must compare the
released live snapshot against the latest safe live-content basis, not against a
public-projection scroll snapshot. Otherwise a release could incorrectly treat a
view-only public scroll as if it were live content publication.

`m_latest_content_render_snapshot`-style state must only advance on safe live
content publication, not on `basis=PUBLIC_PROJECTION, purpose=SCROLL`.

### Compatibility definitions

When the detached anchor retained line id survives but content generation does
not match, the exact anchor is considered gone. Reconciliation then proceeds to
the successor/predecessor fallback steps and records
`detached_anchor_content_generation_changed`.

Compatible geometry for exact anchor restoration means the public geometry
generation and released live geometry generation describe the same cell-column
width and visual-fragment mapping. If geometry changed during the hold,
immediate public scrolling has already been disabled for that hold; release may
still use the stored anchor tuple only as a best-effort retained-row identity,
not as an exact visual-fragment placement proof.

The public screen-buffer identity must include both buffer kind and buffer epoch.
A hidden transition from primary to alternate and back to primary creates a new
epoch; release records `screen_buffer_epoch_changed` and does not claim exact or
sticky-tail reconciliation against the prior primary identity.
Epoch mismatch always prevents exact visual-fragment restoration.

If retained row identity can still be mapped after an epoch mismatch, that is a
fallback restoration, not an exact restoration, and must record a
`screen_buffer_epoch_changed` reconciliation reason.

Viewport-only projection fallback rows do not prove true retained-line visual
fragment ordinals when earlier fragments may sit above the copied viewport.
Release must not report `exact_anchor` from those rows; it must use an
offset/deferred fallback or another explicitly non-exact reconciliation result.

Safe geometry-only resize snapshots that are derived from the last public
content basis may still publish during a synchronized-output hold. They do not
refresh the public projection. After such a geometry change, immediate public
scroll snapshots are disabled for the hold as described above.

### State split

The session should maintain these concepts explicitly:

- `live model`: parser-applied terminal state, including hidden synchronized
  output changes.
- `public projection`: rows, provenance, modes, style references, hyperlinks,
  and metadata from the most recent safe publication boundary.
- `live viewport controller`: viewport state synchronized to the live model.
- `public viewport controller`: viewport state over the public projection.

During normal operation the public projection and live model are refreshed
together at safe publication boundaries. During synchronized output the public
projection freezes while the live model continues to mutate.

### Public projection contents

The target public projection must contain enough information to render all
public rows that a user may scroll to while synchronized output is active. A
visible immediate-scroll implementation cannot only store the currently visible
rows.

The Phase 1 scaffold is the deliberate exception: it is runtime-inert,
viewport-only, and used only as a safe-basis copied projection contract. It may
record that the safe basis had public scrollback, but it does not store that
scrollback unless those rows were already visible in the safe-basis viewport.

At minimum, each projected row needs:

- row cells or a safe immutable reference to row cells;
- retained line identity;
- content generation or equivalent row version;
- style and hyperlink references required by rendering and text extraction;
- enough logical row metadata to build visible-line provenance.

The projection may be materialized as copied rows, copy-on-write retained rows,
or immutable row leases. The architectural boundary is the public projection;
copy-on-write is an implementation strategy, not the top-level design.

Any phase that makes public viewport movement visible must first close the Phase
1 storage gap by adding a safe full public row source or by invalidating and
deferring any request whose target rows fall outside the copied viewport.

### Public scroll snapshot

When synchronized output is active and the user scrolls:

- mutate the public viewport controller;
- clamp against public projection bounds;
- publish a public-projection snapshot immediately;
- mark the full visible viewport dirty initially;
- do not read live model rows;
- do not update the live content snapshot basis;
- record diagnostics that this was a public-view publication.

The first implementation should prefer conservative invalidation over partial
dirty optimization.

### Release reconciliation

When synchronized output releases:

- if `sticky_tail` is true, publish the live release at live tail;
- if detached, anchor by the first visible public retained line when possible;
- if the retained line still exists in the live model, place it at the same
  visual row;
- for wrapped retained lines, apply the captured visual fragment index within
  that retained line before reporting `exact_anchor`;
- if the captured visual fragment index is viewport-relative rather than proven
  against the full retained row store, refuse `exact_anchor` and use a
  non-exact offset/deferred fallback;
- if the anchor was evicted or rewritten, clamp predictably and record a
  diagnostic reason;
- refresh the public projection from the released live model;
- publish the live release snapshot exactly once.

Offset-only reconciliation is not sufficient because hidden output can grow or
evict scrollback while the public projection is frozen.

## Design options considered

### Option A: public projection cache

Maintain a session-owned cache of the last safe public rows and render immediate
scroll snapshots from that cache.

Pros:

- directly models the safety boundary;
- can expose public scrollback rows not visible in the last frame;
- avoids hidden live model reads during synchronized output;
- supports selection and transcript replay with explicit basis metadata.

Cons:

- adds memory ownership and row lifetime complexity;
- requires careful release reconciliation;
- requires new tests for projection bounds, eviction, and selection basis.

Assessment: recommended direction.

### Option B: split content and view snapshot streams

Publish content snapshots and view snapshots separately. Content snapshots remain
gated by synchronized output; view snapshots publish immediately. The renderer
composes the latest public content basis with the latest public view state.

Pros:

- clean conceptual separation: synchronized output blocks content, not view;
- avoids treating viewport changes as content changes;
- can reduce unnecessary content snapshot churn.

Cons:

- invasive because current consumers expect one `Terminal_render_snapshot`;
- every renderer, transcript, validation, and selection consumer must learn the
  two-layer model;
- higher migration risk than a derived public-projection snapshot path.

Assessment: architecturally clean, but probably a later-stage refactor unless
the derived snapshot approach becomes too awkward.

### Option C: derived scroll snapshot from latest public content snapshot

Synthesize a scroll-only snapshot from the last public content basis, similar to
existing derived resize or selection snapshots.

Pros:

- aligns with existing derived snapshot patterns;
- smaller migration than a full content/view split;
- can keep existing renderer interface mostly intact.

Cons:

- insufficient if the latest public snapshot contains only currently visible
  rows;
- still needs a public row source for offscreen scrollback;
- must avoid accidentally treating derived snapshots as fresh live content.

Assessment: good implementation shape if backed by a public projection. Not
sufficient as a standalone shortcut.

### Option D: frozen full model clone

Clone or fork the entire terminal model at synchronized-output entry and render
public scroll snapshots from the frozen model.

Pros:

- reuses existing render snapshot and selection logic;
- may be faster to prototype;
- fewer new row projection APIs initially.

Cons:

- potentially doubles memory;
- deep-copy or copy-on-write semantics are risky;
- easy to route mutations to the wrong model;
- resize and reflow behavior during a hold becomes complex;
- can become a temporary hack that is hard to remove.

Assessment: possible bridge, not preferred long-term architecture.

### Option E: renderer-side pixel or row translation

Keep publication blocked and ask the renderer to visually translate already
published rows.

Pros:

- superficially small;
- might handle sub-row pixel movement.

Cons:

- cannot reveal public scrollback rows not already in the snapshot;
- breaks selection and copy semantics;
- leaves public viewport properties stale;
- bypasses snapshot validation and transcript replay;
- will produce incorrect rows under real scrollback movement.

Assessment: reject.

### Option F: force synchronized-output release on scroll

When the user scrolls, immediately release synchronized output so a normal live
snapshot can publish.

Pros:

- easy to reason about mechanically.

Cons:

- violates synchronized-output semantics;
- exposes partial application frames;
- turns a user input into protocol behavior;
- will cause tearing for TUIs that deliberately use synchronized output.

Assessment: reject.

### Option G: publish live model with filters

Allow live snapshot publication but filter or suppress dirty rows that are hidden.

Pros:

- appears to reuse current render path.

Cons:

- hidden output may affect scrollback depth, row identity, cursor state, modes,
  and rows not marked dirty;
- filtering dirty ranges is not a complete safety proof;
- high risk of subtle leaks.

Assessment: reject.

## Public API considerations

The safest rollout is additive and opt-in.

Potential policy:

```cpp
enum class Synchronized_output_scroll_policy
{
    DEFER_UNTIL_CONTENT_PUBLICATION,
    IMMEDIATE_PUBLIC_PROJECTION,
};
```

Defaulting to the existing deferred behavior would reduce compatibility risk.
Defaulting to immediate mode may be appropriate later once conformance and
manual validation are strong enough.

Existing boolean scroll APIs are not expressive enough to explain the new state.
Diagnostics should distinguish:

- local scroll was applied;
- visible public scroll was applied;
- live content publication remains blocked;
- snapshot came from public projection;
- hidden live rows were not eligible;
- release reconciliation preserved anchor, clamped, or fell back.

## Scrollbar and app integration

The app scrollbar must use the same public viewport semantics as text-area wheel
scrolling.

During synchronized output:

- scrollbar range should reflect public projection scrollback depth;
- thumb position should reflect public viewport;
- wheel, track, and thumb drag need defined behavior;
- hidden live scrollback growth must not change scrollbar range until release;
- release should update range and thumb position after reconciliation.

The app may optionally show an "output pending" indicator when the visible frame
is a public projection while hidden output exists.

## Selection implications

Selection should not be enabled by accident over hidden live content.

Initial safe policy:

- selection payloads that already exist may remain copyable;
- visual spans should be emitted only when the public projection proves the
  selected rows and viewport mapping;
- new selection creation during synchronized output is disabled in the MVP with
  diagnostic reason `selection_public_projection_unsupported`.

Preferred later policy:

- selection source identity includes public projection generation;
- selected text extraction reads public projection rows;
- visual lease reattachment at release uses retained row identities;
- if proof fails, preserve text payload but suppress visual spans.

## Transcript and replay implications

The transcript schema must represent public projection scrolling explicitly.

The canonical MVP transcript field list is defined in
`Snapshot basis, purpose, and transcript schema` above. Do not maintain a second
field list here. In particular, use `snapshot basis`; do not introduce a
separate `content basis` field name.

Existing replay assumptions that `render_publication_blocked` implies no visible
scroll must be revised.

## Performance and memory risks

Main risks:

- copying full scrollback at every safe publication boundary;
- publishing full snapshots at high wheel-event rates;
- pinning evicted rows through immutable public leases;
- invalidating too much of the QSG row cache.

Mitigations:

- build the projection from immutable retained rows or copy-on-write storage;
- only refresh the public projection at content publication boundaries;
- full-dirty public scroll snapshots first, optimize later;
- measure high-frequency wheel during long synchronized-output holds;
- bound retained public rows by the configured scrollback limit.

## Resize and alternate-screen behavior

Initial scope should be primary-screen scrollback only.

For resize during synchronized output:

- either derive a resized public projection from public data only;
- or disable immediate public scrolling until release when geometry changes;
- tests must cover same-size and changed-size holds.

For alternate screen and mouse reporting:

- keep existing application-controlled wheel routing;
- do not introduce local public scrollback behavior unless a separate product
  contract says so.

## Minimal viable implementation direction

The least invasive clean approach is:

1. Add a public projection row source captured from safe publications.
2. Add a public viewport controller.
3. Add a derived public scroll snapshot path using the projection.
4. Keep `Terminal_render_snapshot` as the renderer-facing object initially.
5. Publish public scroll snapshots while synchronized output is active.
6. Reconcile by retained row identity on release.
7. Later consider splitting content and view snapshots if derived snapshots
   become too complex.

## Tests required before first opt-in implementation

- Immediate public scroll publishes a snapshot during synchronized output.
- Hidden live row rewrites do not appear in public scroll snapshots.
- Hidden live active-grid rewrites do not appear in public scroll snapshots near
  the public tail.
- Hidden cursor movement, hidden mode changes, hidden style changes, and hidden
  hyperlink changes do not appear in public projection snapshots.
- Hidden live scrollback growth does not change public scroll bounds.
- Multiple wheel events accumulate visibly during a hold.
- Release preserves detached public anchor by retained row identity.
- Evicted public anchor follows the specified successor/predecessor/oldest-live
  fallback order and records the reconciliation diagnostic.
- Full-row Phase 4 projection capture stores retained-line visual fragment
  ordinals and release reconciliation uses them for exact wrapped-line anchors.
- No-payload synchronized-output hold still permits immediate public scroll.
- Selection spans are valid against public projection or suppressed.
- New mouse selection during a public-projection hold is ignored with the MVP
  diagnostic reason.
- Transcript replay reproduces public scroll before release and live content
  after release.
- Transcript replay remains backward compatible with pre-snapshot-kind
  transcripts.
- Transcript replay records effective policy per hold and ignores mid-hold
  policy changes until the next hold.
- Scrollbar wheel, track, and thumb drag operate against public projection bounds
  during a hold.
- Invalidated-projection scrollbar state remains frozen at the last public
  range while deferred intent is recorded.
- Deferred off-copied user scroll intent supersedes stale detached-anchor
  reconciliation on release.
- Selection mutation paths during a public-projection hold are ignored with the
  MVP diagnostic.
- `viewportAtTail == true` with `sticky_tail == false` after clamping still uses
  detached release reconciliation, not live-tail release.
- Clamping to public offset `0` does not set `sticky_tail=true`.
- `DEFER_UNTIL_CONTENT_PUBLICATION` sampled at entry creates no public
  projection and preserves legacy deferred behavior for that hold.
- Mid-hold policy changes do not retroactively change the active hold.
- Input-segment splitting at `DECSET 2026` publishes the safe prefix before
  projection capture.
- Input-segment splitting at `DECRST 2026` publishes and reconciles the release
  before processing the post-release suffix.
- Forced release publishes and reconciles before later queued bytes can coalesce
  into the release basis.
- Phase 3 immediate public scroll records intent but emits no visible
  `basis=PUBLIC_PROJECTION, purpose=SCROLL` snapshot.
- `basis=PUBLIC_PROJECTION, purpose=SCROLL` snapshots do not advance
  `m_latest_content_render_snapshot`-style live-content basis.
- A live-content release after a previously published public-projection scroll
  dirties the full viewport.
- Explicit scroll-to-bottom sets `sticky_tail=true`, and release follows live
  tail across hidden scrollback growth.
- Resize during hold disables immediate public scrolling with controller-side
  `public_projection_geometry_invalidated`; release reports
  `detached_anchor_geometry_changed` if a surviving detached retained row is
  restored as best effort.
- Hidden buffer transitions into alternate screen disable primary projection
  anchoring on release.
- Rapid synchronized-output toggles create distinct projection generations.

## Open questions

These are not MVP blockers, but should be answered before changing the default
policy from deferred to immediate:

- Can the public projection use immutable retained row leases, or should the
  first implementation copy row records at synchronized-output entry?
- What measured memory and latency budget should be required before enabling the
  immediate policy by default?
- Can public-projection snapshots later use precise row-shift dirty ranges, or
  should they remain full-viewport dirty permanently?
- Should projection-backed selection creation be added in a later phase, or is
  disabling new selection during holds an acceptable long-term behavior?
- Should a later phase support public-data-only resize projection during a hold?
- Should the app show an explicit "output pending" indicator when rendering a
  public projection while hidden live output exists?

## Current recommendation

Do not implement another local patch in the wheel handler.

Proceed only through a design-governed change that introduces an explicit public
projection and public viewport. The first implementation should keep the
renderer-facing snapshot shape stable by publishing derived public-projection
snapshots, not by immediately splitting all snapshot consumers into content and
view layers.

Implementation must not begin until the architecture and implementation-plan
reviews agree that the MVP contract above is coherent and testable.
