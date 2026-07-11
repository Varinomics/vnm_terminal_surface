# Diagnostics Schema

This document describes the active QSG atlas diagnostics counters,
descriptor-backed retained-history measurements, runtime JSON-only helpers,
and the legacy text-layout descriptor table retained for compatibility checks.
It is kept in sync with the descriptor tables in
`src/diagnostics/metric_descriptor.h` (the retained-history and text-layout
blocks) and
`src/diagnostics/atlas_metric_descriptors.h` (the atlas blocks); those tables
are the single source of truth for the listed fields, and the byte-golden test
`tests/diagnostics_text_layout/diagnostics_text_layout_tests.cpp` pins the
descriptor output against them.

## Descriptor-backed serializations

Descriptor-backed atlas and retained-history metric blocks are emitted in two
forms:

- **JSON** (`src/diagnostics/metrics_json.cpp`, always built): the runtime
  metrics document. Counters are emitted as decimal **strings** (so a 64-bit
  value is never silently narrowed to a JSON `double`); doubles are emitted as
  native JSON numbers and booleans as native JSON `true`/`false`. JSON object
  keys are unordered; consumers must look metrics up by name, not by position.
- **TEXT** (`src/diagnostics/profile_text.cpp`, built only when
  `VNM_TERMINAL_ENABLE_PROFILING=ON`): the human-readable profile report. Each
  counter is one line, `  <label>=<value>`, and **order is significant** -- the
  report is read top to bottom and diffed line by line.

Historically each serializer hand-wrote its own copy of the field list, so the
two could drift (a counter added to JSON but not TEXT, or reordered). The active
atlas blocks now flow through one shared descriptor table each, consumed by both
`emit_metrics_json` and `emit_metrics_text`, so the field set and order cannot
diverge. For every active table-driven atlas block the JSON key is also the TEXT
label. The text-layout descriptor table remains documented and golden-tested as
legacy renderer compatibility schema, but the canonical renderer diagnostics
surface is now the `qsg_atlas` block.

The runtime metrics document also contains hand-written JSON-only sections such
as `render_invalidation` and `backend_drain`. Those sections are public runtime
JSON helpers, not descriptor-backed JSON/TEXT metric blocks, and no TEXT
profile section mirrors them.

A few atlas fields are deliberately left hand-written rather than table-driven
because they are not plain counter/bool field reads: the `warm_elapsed_ms` and
`lazy_elapsed_ms` durations (emitted as raw `double` values), the top-level
`max_glyph_instance_page` (a `std::max(0, ...)` clamp), the enum-string fields
(text-renderer policy, sampler mode, LCD order), the nested `first_glyph_miss`
diagnostic, and the `msdf_text` doubles. These stay in the serializers, around
the shared tables.

### Retained history block (JSON key `retained_history`, TEXT header `retained_history`)

This block reports live retained-history ring measurements and style/hyperlink
compaction counters. `average_retained_row_bytes` is
`retained_record_bytes / retained_rows`, or zero when the ring is empty. The
nested estimate is a codec-owned projection, not a measurement of retained
content.

| Field | Kind | Unit | Stability |
|-------|------|------|-----------|
| `byte_budget` | Counter | Bytes | Unstable |
| `retained_rows` | Counter | Count | Unstable |
| `retained_record_bytes` | Counter | Bytes | Unstable |
| `average_retained_row_bytes` | Double | Bytes | Unstable |
| `payload_kind_generic_compact_rows` | Counter | Count | Unstable |
| `payload_kind_prefix_plain_ascii_rows` | Counter | Count | Unstable |
| `current_style_count` | Counter | Count | Unstable |
| `peak_style_count` | Counter | Count | Unstable |
| `style_compaction_count` | Counter | Count | Unstable |
| `reclaimed_styles` | Counter | Count | Unstable |
| `hyperlink_compaction_count` | Counter | Count | Unstable |
| `reclaimed_hyperlink_ids` | Counter | Count | Unstable |
| `prefix_plain_ascii_estimate` | Object | None | Unstable |

