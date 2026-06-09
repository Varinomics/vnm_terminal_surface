# Diagnostics Schema

This document describes the renderer diagnostics counters that the surface
serializes for inspection. It is kept in sync with the descriptor table in
`src/diagnostics/metric_descriptor.h`; that table is the single source of truth
for the field list, and the byte-golden test
`tests/diagnostics_text_layout/diagnostics_text_layout_tests.cpp` pins the
output against it.

## Two serializations, one field list

Every counter block is emitted in two forms:

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
text-layout block now flows through one shared descriptor table
(`metric_descriptor.h`) consumed by both `emit_metrics_json` and
`emit_metrics_text`, so the field set and order cannot diverge. For the
text-layout block the JSON key and the TEXT label are identical strings.

## Descriptor model

A `Metric_descriptor<Stats>` row carries:

| Column      | Meaning                                                              |
|-------------|---------------------------------------------------------------------|
| `json_key`  | JSON object key.                                                    |
| `text_label`| TEXT report label (identical to `json_key` for the text-layout block). |
| `kind`      | `COUNTER` (numeric) or `BOOL`.                                      |
| `unit`      | `COUNT`, `BYTES`, `MICROSECONDS`, `MILLISECONDS`, or `NONE`.        |
| `stability` | `STABLE` or `UNSTABLE` (see below).                                 |
| reader      | A stateless function pointer that reads the field from `Stats` (one reader per kind; the other is null). |

Each reader casts its field to `std::uint64_t`, which lets one table serve both
the `int`-typed per-frame stats struct and the `std::uint64_t`-typed cumulative
stats struct.

### Stability

These are debug/diagnostic counters. They reflect internal renderer accounting
(fast-path screening, classification buckets, cache reuse) and are expected to
change as the renderer evolves. They are therefore marked **`UNSTABLE`**: tools
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
