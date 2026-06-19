# Diagnostics Schema

This document describes the renderer diagnostics counters that the surface
serializes for inspection. It is kept in sync with the descriptor tables in
`src/diagnostics/metric_descriptor.h` (the text-layout block) and
`src/diagnostics/atlas_metric_descriptors.h` (the atlas blocks); those tables
are the single source of truth for the field lists, and the byte-golden test
`tests/diagnostics_text_layout/diagnostics_text_layout_tests.cpp` pins the
output against them.

## Descriptor-backed serializations

Descriptor-backed counter blocks are emitted in two forms:

- **JSON** (`src/diagnostics/metrics_json.cpp`, always built): the runtime
  metrics document. Counters are emitted as decimal **strings** (so a 64-bit
  value is never silently narrowed to a JSON `double`); booleans are emitted as
  native JSON `true`/`false`. JSON object keys are unordered; consumers must
  look counters up by name, not by position.
- **TEXT** (`src/diagnostics/profile_text.cpp`, built only when
  `VNM_TERMINAL_ENABLE_PROFILING=ON`): the human-readable profile report. Each
  counter is one line, `  <label>=<value>`, and **order is significant** -- the
  report is read top to bottom and diffed line by line.

Historically each serializer hand-wrote its own copy of the field list, so the
two could drift (a counter added to JSON but not TEXT, or reordered). The
text-layout block and several atlas blocks now flow through one shared
descriptor table each, consumed by both `emit_metrics_json` and
`emit_metrics_text`, so the field set and order cannot diverge. For every
table-driven block the JSON key is also the TEXT label.

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

## Runtime JSON-only sections

These sections are emitted only by the runtime JSON helpers in
`vnm_terminal/diagnostics/metrics_json.h`. Counter values are decimal strings;
boolean values are JSON booleans. These diagnostics are `UNSTABLE`.

### Render invalidation block (JSON key `render_invalidation`)

| Field | Kind | Unit | Stability |
|-------|------|------|-----------|
| `update_requests` | Counter | Count | Unstable |
| `scheduled_updates` | Counter | Count | Unstable |
| `coalesced_requests` | Counter | Count | Unstable |
| `consumed_updates` | Counter | Count | Unstable |
| `backend_callback_frame_deferrals` | Counter | Count | Unstable |
| `backend_callback_event_epoch` | Counter | Count | Unstable |
| `backend_callback_frame_boundary_epoch` | Counter | Count | Unstable |
| `render_snapshot_callback_epoch` | Counter | Count | Unstable |
| `last_rendered_snapshot_sequence` | Counter | Count | Unstable |
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

## Descriptor model

A `Metric_descriptor<Stats>` row carries:

| Column     | Meaning                                                              |
|------------|----------------------------------------------------------------------|
| `json_key` | JSON object key and TEXT report label.                               |
| `kind`     | `COUNTER` (numeric) or `BOOL`.                                       |
| reader     | A stateless function pointer that reads the field from `Stats` (one reader per kind; the other is null). |

Each reader casts its field to `std::uint64_t`, which lets one table serve both
the `int`-typed per-frame stats struct and the `std::uint64_t`-typed cumulative
stats struct.

The `Unit` and `Stability` columns in the tables below are schema
documentation, not stored descriptor fields.

### Stability

These are debug/diagnostic counters. They reflect internal renderer accounting
(fast-path screening, classification buckets, cache reuse) and are expected to
change as the renderer evolves. They are therefore treated as unstable: tools
may read them for investigation, but no external contract guarantees a counter
name, its meaning, or its presence across versions. They are not a public API.

## Text-layout block

Counters describing the QTextLayout slow path and the ASCII-replacement fast
path. Run counters tally text runs; `*_code_units` counters tally UTF-16 code
units. All are `COUNTER` / `COUNT` / `UNSTABLE`. The block is emitted in the
order below; `text_ascii_replacement_add_glyphs_calls` is optional (emitted only
when the stats type carries it) and is the one field handled outside the shared
table loop.

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
hand-written in the serializers. All atlas diagnostics here are `UNSTABLE`.

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
emitted outside the shared table (so they are not table rows below).

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
| `warm_elapsed_ms` (hand-written, `double`) | — | Milliseconds | Unstable |
| `warm_page_pressure` | Bool | None | Unstable |
| `lazy_insert_attempts` | Counter | Count | Unstable |
| `lazy_inserts` | Counter | Count | Unstable |
| `lazy_failed_inserts` | Counter | Count | Unstable |
| `lazy_elapsed_ms` (hand-written, `double`) | — | Milliseconds | Unstable |
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
They are emitted as two contiguous runs in each serializer (separated there by
hand-written enum-string and boolean flags, and by the hand-written
`max_glyph_instance_page` clamp, which are not table rows).

| Field | Kind | Unit | Stability |
|-------|------|------|-----------|
| `capture_count` | Counter | Count | Unstable |
| `prepare_count` | Counter | Count | Unstable |
| `render_count` | Counter | Count | Unstable |
| `capture_sequence` | Counter | Count | Unstable |
| `captured_snapshot_sequence` | Counter | Count | Unstable |
| `captured_font_epoch` | Counter | Count | Unstable |
| `rasterized_glyphs` | Counter | Count | Unstable |
| `atlas_page_count` | Counter | Count | Unstable |

## Adding or changing a counter

1. Add the field to the stats struct in
   `include/vnm_terminal/internal/qsg_terminal_renderer.h`.
2. Add one `VNM_TL_COUNTER(field)` row to the text-layout table in
   `src/diagnostics/metric_descriptor.h`, at the intended emit position.
3. Add the matching row to the table above.
4. Extend the golden fixture/oracle in
   `tests/diagnostics_text_layout/diagnostics_text_layout_tests.cpp` and confirm
   the test still passes.

Because both serializers read the one table, step 2 updates JSON and TEXT
together; the golden test fails loudly if the doc, table, and output disagree.