### Prefix plain ASCII estimate block

Contract version `1` projects homogeneous full-width rows eligible for the
prefix-plain-ASCII codec. It uses the live ring byte budget, current model
width, exact encoded record size including ring overhead, and the codec-owned
target of 205,000 rows. Styled, linked, non-ASCII, or otherwise generic-compact
rows may consume more bytes.

JSON path: `retained_history.prefix_plain_ascii_estimate`. TEXT header:
`prefix_plain_ascii_estimate`.

| Field | Kind | Unit | Stability |
|-------|------|------|-----------|
| `contract_version` | Counter | Version | Unstable |
| `source_width_columns` | Counter | Columns | Unstable |
| `record_bytes` | Counter | Bytes | Unstable |
| `retained_rows` | Counter | Count | Unstable |
| `target_rows` | Counter | Count | Unstable |
| `max_columns_at_target_rows` | Counter | Columns | Unstable |

## Runtime JSON-only sections

These sections are emitted only by the runtime JSON helpers in
`vnm_terminal/diagnostics/metrics_json.h`. Counter values are decimal strings;
string metadata values are JSON strings; boolean values are JSON booleans.
These diagnostics are `UNSTABLE`.

### Renderer compatibility block (JSON key `renderer`)

This legacy block is retained for hosts that still frame a `renderer` object.
New renderer diagnostics should read the `qsg_atlas` block.

| Field | Kind | Unit | Stability |
|-------|------|------|-----------|
| `compatibility_scope` | String | None | Unstable |
| `canonical_renderer_metrics` | String | None | Unstable |
| `frames_published` | Counter | Count | Unstable |
| `paint_completed_frames` | Counter | Count | Unstable |
| `qsg_atlas_render_count` | Counter | Count | Unstable |

### Render invalidation block (JSON key `render_invalidation`)

| Field | Kind | Unit | Stability |
|-------|------|------|-----------|
| `update_requests` | Counter | Count | Unstable |
| `scheduled_updates` | Counter | Count | Unstable |
| `coalesced_requests` | Counter | Count | Unstable |
| `consumed_updates` | Counter | Count | Unstable |
| `render_snapshot_callback_epoch` | Counter | Count | Unstable |
| `last_rendered_snapshot_sequence` | Counter | Count | Unstable |
| `last_rendered_publication_generation` | Counter | Count | Unstable |
| `pending_update` | Bool | None | Unstable |

### Backend drain block (JSON key `backend_drain`)

| Field | Kind | Unit | Stability |
|-------|------|------|-----------|
| `total_drain_calls` | Counter | Count | Unstable |
| `budgeted_drain_calls` | Counter | Count | Unstable |
| `unbudgeted_drain_calls` | Counter | Count | Unstable |
| `posted_drain_calls` | Counter | Count | Unstable |
| `posted_full_budget_calls` | Counter | Count | Unstable |
| `posted_frame_pending_small_budget_calls` | Counter | Count | Unstable |
| `budget_exhausted_incomplete` | Counter | Count | Unstable |
| `cursor_stable_incomplete` | Counter | Count | Unstable |
| `total_elapsed_ns` | Counter | Nanoseconds | Unstable |
| `max_elapsed_ns` | Counter | Nanoseconds | Unstable |
| `session_processing_calls` | Counter | Count | Unstable |
| `session_processing_elapsed_ns` | Counter | Nanoseconds | Unstable |
| `session_processing_max_elapsed_ns` | Counter | Nanoseconds | Unstable |
| `sync_from_session_calls` | Counter | Count | Unstable |
| `sync_from_session_elapsed_ns` | Counter | Nanoseconds | Unstable |
| `sync_from_session_max_elapsed_ns` | Counter | Nanoseconds | Unstable |
| `frame_work_pending_drain_calls` | Counter | Count | Unstable |
| `frame_work_pending_elapsed_ns` | Counter | Nanoseconds | Unstable |
| `render_update_pending_drain_calls` | Counter | Count | Unstable |
| `atlas_completion_pending_drain_calls` | Counter | Count | Unstable |
| `requeue_count` | Counter | Count | Unstable |
| `pending_callback_after_drain` | Counter | Count | Unstable |
| `output_backpressure_after_drain` | Counter | Count | Unstable |

