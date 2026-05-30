# Post-scalar performance audit: Focus E - architecture

## Executive summary

The current retained model -> full render snapshot -> full render frame -> QSG node update pipeline is correct and testable, but it is not the right long-term scaling contract for very large terminal grids.

The `nelostie_profile_scalar_span.txt` profile is a warning sign. The grid is 235 rows by 873 columns, roughly 205k cells. Dirty-row tracking is already effective enough to identify small update regions in many buckets, yet publication still repeatedly spends multi-millisecond time in `Terminal_screen_model::render_snapshot::append_rows`. That means the architecture is still paying broad visible-grid materialization costs before downstream row caches can help.

This is not primarily a local scalar hot spot anymore. The problem is that the contracts force each stage to reconstruct a complete visible-frame description even when the semantic change is a few rows. Renderer row caches then avoid some QSG churn, but only after the model has produced a full snapshot and the frame builder has walked enough data to classify, pack, and group a full frame.

Recommended direction: keep the immutable snapshot contract for diagnostics, transcript/replay, selection, and fallback correctness, but add a visual publication contract that can carry retained row payloads or row slabs. Prototype row-level retained render output first, not tile rendering and not a direct model-to-QSG diff. Tiles are probably overkill for terminal text unless later profiles show per-row retained rendering still cannot meet target frame budgets.

## Architecture diagnosis

The documented architecture explicitly says rendering is snapshot based: `Terminal_screen_model` produces `Terminal_render_snapshot`, `Terminal_session` stores a `std::shared_ptr<const Terminal_render_snapshot>`, `VNM_TerminalSurface` copies that shared handle into the item, `updatePaintNode` builds a `Terminal_render_frame`, and `Qsg_terminal_renderer` updates reusable scene graph nodes.

That design has strong correctness properties:

- One immutable object describes the visible terminal state.
- Transcript capture/replay can serialize stable snapshot state.
- Selection, cursor, IME, public projection, geometry-derived snapshots, and render tests all share one state carrier.
- QSG lifetime is isolated from parser/model mutation.
- Dirty row ranges are metadata, not the only source of truth, so full repaint fallback is simple.

The scaling problem is that the snapshot is still full-state shaped. `Terminal_render_snapshot` owns grid size, viewport, styles, cells, line provenance, dirty row ranges, hyperlinks, cursor, IME state, selection spans, metadata, and modes. The cell vector is positioned, but it is still a materialized vector for the visible snapshot. In the model implementation, snapshot construction reserves rows x columns capacity and appends visible row cells into `snapshot.cells`. The profile shows this `append_rows` stage dominating publication.

Dirty rows currently help, but too late and too narrowly:

- The model tracks dirty rows and publishes compact dirty ranges.
- The session and surface coalesce dirty ranges when snapshots are superseded before painting.
- The renderer has row-cache identities using active buffer, logical row, retained line id, and content generation.
- QSG update can reuse row resources when descriptors and row identity match.

Those mechanisms reduce QSG rebuilds, but they do not prevent full visible snapshot construction. They also do not prevent full render-frame construction from converting the complete snapshot into frame vectors and sidecars before node-level reuse is applied.

The current architecture is therefore a hybrid: retained model state and retained QSG row resources, connected by full-frame value objects. That middle full-frame boundary is the strategic bottleneck for very large grids.

For a 235x873 terminal, a single full visible-cell pass is already around 205k cell positions before style, text, wide-cell, hyperlink, selection, cursor, geometry, and packing work. Larger grids will scale linearly with visible cells even when the actual changed surface is a few rows. That is the wrong asymptotic shape for terminal workloads, which are mostly row-oriented appends, edits, scrolls, prompt updates, cursor movement, selection overlays, and occasional full-screen applications.

The current design remains appropriate as a correctness oracle and fallback path. It is not appropriate as the only hot-path visual publication contract for very large grids.

## Alternative designs

### Full snapshots with better local optimization

