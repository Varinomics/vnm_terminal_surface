# Focus C QSG renderer and render-frame construction audit

## Executive summary

The scalar-span profile does not point to a single model-snapshot bottleneck. The render thread spends most of its time after the snapshot has already reached `VNM_TerminalSurface::updatePaintNode`.

Primary findings:

- `build_terminal_render_frame` is the dominant render-thread cost: 26.128 s of 39.623 s in `updatePaintNode` across 662 render frames, with a 39.47 ms mean.
- Frame construction still performs whole-snapshot work even when dirty rows are sparse. The profile has 155,335 visible row-visits in rendered frames but only 11,096 dirty rows, while both the cell pass and packed-data pass process 55.837 M input cells.
- `build_terminal_render_frame::packed_data` is almost as expensive as the primary cell pass. It reclassifies cells, builds a row table, sorts row cells, converts packed text to UTF-8, and builds packed text spans that are not consumed by the QSG text path.
- QSG text-resource reuse is partially effective. Clean rows are skipped aggressively, and ASCII coalescing reduces 4.378 M text runs to 483,907 resource runs. However, dirty text rows still rebuild QSG text resources, and content-generation changes can force create/remove slot churn instead of in-place replacement.
- The renderer has a useful dirty/reuse policy, but it is applied after expensive full-frame construction. The largest win is to stop constructing full per-cell render-frame and packed-text sidecars for rows that QSG will later skip.

## Measured render costs

Profile: `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile_scalar_span.txt`.

Render-thread aggregate:

- `VNM_TerminalSurface::updatePaintNode`: 662 calls, 39.6231856 s total, 59.854 ms mean, 1.000686 s max.
- `build_terminal_render_frame`: 662 calls, 26.1280570 s total, 39.468 ms mean, 78.034 ms max.
- `build_terminal_render_frame::cells`: 661 calls, 13.2810813 s total, 20.092 ms mean.
- `build_terminal_render_frame::packed_data`: 661 calls, 12.7507519 s total, 19.290 ms mean.
- `Qsg_terminal_renderer::update_node`: 662 calls, 12.9840016 s total, 19.613 ms mean.
- `sync_text_resource_nodes`: 662 calls, 8.0444763 s total, 12.152 ms mean.
- `sync_graphic_rect_row_layer`: 662 calls, 2.3511781 s total, 3.552 ms mean.
- `packed_hard_graphic_rects`: 662 calls, 0.8849001 s total, 1.337 ms mean.

Text-resource subcosts:

- `make_text_resource_node`: 7,906 calls, 4.3199957 s total, 546 us mean.
- `append_batched_text_run_nodes`: 7,906 calls, 4.3170397 s total.
- `prepare_text_layout`: 483,907 calls, 1.4000564 s total.
- `add_text_run_layout`: 483,907 calls, 2.6700543 s total.
- `text_resource_row_descriptor`: 93,517 calls, 1.0599464 s total.
- `text_run_groups_by_viewport_row`: 662 calls, 0.4508819 s total.
- `sync_text_resource_nodes::remove_stale_entries`: 662 calls, 0.4114278 s total.
- `sync_text_resource_nodes::replace_cache_entry`: 4,513 calls, 0.4877344 s total.
- `sync_text_resource_nodes::reparent_slots`: 294 calls, 0.5542919 s total.

Renderer counters:

- Rendered frames: 662.
- Visible rows across frames: 155,335.
- Dirty rows across frames: 11,096.
- Full-dirty rows across frames: 2,115.
- Cell-pass input cells: 55,837,192.
- Packed-pass input cells: 55,837,192.
- Dirty-row lookup count: 111,674,384.
- Text groups considered: 93,517.
- Text groups dirty: 9,664.
- Text groups clean: 83,853.
- Text clean reuse skips: 83,853.
- Text content rebuilds: 7,906.
- Text content reused: 85,611.
- Text content removed: 3,230.
- Text cache entries created: 3,393.
- Text cache entries replaced: 4,513.
- Text leaf nodes created: 73,082.
- Text cache child nodes cleared for replacement: 250,963.
- Text cache child nodes cleared for removal: 304,706.
- Text max child nodes cleared in one cache entry: 669.
- Text resource descriptor reuses: 1,758.
- Text key-match reuses: 0.
- Text runs before coalescing: 4,378,068.
- Text runs after coalescing: 483,907.
- Graphic rect rows rebuilt: 2,051.
- Graphic rect rows reused: 36,247.