Budgeted drains that stop `UNSETTLED` increment
`budget_exhausted_incomplete`. Budgeted drains that stop `CURSOR_STABLE`
increment `cursor_stable_incomplete`. `HELD` drains increment neither. When
the cursor-stable extension is disabled, frame-drain DECTCM cursor-stable
boundaries are reported as `UNSETTLED` for default-off behavior;
synchronized-output release-stable stops remain `CURSOR_STABLE`.

## Descriptor model

A `Metric_descriptor<Stats>` row carries:

| Column     | Meaning                                                              |
|------------|----------------------------------------------------------------------|
| `json_key` | JSON object key and TEXT report label.                               |
| `kind`     | `COUNTER` (integer), `BOOL`, or `DOUBLE`.                             |
| readers    | Stateless function pointers that read the field from `Stats`; the row's kind selects one. |

Counter readers cast their fields to `std::uint64_t`; bool and double readers
preserve their native types.

The `Unit` and `Stability` columns in the tables below are schema
documentation, not stored descriptor fields.

### Stability

These are debug/diagnostic counters. They reflect internal renderer accounting
(fast-path screening, classification buckets, cache reuse) and are expected to
change as the renderer evolves. They are therefore treated as unstable: tools
may read them for investigation, but no external contract guarantees a counter
name, its meaning, or its presence across versions. They are not a public API.

## Text-layout block

Legacy compatibility counters describing the QTextLayout slow path and the
ASCII-replacement fast path. Run counters tally text runs; `*_code_units`
counters tally UTF-16 code units. All are `COUNTER` / `COUNT` / `UNSTABLE`. The
block is emitted in the order below by the retained descriptor emitters;
`text_ascii_replacement_add_glyphs_calls` is optional (emitted only when the
stats type carries it) and is the one field handled outside the shared table
loop.