This keeps the current contract and attacks allocations, row projection, text conversion, packing, and QSG key construction.

Benefits:

- Lowest migration risk.
- Preserves transcript/replay and existing tests.
- Can reduce constant factors.

Limits:

- Still O(visible rows x columns) when publishing a visual update.
- Still creates pressure proportional to grid size, not change size.
- Cannot make small dirty-row updates cheap enough for very large grids if full snapshot append remains dominant.

Assessment: useful as cleanup, but insufficient as the strategic fix.

### Delta snapshots

A delta snapshot would carry snapshot metadata plus changed rows/cells relative to a known base sequence. Consumers would apply it to retained state.

Benefits:

- Aligns publication cost with changed rows.
- Preserves a snapshot-like contract and sequencing model.
- Could coexist with full snapshots for fallback, transcript checkpoints, and validation.

Costs:

- Requires every consumer to understand base sequence validity.
- Needs invalidation rules for viewport changes, resize, style table changes, color state changes, selection changes, IME changes, cursor-only changes, public projection scroll snapshots, and synchronized-output release.
- Transcript/replay must decide whether to record deltas, periodic full snapshots, or both.

Assessment: viable, but a pure delta snapshot is broader than the renderer needs. It risks turning the canonical data model into a replication protocol.

### Row slabs

A row-slab contract groups a small contiguous range of retained rows, for example 4, 8, or 16 viewport rows, with row identity, cells/spans, overlay-affecting metadata, and dirty state. The visual path updates only affected slabs.

Benefits:

- Fits terminal row-oriented mutation and scroll behavior.
- Reduces per-update overhead without requiring per-cell diff plumbing.
- Gives a bounded granularity for cache invalidation and QSG grouping.
- Easier than arbitrary rectangular tiling because wide cells, line provenance, selection spans, and cursor rows mostly stay row-local.

Costs:

- Slab boundaries need rules for wrapped lines, wide cells, selections spanning many rows, and overlays.
- A one-row edit may rebuild a slab, not exactly one row.
- Requires retained slab identity and base-generation handling.

Assessment: strong candidate if per-row management overhead is too high. It is a pragmatic middle ground between full frames and tiles.

### Retained render rows

The model/session publishes row-level render payloads keyed by retained line id and content generation. The renderer consumes rows directly or through a lightweight frame envelope. Clean rows are referenced, not rebuilt.

Benefits:

- Matches existing renderer row-cache identity concepts.
- Avoids rebuilding full visible `snapshot.cells` and full `Terminal_render_frame` vectors for unchanged rows.
- Lets row provenance become an actual hot-path contract instead of only a renderer cache key.
- Keeps dirty-row semantics close to the model, where mutation identity is known.

Costs:

- Requires a retained visual-state object somewhere between session and renderer.
- Needs clear fallback to full snapshot when row identity is unstable.
- Must separate content rows from overlay-only changes such as cursor blink, selection, IME preedit, visual bell, hyperlink underline policy, and geometry changes.

Assessment: best first strategic prototype. It reuses the row identity work already present and attacks the dominant `append_rows` cost directly.

### Tile-based rendering

A tile-based renderer divides the visible grid into rectangular regions and updates only dirty tiles. Tiles may become QSG subtrees, geometry batches, or texture-backed surfaces.

Benefits:

- Good for very large two-dimensional surfaces with localized edits.
- Can bound rebuild work even when rows are extremely long.
- Potentially useful for graphics-heavy terminal content or future scrollback minimaps.

Costs:

- Terminal semantics are row-major, not tile-major.
- Wide glyphs, combining marks, shaped runs, selections, hyperlinks, cursor overlays, and line wrapping can cross tile boundaries.
- Text shaping through Qt APIs does not naturally produce independent fixed-cell tiles without careful clipping and fallback behavior.
- Migration risk is high and test surface expands sharply.

Assessment: not the first move. Consider only if row/slab retention still leaves unacceptable long-row costs.