Snapshot-side context:

- Render snapshots constructed: 1,498.
- Snapshot rows visited/materialized: 352,030.
- Snapshot cells scanned: 307,322,190.
- Snapshot cells emitted: 134,005,265.
- Snapshot dirty rows requested/visible: 14,379.
- Snapshots superseded before render: 112.

## Frame construction map

`VNM_TerminalSurface::updatePaintNode` builds a fresh `Terminal_render_frame` for every render update, then passes that immutable frame to `Qsg_terminal_renderer::update_node`.

Frame construction path:

- `VNM_TerminalSurface_render_bridge::set_render_snapshot` coalesces dirty rows only when a render update is already pending and both old and new snapshots exist.
- `VNM_TerminalSurface::updatePaintNode` calls `build_terminal_render_frame` before any QSG row-cache reuse decision is possible.
- `build_terminal_render_frame` initializes default background, copies grid, viewport, and dirty ranges, computes cursor/IME state, then reserves vectors from full snapshot cell count.
- `build_terminal_render_frame::cells` iterates every `snapshot->cells` entry. For each cell it performs dirty-row lookup, simple-content classification, cursor/IME checks, style resolution, rectangle construction, provenance lookup, background emission, text/graphic routing, decoration emission, and text-run construction.
- `build_terminal_render_frame_packed_data` then performs another pass over the same snapshot cells. It builds a row table, stable-sorts cells by row/column, reclassifies cells, repeats cursor/IME filtering, and appends packed text or packed graphic spans.
- Selection, cursor, IME, and visual-bell overlays are appended after the full cell and packed passes.
- `Qsg_terminal_renderer::update_node` builds layer keys, derives packed hard-graphic rects, syncs geometry row layers, syncs text resources, then syncs cursor and overlay layers.

Packed-data map:

- `terminal_packed_render_row_t` carries active buffer, viewport row, logical row, first text span, text span count, first graphic span, and graphic span count.
- `terminal_packed_text_span_t` carries columns, style/colors, UTF-8 byte offset, and byte length.
- `terminal_packed_graphic_span_t` carries columns, style/colors, codepoint offset, and codepoint count.
- Packed graphic spans are consumed by `packed_hard_graphic_rects` and merged into graphic-rect layer inputs.
- Packed text spans and packed text bytes are not consumed by the QSG text-resource path in the inspected renderer. Text rendering still uses `frame.text_runs` and row-local `Terminal_render_text_run` vectors.

## QSG reuse/churn analysis

Text row identity:

- Text row slots are keyed by `row_cache_identity_t`: active buffer, logical row, retained line id, and content generation.
- A clean-row fast path is available only when the frame-level text key is unchanged, dirty ranges are non-empty, retained provenance is valid, an old slot exists for the same identity, and both old and current text-resource descriptors exist.
- Clean-row reuse is effective in the profile: 83,853 clean groups were skipped.

Dirty text behavior:

- Dirty groups still compute descriptors and usually rebuild or replace resources.
- Descriptor reuse caught 1,758 dirty rows whose descriptors were identical, but key-match reuse was zero.
- When a row content-generation changes, the new row identity no longer matches the old slot. That means the renderer creates a new wrapper/clip/resource entry for the new identity, then later destroys the old unconsumed slot. This is more churn than an in-place node replacement policy keyed by stable row ownership plus mutable content identity.
- Replacement is used only when an old slot with the same full identity exists but the resource key differs. That explains why the profile has both `text_cache_entries_created=3393` and `text_cache_entries_replaced=4513` instead of one stable replacement path for all dirty rows.