| Field | Kind | Unit | Stability |
|-------|------|------|-----------|
| `qt_text_layout_calls` | Counter | Count | Unstable |
| `text_layout_runs_single_code_unit` | Counter | Count | Unstable |
| `text_layout_runs_multi_code_unit` | Counter | Count | Unstable |
| `text_layout_runs_all_space` | Counter | Count | Unstable |
| `text_layout_runs_printable_ascii` | Counter | Count | Unstable |
| `text_layout_runs_printable_ascii_with_space` | Counter | Count | Unstable |
| `text_layout_runs_other_ascii` | Counter | Count | Unstable |
| `text_layout_runs_non_ascii` | Counter | Count | Unstable |
| `text_layout_runs_clipped` | Counter | Count | Unstable |
| `text_layout_runs_ascii_layout_font` | Counter | Count | Unstable |
| `text_layout_runs_force_blended_order` | Counter | Count | Unstable |
| `text_layout_runs_with_hyperlink` | Counter | Count | Unstable |
| `text_layout_runs_with_decoration` | Counter | Count | Unstable |
| `text_layout_runs_mixed_ascii_non_ascii` | Counter | Count | Unstable |
| `text_layout_runs_pure_non_ascii` | Counter | Count | Unstable |
| `text_layout_runs_plain_unclipped` | Counter | Count | Unstable |
| `text_layout_runs_plain_unclipped_ascii_font` | Counter | Count | Unstable |
| `text_layout_runs_all_space_plain_unclipped` | Counter | Count | Unstable |
| `text_layout_runs_printable_ascii_plain_unclipped` | Counter | Count | Unstable |
| `text_layout_runs_non_ascii_plain_unclipped` | Counter | Count | Unstable |
| `text_layout_runs_mixed_ascii_non_ascii_plain_unclipped` | Counter | Count | Unstable |
| `text_layout_runs_pure_non_ascii_plain_unclipped` | Counter | Count | Unstable |
| `text_layout_runs_fast_space_candidate` | Counter | Count | Unstable |
| `text_layout_runs_fast_ascii_candidate` | Counter | Count | Unstable |
| `text_layout_runs_fast_ascii_no_space_candidate` | Counter | Count | Unstable |
| `text_layout_runs_fast_ascii_single_candidate` | Counter | Count | Unstable |
| `text_layout_runs_fast_ascii_multi_candidate` | Counter | Count | Unstable |
| `text_ascii_replacement_runs_screened` | Counter | Count | Unstable |
| `text_ascii_replacement_runs_eligible` | Counter | Count | Unstable |
| `text_ascii_replacement_runs_attempted` | Counter | Count | Unstable |
| `text_ascii_replacement_runs_trusted_fast_path` | Counter | Count | Unstable |
| `text_ascii_replacement_runs_succeeded` | Counter | Count | Unstable |
| `text_ascii_replacement_runs_all_space_succeeded` | Counter | Count | Unstable |
| `text_ascii_replacement_add_glyphs_calls` (optional) | Counter | Count | Unstable |
| `text_ascii_replacement_runs_fallback` | Counter | Count | Unstable |
| `text_ascii_replacement_runs_rejected_clipped` | Counter | Count | Unstable |
| `text_ascii_replacement_runs_rejected_force_blended_order` | Counter | Count | Unstable |
| `text_ascii_replacement_runs_rejected_decoration` | Counter | Count | Unstable |
| `text_ascii_replacement_runs_rejected_hyperlink` | Counter | Count | Unstable |
| `text_ascii_replacement_runs_rejected_non_printable_ascii` | Counter | Count | Unstable |
| `text_ascii_replacement_runs_rejected_non_ascii` | Counter | Count | Unstable |
| `text_ascii_replacement_runs_rejected_geometry` | Counter | Count | Unstable |
| `text_ascii_replacement_runs_rejected_unsupported_font` | Counter | Count | Unstable |
| `text_ascii_replacement_runs_rejected_internal_node` | Counter | Count | Unstable |
| `text_ascii_replacement_runs_rejected_glyph_mapping` | Counter | Count | Unstable |
| `text_layout_code_units` | Counter | Count | Unstable |
| `text_layout_space_code_units` | Counter | Count | Unstable |
| `text_layout_printable_ascii_code_units` | Counter | Count | Unstable |
| `text_layout_other_ascii_code_units` | Counter | Count | Unstable |
| `text_layout_non_ascii_code_units` | Counter | Count | Unstable |
| `text_layout_plain_unclipped_code_units` | Counter | Count | Unstable |
| `text_layout_all_space_plain_unclipped_code_units` | Counter | Count | Unstable |
| `text_layout_printable_ascii_plain_unclipped_code_units` | Counter | Count | Unstable |
| `text_layout_non_ascii_plain_unclipped_code_units` | Counter | Count | Unstable |
| `text_layout_fast_space_candidate_code_units` | Counter | Count | Unstable |
| `text_layout_fast_ascii_candidate_code_units` | Counter | Count | Unstable |
| `text_ascii_replacement_code_units_screened` | Counter | Count | Unstable |
| `text_ascii_replacement_code_units_eligible` | Counter | Count | Unstable |
| `text_ascii_replacement_code_units_attempted` | Counter | Count | Unstable |
| `text_ascii_replacement_code_units_trusted_fast_path` | Counter | Count | Unstable |
| `text_ascii_replacement_code_units_succeeded` | Counter | Count | Unstable |
| `text_ascii_replacement_code_units_fallback` | Counter | Count | Unstable |

## Atlas blocks