### Direct model-to-render diff

The model would emit renderer operations directly: row changed, row scrolled, cursor moved, style changed, selection changed, and so on. The snapshot/frame boundary would be bypassed for the visual hot path.

Benefits:

- Lowest theoretical overhead.
- Avoids duplicating state into snapshots and frames for visual updates.

Costs:

- Couples renderer behavior to model mutation details.
- Weakens the clean immutable boundary that currently supports testing and replay.
- Makes public projection, synchronized-output release, geometry-derived snapshots, and selection-derived snapshots harder to reason about.
- Risks introducing operation-order bugs where a retained-state mismatch is harder to diagnose than an invalid snapshot.

Assessment: too invasive for the next step. It may be a future end-state, but only after retained rows prove the needed invariants.

### Throttling and coalescing

The session/surface can reduce render publication frequency, coalesce dirty rows across pending snapshots, and skip intermediate visual updates during bursts.

Benefits:

- Helps bursty backend output.
- Already partially present through pending render update coalescing and dirty-row coalescing.
- Low architectural risk if framed as scheduling policy.

Limits:

- Does not reduce the cost of the frame that is eventually rendered.
- Can add latency and make interactive workloads feel worse if applied bluntly.
- Can hide architecture cost in benchmarks without fixing scaling.

Assessment: useful complement, not a primary architecture fix.

## Migration cost

Lowest-cost path:

- Preserve `Terminal_render_snapshot` as the canonical full-state diagnostic and fallback object.
- Add retained row identities and row payload handles to the visual path.
- Teach frame building to accept a retained row/slab source and rebuild only changed visual rows.
- Keep current full snapshot -> full frame path behind tests and fallback until the retained path reaches parity.

Moderate-cost path:

- Introduce `Terminal_visual_update` or similar as a sibling to `Terminal_render_snapshot`.
- It carries sequence, viewport identity, grid metrics basis, dirty rows/slabs, overlay changes, style/color generation, and references to retained row payloads.
- The surface stores the latest retained visual state instead of only a latest snapshot handle.
- The renderer consumes retained rows and produces QSG row updates.

High-cost path:

- Replace snapshot publication with deltas everywhere.
- Convert transcript/replay to delta-aware validation.
- Redesign selection/public-projection/geometry-derived paths around retained visual state.
- Potentially remove the full-frame object from hot rendering.

Expected cost ranking:

1. Throttling/coalescing policy improvements: low cost, limited strategic value.
2. Retained render rows: medium cost, high value.
3. Row slabs: medium cost, high value if rows are too granular.
4. Delta snapshots as a canonical contract: high cost, mixed value.
5. Tile rendering: high cost, uncertain terminal-specific value.
6. Direct model-to-render diff: highest coupling risk.

## Correctness risks

The biggest risk in moving away from full snapshots is stale retained state. The current architecture avoids that by making every render consume a complete immutable state object. A retained-row design must make invalidation explicit.

Key risks:

- Dirty-row under-reporting would become visible corruption rather than just missed optimization.
- Viewport remapping can make the same viewport row refer to a different logical row; retained rows must key by active buffer, logical row, retained line id, content generation, and compatible geometry/style generations.
- Style-table and color-state changes can alter many rows without cell text changes.
- Selection, IME preedit, cursor blink, cursor shape, visual bell, hyperlink underline, and focus-related overlays may need overlay-layer invalidation independent of content-row invalidation.
- Public projection scroll snapshots currently require full dirty row ranges; a retained path must not treat those as ordinary row deltas unless the projection basis is stable.
- Synchronized-output release can publish accumulated changes with unstable mutation identity; this should force full row/slab validation or fallback.
- Transcript/replay and snapshot diagnostics must remain able to produce a full comparable state.
- Resize and font metric changes invalidate geometry even if retained text content is unchanged.
- Wide cells and combining sequences require row-local consistency checks so partial row updates do not split base/continuation semantics.