Text resource creation:

- ASCII coalescing is useful. It reduces 4.378 M candidate runs to 483,907 resource runs, and the last frame reduced 46,151 dirty-row runs to 147 runs.
- The remaining QSG text creation cost is still high because every rebuilt row goes through `make_text_resource_node`, `prepare_text_layout`, and `add_text_run_layout` for all coalesced runs in that row.
- `text_resource_max_runs_after_coalescing_per_row=632` shows some rows remain large enough to make a single dirty row expensive.

Geometry row reuse:

- Geometry row slots use active buffer and logical row as identity, with a separate per-row content key. This permits in-place row wrapper reuse when identity is stable and key changes.
- Graphic rect row reuse is working: 36,247 graphic rect rows reused versus 2,051 rebuilt, with zero row-cache fallbacks.
- The cost is still non-trivial because `graphic_rect_layer_inputs`, `packed_hard_graphic_rects`, and `rect_layer_groups` must process full-frame graphics before row reuse can skip or update nodes.

Layer/order churn:

- `text_wrapper_order_rebuilds=294` and `sync_text_resource_nodes::reparent_slots=0.554 s` indicate child-order maintenance is measurable.
- Slot order is compared using full row identity. If content-generation changes alter identities for rows that remain at the same viewport position, order repair can be triggered even when visible row order did not semantically change.

## Non-obvious issues

1. Packed text is an expensive sidecar without a renderer consumer.

The packed pass builds packed text rows/spans/bytes for the same text cells that the QSG path later handles through `frame.text_runs`. In the inspected renderer, packed text data is used for stats and storage, but not for creating QSG text nodes. This makes `build_terminal_render_frame::packed_data` a near-duplicate of the primary cell pass for text-heavy frames.

2. Dirty rows are discovered early but exploited late.

The snapshot and render frame both carry dirty ranges, but `build_terminal_render_frame::cells` and `build_terminal_render_frame::packed_data` still traverse all input cells. QSG clean-row reuse saves node work after the full frame already paid most per-cell construction cost.

3. The packed pass performs extra row-table allocation and sorting.

`build_terminal_render_frame_packed_data` builds `std::vector<std::vector<const Terminal_render_cell*>>`, pushes cell pointers by row, and stable-sorts rows before packing. The model snapshot append path emits rows in viewport order, so this should be treated as a correctness guard or transition mechanism, not as free. It is part of the 12.751 s packed-data cost.

4. Dirty-row lookup is repeated per cell in both passes.

`frame.stats.dirty_row_lookup_count=111,674,384` equals two dirty checks per input cell. Each lookup walks dirty ranges linearly. The current dirty range count is often low, but this is still avoidable repeated work and makes pathological dirty-range fragmentation more expensive.

5. Clean text skip still builds current-row descriptors.

`sync_text_resource_nodes` computes `text_resource_row_descriptor` before the clean-row skip. The profile spends 1.060 s building 93,517 descriptors, even though 83,853 groups take the clean skip. The descriptor also validates cursor/preedit/clipping constraints, but that validation could be split from full descriptor construction.

6. Full row identity couples ownership reuse to content freshness.

Including `content_generation` in the slot lookup identity is safe, but it prevents in-place reuse of wrapper/clip ownership across actual row content changes. The renderer then allocates a new slot and removes the old one for changed identities. A two-level identity would likely reduce create/remove churn: stable row ownership identity for slot lookup, content identity for resource validity.

7. The current profile lacks enough row-cache miss taxonomy.

The counters show rebuild/create/replace/remove totals, but they do not directly answer why a dirty row rebuilt: missing old slot due content-generation identity change, descriptor ineligible, key mismatch, cursor/preedit exclusion, clipped text, or true content change. That makes optimization riskier than necessary.