These blocks come from the QSG atlas renderer's per-frame report
(`Qsg_atlas_frame_report` and its sub-summaries). JSON nests each block under a
key (e.g. `"producer"`); TEXT emits the field run under a header line (e.g.
`  producer`). Only the flat field run is table-driven, in
`src/diagnostics/atlas_metric_descriptors.h`; the surrounding nesting/header is
hand-written in the serializers. Top-level count and elapsed fields on
`Qsg_atlas_frame_report` are recorder-lifetime cumulative counters. Nested
sub-summaries such as `producer`, frame-build-derived fields, `coverage`, and
`buffer_upload` generally describe the latest recorded report snapshot.
`warm_lazy` is the persistent warm/lazy summary for the current atlas epoch,
and any field name or row that explicitly states cumulative semantics remains
cumulative. All atlas diagnostics here are `UNSTABLE`.

### Atlas producer block (JSON key `producer`, TEXT header `producer`)

| Field | Kind | Unit | Stability |
|-------|------|------|-----------|
| `text_runs_considered` | Counter | Count | Unstable |
| `text_runs_empty` | Counter | Count | Unstable |
| `shape_cache_lookups` | Counter | Count | Unstable |
| `shape_cache_hits` | Counter | Count | Unstable |
| `shape_cache_misses` | Counter | Count | Unstable |
| `shape_cache_inserts` | Counter | Count | Unstable |
| `shape_cache_pruned` | Counter | Count | Unstable |
| `shape_cache_entries` | Counter | Count | Unstable |
| `shaped_runs_built` | Counter | Count | Unstable |
| `shaped_runs_reused` | Counter | Count | Unstable |
| `shaped_glyph_records_built` | Counter | Count | Unstable |
| `shaped_glyph_records_reused` | Counter | Count | Unstable |
| `presentation_run_scans` | Counter | Count | Unstable |
| `presentation_source_scans` | Counter | Count | Unstable |
| `presentation_fast_text_runs` | Counter | Count | Unstable |
| `presentation_emoji_runs` | Counter | Count | Unstable |
| `slot_resolutions_built` | Counter | Count | Unstable |
| `slot_resolutions_reused` | Counter | Count | Unstable |
| `simple_path_attempts` | Counter | Count | Unstable |
| `simple_path_used` | Counter | Count | Unstable |
| `simple_path_fallbacks` | Counter | Count | Unstable |

### Atlas warm-lazy block (JSON key `warm_lazy`, TEXT header `warm_lazy`)

Emitted in field order; the two `*_elapsed_ms` durations are `double` values
emitted outside the shared table (so they are not table rows below). This block
is copied from the renderer's persistent warm/lazy summary: `warm_*` fields
summarize the current atlas warm epoch, while `lazy_*` counters and
`incomplete_frames` accumulate within that summary until the renderer resets it.

| Field | Kind | Unit | Stability |
|-------|------|------|-----------|
| `warm_completed` | Bool | None | Unstable |
| `warm_epoch` | Counter | Count | Unstable |
| `warm_seed_strings` | Counter | Count | Unstable |
| `warm_shaped_glyph_records` | Counter | Count | Unstable |
| `warm_covered_glyph_records` | Counter | Count | Unstable |
| `warm_skipped_glyph_records` | Counter | Count | Unstable |
| `warm_environment_skipped_glyph_records` | Counter | Count | Unstable |
| `warm_failed_glyph_records` | Counter | Count | Unstable |
| `warm_missing_string_indexes` | Counter | Count | Unstable |
| `warm_invalid_string_indexes` | Counter | Count | Unstable |
| `warm_unsupported_images` | Counter | Count | Unstable |
| `warm_cache_hits` | Counter | Count | Unstable |
| `warm_insert_attempts` | Counter | Count | Unstable |
| `warm_inserts` | Counter | Count | Unstable |
| `warm_failed_inserts` | Counter | Count | Unstable |
| `warm_elapsed_ms` (hand-written, `double`) | Double | Milliseconds | Unstable |
| `warm_page_pressure` | Bool | None | Unstable |
| `lazy_insert_attempts` | Counter | Count | Unstable |
| `lazy_inserts` | Counter | Count | Unstable |
| `lazy_failed_inserts` | Counter | Count | Unstable |
| `lazy_elapsed_ms` (hand-written, `double`) | Double | Milliseconds | Unstable |
| `lazy_max_insert_us` | Counter | Microseconds | Unstable |
| `lazy_frames` | Counter | Count | Unstable |
| `incomplete_frames` | Counter | Count | Unstable |