The safe rule is: retained visual updates are allowed only when the producer can name the base sequence and all generation keys that make reuse valid. Otherwise publish a full snapshot/full repaint fallback.

## Recommended roadmap

1. Keep the full snapshot path as the correctness baseline.

Do not delete or weaken `Terminal_render_snapshot` while exploring performance architecture. It remains valuable for diagnostics, replay, tests, and fallback rendering.

2. Promote row identity from renderer optimization to visual contract.

The renderer already has row-cache identity concepts. Move the same idea upstream so the model/session can publish retained row payloads and dirty row/slab updates without materializing every visible cell on every frame.

3. Split content invalidation from overlay invalidation.

Cursor blink, cursor movement, selection changes, IME preedit, visual bell, and hyperlink underline changes should not require full content-row reconstruction. Treat these as separate overlay updates whenever possible.

4. Add generation keys before adding clever reuse.

A retained row/slab must declare the generations it depends on: content, row origin, grid geometry, style/color state, viewport mapping, and render options that affect visual output. Missing generation keys will create subtle stale rendering bugs.

5. Use row slabs only if row-level overhead becomes excessive.

Start with retained rows because terminal semantics are row-centered and existing code already has row identity. If per-row QSG/cache/key overhead becomes the next bottleneck, aggregate rows into slabs.

6. Defer tile rendering.

Tile rendering should be a second-generation experiment after retained rows/slabs. It is not the natural first fit for Qt text shaping or terminal line semantics.

7. Use throttling as a policy layer, not a substitute.

Coalescing pending updates is sensible, but the architecture still needs a cheap final render when a frame is eventually delivered.

## What to prototype first

Prototype retained render rows for the visual hot path.

Minimum prototype shape:

- Add an internal retained visual update envelope with sequence, grid size, viewport identity, dirty rows, and overlay-change flags.
- For each dirty row, publish a row payload keyed by active buffer, logical row, retained line id, content generation, and style/color generation.
- Let unchanged rows be referenced from previous retained visual state instead of rebuilt into `Terminal_render_snapshot::cells`.
- Keep the current full snapshot publication for transcript/replay and fallback.
- Teach `build_terminal_render_frame` or a sibling builder to build frame rows only for dirty content rows plus overlay rows.
- Force full fallback on viewport remap, resize, style-table generation mismatch, public projection scroll basis mismatch, synchronized-output unstable identity, or missing row provenance.

Prototype success criteria:

- For a stable viewport with one dirty row, model publication should not reserve or walk rows x columns cells.
- Frame construction should scale with dirty rows plus overlay rows, not visible cells.
- Existing renderer row-cache hits should remain valid or improve.
- Full snapshot diagnostics should still be available on demand.
- A forced full repaint should produce the same visual output as the retained-row path.

Suggested measurement:

- Add counters for retained rows referenced, retained rows rebuilt, fallback full rows rebuilt, overlay-only rows rebuilt, and full snapshot materializations avoided.
- Re-run the nelostie scalar-span profile and compare `Terminal_screen_model::render_snapshot::append_rows`, `Terminal_session::publish_render_snapshot`, `VNM_TerminalSurface::updatePaintNode`, and renderer row-cache counters.

## Files inspected

- `C:\plms\varinomics\varinomics-standards\varinomics_coding_style_guideline.md`
- `C:\plms\varinomics\varinomics-standards\varinomics_coding_style_llm_addendum.md`
- `C:\plms\varinomics\varinomics-standards\varinomics_review_scope.md`
- `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile_scalar_span.txt`
- `C:\plms\varinomics\vnm_terminal_surface\docs\architecture.md`
- `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\render_snapshot.h`
- `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\qsg_terminal_render_frame.h`
- `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\qsg_terminal_renderer.h`
- `C:\plms\varinomics\vnm_terminal_surface\src\terminal_screen_model.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\src\terminal_session.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\src\vnm_terminal_surface.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\src\qsg_terminal_renderer.cpp`