## Improvement options

1. Gate packed text construction behind an actual consumer.

If packed text is not used by the renderer, stop building `packed_text_spans` and `packed_text_bytes` in normal QSG frames. Keep packed graphic construction if `packed_hard_graphic_rects` remains the graphic source. This targets roughly half of `build_terminal_render_frame` time in this profile.

2. Split frame construction into stable per-row caches plus dirty-row deltas.

Keep render-frame row products keyed by retained line id/content generation and rebuild only dirty rows, cursor/IME rows, rows affected by viewport remapping, and rows whose style/color tables changed. QSG already has row reuse; moving reuse up into frame construction would avoid producing full `text_runs`, background rects, and packed sidecars for clean rows.

3. Precompute dirty-row membership per frame.

Build a row bitset or compact cursor over dirty ranges once per frame and pass it to classification and packing. This removes repeated `snapshot_row_is_dirty` scans and makes dirty fragmentation predictable.

4. Make packed data row-based and selective.

If packed data must remain, build it from the same row iteration used by the primary cell pass or cache it per retained row. Avoid a second full classify pass, avoid `build_explicit_snapshot_row_table` when snapshot cells are already row-major, and avoid packing clean rows whose packed products are unchanged.

5. Split text slot identity.

Use a stable slot identity for ownership and ordering, for example active buffer plus retained line id or viewport/logical row provenance, and use content generation/descriptor/key as the resource-validity identity. That would allow dirty content changes to replace a child resource in an existing wrapper/clip instead of creating a new slot and destroying the old slot.

6. Add a descriptor eligibility fast path.

For clean rows with a valid old descriptor and unchanged text frame key, check only cheap invalidators such as cursor text, preedit caret, clipping policy, and valid provenance. Build the full `Text_resource_row_descriptor` only for dirty rows or rows that are otherwise candidates for descriptor reuse.

7. Add miss-reason counters before changing policy.

Add counters for text row miss causes: no old slot, old slot identity generation mismatch, descriptor ineligible, descriptor mismatch, key mismatch, cursor/preedit exclusion, clipped run, and row order changed. These counters should be temporary or retained only if they stay useful in profiling output.

## Validation gates

- Reprofile the same scalar-span scenario and compare: `updatePaintNode`, `build_terminal_render_frame`, `build_terminal_render_frame::cells`, `build_terminal_render_frame::packed_data`, `Qsg_terminal_renderer::update_node`, and `sync_text_resource_nodes`.
- Require no increase in `paint_completed=false`, `text_content_failures`, row-cache fallbacks, or slow text-layout samples.
- Pixel-compare QSG render output for dirty text changes, cursor over text, IME preedit, hard block graphics, selection overlays, visual bell, and viewport scroll reuse.
- Add or update focused tests that distinguish clean-row skip, dirty descriptor reuse, content-generation row replacement, and packed graphic reuse.
- Track node churn after any identity-policy change: text cache entries created/replaced/removed, text leaf nodes created, child nodes cleared for replacement/removal, wrapper order rebuilds, and reparent time.
- Add a temporary packed-text-disabled profile gate. If rendered pixels and QSG tests are unchanged while packed-data time drops, packed text is confirmed as dead render-thread work for this path.

## Files inspected

- `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile_scalar_span.txt`
- `C:\plms\varinomics\vnm_terminal\src\main.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\qsg_terminal_render_frame.h`
- `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\render_snapshot.h`
- `C:\plms\varinomics\vnm_terminal_surface\src\qsg_terminal_renderer.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\src\terminal_screen_model.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\src\vnm_terminal_surface.cpp`
- `C:\plms\varinomics\varinomics-standards\varinomics_review_scope.md`
- `C:\plms\varinomics\varinomics-standards\varinomics_coding_style_guideline.md`
- `C:\plms\varinomics\varinomics-standards\varinomics_coding_style_llm_addendum.md`