### Glyph coverage block (JSON key `coverage`, TEXT header `coverage`)

| Field | Kind | Unit | Stability |
|-------|------|------|-----------|
| `grayscale_masks` | Counter | Count | Unstable |
| `lcd_rgb_masks` | Counter | Count | Unstable |
| `lcd_bgr_masks` | Counter | Count | Unstable |
| `color_images` | Counter | Count | Unstable |
| `ambiguous_images` | Counter | Count | Unstable |
| `unsupported_images` | Counter | Count | Unstable |
| `missed_images` | Counter | Count | Unstable |

### Atlas capabilities block (JSON key `capabilities`, TEXT header `capabilities`)

| Field | Kind | Unit | Stability |
|-------|------|------|-----------|
| `glyph_shader_package_available` | Bool | None | Unstable |
| `dual_source_probe_shader_package_available` | Bool | None | Unstable |
| `dual_source_blend_factors_available` | Bool | None | Unstable |
| `dual_source_blend_factors_runtime_probe` | Bool | None | Unstable |

### Atlas top-level overlap (`qsg_atlas` TEXT section / top-level JSON object)

The frame-report counter fields that overlap between the top-level JSON object
and the TEXT `qsg_atlas` section, with the same name and a plain field read.
They are emitted as two contiguous runs in each serializer, with hand-written
enum-string and boolean flags outside the shared table rows. The rasterization
run is followed by the hand-written `max_glyph_instance_page` clamp, which is
not a table row. `capture_count`, `prepare_count`, `prepare_elapsed_ns`,
`render_count`, and `render_elapsed_ns` are cumulative recorder counters and are
the atlas phase evidence to use when a consumer needs run-level prepare/render
timing. Sequence, color, cursor, and nested-summary fields describe the latest
captured or rendered report state.

| Field | Kind | Unit | Stability |
|-------|------|------|-----------|
| `capture_count` | Counter | Count | Unstable |
| `prepare_count` | Counter | Count | Unstable |
| `prepare_elapsed_ns` | Counter | Nanoseconds | Unstable |
| `render_count` | Counter | Count | Unstable |
| `render_elapsed_ns` | Counter | Nanoseconds | Unstable |
| `capture_sequence` | Counter | Count | Unstable |
| `captured_snapshot_sequence` | Counter | Count | Unstable |
| `captured_font_epoch` | Counter | Count | Unstable |
| `rasterized_glyphs` | Counter | Count | Unstable |
| `atlas_page_count` | Counter | Count | Unstable |

## Adding or changing a counter

For a legacy text-layout compatibility counter:

1. Add the field to the stats struct in
   `include/vnm_terminal/internal/qsg_terminal_renderer.h`.
2. Add one `VNM_TL_COUNTER(field)` row to the text-layout table in
   `src/diagnostics/metric_descriptor.h`, at the intended emit position.
3. Add the matching row to the table above.
4. Extend the golden fixture/oracle in
   `tests/diagnostics_text_layout/diagnostics_text_layout_tests.cpp` and confirm
   the test still passes.

For an atlas counter:

1. Add the field to the relevant atlas report struct, usually
   `include/vnm_terminal/internal/qsg_atlas_renderer.h`.
2. Add one row to the atlas descriptor table in
   `src/diagnostics/atlas_metric_descriptors.h`, at the intended emit position.
3. Add the matching row to the atlas table above.
4. Extend the relevant atlas/profile diagnostics fixture or oracle if the
   counter appears in profile text, and confirm the schema sync and diagnostics
   tests still pass.

The JSON and TEXT serializers read their descriptor tables, so descriptor edits
update both surfaces together. The golden test checks emitted profile text; the
schema-sync test checks each documented field table against the expected field
sequence. Keep order, units, and stability wording in this document aligned with
the descriptor intent during review.
