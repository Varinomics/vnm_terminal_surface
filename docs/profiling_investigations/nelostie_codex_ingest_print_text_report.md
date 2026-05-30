# Nelostie ingest and print_text profiling investigation

## Scope

This report investigates the ingest/parser/apply path for the captured Nelostie
stress demo profile, with emphasis on `Terminal_screen_model::apply_parser_actions`
and `Terminal_screen_model::apply_action::print_text`.

The workload is treated as intentional: the grid is large and many rows are dirty.
The useful question is what scales better under that workload, not whether the
workload should be reduced.

## Profile evidence

Directly measured profile facts from
`C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile.txt`:

| Metric | Value |
| --- | ---: |
| Grid rows | 233 |
| Grid columns | 871 |
| Grid cells | 202,943 |
| Dirty row mark requests | 7,875,373 |
| Duplicate dirty row mark requests | 7,224,579 |
| Unique pending row marks | 650,794 |
| Published unique rows | 53,632 |
| `mark_all_dirty` calls | 41 |
| `publish_pending_changes` calls from dirty-row stats | 991,700 |
| Max pending dirty rows | 272 |

Hot aggregate scope totals:

| Scope | Calls | Total | Mean | Max |
| --- | ---: | ---: | ---: | ---: |
| `Terminal_session::process_pending_commands` | 1,529 | 47.323 s | 30.950 ms | 1.613 s |
| `Terminal_session::process_backend_output_command` | 1,285 | 47.127 s | 36.675 ms | 647.557 ms |
| `Terminal_session::ingest_backend_output_segment` | 1,285 | 46.671 s | 36.320 ms | 646.538 ms |
| `Terminal_session::model_ingest` | 1,285 | 38.168 s | 29.702 ms | 630.946 ms |
| `Terminal_screen_model::ingest` | 1,285 | 38.166 s | 29.701 ms | 630.942 ms |
| `Terminal_screen_model::parser_ingest` | 1,285 | 1.801 s | 1.401 ms | 11.119 ms |
| `Terminal_screen_model::apply_parser_actions` | 1,285 | 36.100 s | 28.093 ms | 626.721 ms |
| `Terminal_screen_model::apply_action::print_text` | 472,802 | 35.398 s | 74.868 us | 10.385 ms |
| `Terminal_screen_model::apply_action::control_sequence` | 61,296 | 298.097 ms | 4.863 us | 18.030 ms |
| `Terminal_screen_model::apply_action::style_mutation` | 443,371 | 58.825 ms | 132 ns | 118.200 us |
| `Terminal_screen_model::publish_pending_changes` | 991,674 | 106.810 ms | 107 ns | 71.800 us |
| `Terminal_screen_model::render_snapshot::append_rows` | 1,252 | 7.797 s | 6.228 ms | 13.540 ms |

Derived ratios from those direct totals:

| Ratio | Value |
| --- | ---: |
| `print_text` share of `apply_parser_actions` | 98.1% |
| `print_text` share of `Terminal_screen_model::ingest` | 92.8% |
| `parser_ingest` share of `Terminal_screen_model::ingest` | 4.7% |
| `style_mutation` share of `Terminal_screen_model::ingest` | 0.15% |
| Duplicate dirty row mark request rate | 91.7% |

The direct profile evidence points at text application, not parsing and not SGR
application, as the dominant ingest cost. Rendering dirty rows is also expensive,
but it is outside this report's ingest/apply focus.

## Source evidence

Directly inspected source facts:

| Area | Evidence |
| --- | --- |
| Parser text emission | `Terminal_byte_stream_parser::ingest_buffer` accumulates printable scalars in a local `QString print_text` and flushes to `make_print_text_action` before control bytes, escape/CSI/string handling, malformed UTF-8 recovery, and end of input. |
| Apply loop | `Terminal_screen_model::ingest` iterates every parser action, clears per-action dirty state, applies the action, advances guards, publishes or collects changes, then retains active hyperlink ids. |
| `print_text` dispatch | `Terminal_screen_model::apply_action` dispatches `Screen_print_text_mutation` to `put_text(mutation.text)` under the `apply_action::print_text` scope. |
| ASCII fast path | `put_text` groups consecutive printable ASCII `QChar`s and calls `put_printable_ascii_text` for each ASCII run. |
| Row snapshot per ASCII span | `write_printable_ascii_span` copies the entire current row into `before_cells`, writes the span, then calls `advance_row_content_generation_if_changed`. |
| Full-row comparison | `advance_row_content_generation_if_changed` compares `before_cells` and the current row with `rows_have_same_selection_content`, which walks the full row and compares cell text, display width, continuation state, and occupancy. |
| Per-cell clear/write | `write_printable_ascii_span_content` writes each cell through `write_printable_ascii_cell_content`, which calls `mark_terminal_content_changed`, `clear_cell_at`, then assigns text/style/hyperlink fields. |
| Dirty rows | Dirty rows are held in `std::set<int>` for both per-action dirty state and ingest publication. `mark_dirty` has a single `m_last_dirty_row` fast-path before inserting into the set. |
| Publication | `publish_pending_changes` inserts the per-action dirty set into the publication dirty set after nearly every parser action in this profile. |

A lower-bound scaling estimate follows from the source and profile together. With
472,802 `print_text` calls and 871 columns, one whole-row copy plus one whole-row
comparison per single-row ASCII span implies at least 411.8 million `Cell` slots
copied and at least 411.8 million `Cell` slots compared before counting the actual
per-character writes. This is a lower bound because wrapped text can create more
than one span per `print_text` action.

## Likely choke points

### 1. Whole-row copy and compare on every ASCII span

Direct evidence: `print_text` accounts for 35.398 s, 98.1% of
`apply_parser_actions`. Source evidence shows the ASCII span path copies and then
compares an entire 871-cell row for each span.

Source-based inference: the cost scales with `print_actions * row_width`, not only
with changed characters. That is a poor shape for a stress workload with many
small styled text runs on a very wide grid. If most `print_text` actions write a
short run, the full-row snapshot is the likely dominant per-call cost.

### 2. Parser/action fragmentation amplifies the row-wide cost

Direct evidence: the profile has 472,802 `print_text` calls, 443,371
`style_mutation` calls, 61,296 control-sequence calls, and about 991,700 publish
calls. The parser itself costs only 1.801 s, so action construction is not the
main cost, but the action count multiplies the expensive apply-side work.

Source-based inference: the parser flushes text before escape/control/string
handling. If the stress output alternates SGR and short text runs, the parser is
correctly emitting many small text mutations. The expensive part is that each
small text mutation pays row-wide copy/compare costs.

### 3. Per-cell clearing is more expensive than the common ASCII case needs

Direct evidence: every ASCII cell write calls `clear_cell_at`, which resolves the
base cell and clears a span before overwriting the target cell.

Source-based inference: clearing per cell is necessary for correctness around
wide glyph continuations and combining marks, but most printable ASCII writes into
ordinary cells do not need the full general path for every column. A bulk ASCII
span writer can usually clear only span boundaries and any encountered wide
continuations, then assign the contiguous cells directly.

### 4. Dirty-row accumulation repeats work heavily

Direct evidence: dirty-row stats record 7,875,373 mark requests, of which
7,224,579 are duplicates. Only 53,632 rows were newly published into ingest
publication sets.

Source-based inference: the current per-action `clear_dirty` and
`publish_pending_changes` structure prevents a single ingest-wide dirty accumulator
from suppressing repeated row insertions across adjacent actions. The profile
shows `publish_pending_changes` itself is cheap at 106.810 ms, so this is not the
primary measured scope, but the dirty-row work is included inside hot mutation
scopes such as `print_text`.

## Improvement options

| Option | Likely benefit | Risk | Suggested validation |
| --- | --- | --- | --- |
| Replace full-row copy/compare for ASCII spans with range-based change detection. Capture only touched cells and affected wide-boundary cells, or compare old/new values as the span is written. | High. Removes the `print_actions * columns` copy/compare shape from the dominant path. | Medium. Must preserve retained-line content generation semantics for selection, retained history identity, and recovery. | Add profiling scopes for row snapshot copy, row comparison, and cell writes. Run before/after on this profile and targeted tests for retained row identity changes. |
| Add a true bulk printable-ASCII span writer. Assign contiguous one-cell ASCII cells directly with the current style/hyperlink, clearing only necessary wide-span boundaries and encountered continuations. | High. Reduces per-character helper calls and avoids redundant `clear_cell_at` work in the common case. | Medium to high. Wide glyph boundaries, no-autowrap margin behavior, hyperlinks, and combining marks need focused coverage. | Build fixtures for ASCII over normal cells, ASCII overwriting wide glyph starts/continuations, no-autowrap clipping, autowrap at column 871, and hyperlink/style transitions. |
| Accumulate dirty rows in an ingest-level fixed-size structure, not per-action `std::set` churn. For 233 rows, a bitset/vector plus ordered row list should be enough. | Medium. Direct dirty publication scope is small, but duplicate mark traffic is very high and occurs inside hot mutation scopes. | Medium. Must preserve stable mutation identity flags and synchronized-output behavior. | Compare dirty row vectors and `dirty_rows_have_stable_mutation_identity` for existing fixtures. Profile `mark_dirty` separately before changing behavior. |
| Publish/collect dirty state at coarser safe boundaries instead of after every parser action when synchronized output is not active. | Medium if it also suppresses repeated row/set work; low if it only removes `publish_pending_changes` calls. | Medium to high. The current per-action publication may be coupled to mutation identity and recovery guard behavior. | First instrument action count, publication count, and per-action dirty rows. Then prove render/session behavior is unchanged for mixed text, cursor, scroll, resize, and synchronized-output cases. |
| Skip or coalesce no-op SGR actions before they split text application. | Workload-dependent. `style_mutation` itself is cheap, but no-op SGR can fragment text and multiply row-wide text costs. | Medium. Must not merge across style/hyperlink changes that affect cell metadata. | Add counters for SGR actions that leave `m_current_style_id` unchanged and for adjacent print actions separated only by no-op style changes. Validate with styled output fixtures. |
| Longer-term row representation optimized for runs rather than per-cell `QString` cells. | Potentially high for very wide grids and styled stress output. | High. Broad renderer, selection, history, and retained-identity impact. | Treat as separate governed design work only after proving local ASCII span changes are insufficient. |

## Recommended first batch

The lowest-risk high-yield path is to instrument and then remove the full-row
snapshot cost from `write_printable_ascii_span`.

Recommended sequence:

1. Add temporary or guarded profiling scopes inside `print_text` for ASCII span
   write, row snapshot copy, row content-generation comparison, `clear_cell_at`,
   and dirty marking.
2. Re-run the same Nelostie stress demo and confirm whether row copy/compare is
   the dominant sub-cost inside `print_text`.
3. Implement range-based row change detection for ASCII spans, preserving the
   exact retained-line generation contract.
4. Re-run the same profile and compare `print_text` total, mean, max, and dirty
   row counters.
5. Only after that, decide whether bulk cell assignment or dirty-row accumulator
   changes are still needed.

This sequence avoids guessing across multiple interacting optimizations and fits
change-governance expectations: one canonical path is improved, no compatibility
path is added, and later work is not silently deferred without evidence.

## Validation checklist

Use the captured stress demo as the performance gate. The key before/after
metrics are:

| Metric | Target direction |
| --- | --- |
| `Terminal_screen_model::apply_action::print_text total_ns` | Down substantially |
| `Terminal_screen_model::apply_action::print_text mean_ns` | Down substantially |
| `Terminal_screen_model::apply_parser_actions total_ns` | Down roughly with `print_text` |
| `Terminal_screen_model::parser_ingest total_ns` | No material regression |
| Dirty row mark requests and duplicates | Down or unchanged |
| Render snapshot dirty rows | Equivalent output behavior |

Correctness validation should include:

| Case | Reason |
| --- | --- |
| ASCII spans in one row | Main optimized path. |
| ASCII spans wrapping at the final column | Exercises pending-wrap and cursor dirtiness. |
| No-autowrap writes past the right margin | Exercises clipping behavior. |
| ASCII overwriting wide glyph starts and continuations | Guards against broken wide-span cleanup. |
| Combining marks before and after ASCII writes | Guards zero-width scalar attachment. |
| Hyperlink and style changes around short text runs | Guards cell metadata. |
| Scroll, insert/delete lines, and retained history | Guards row content generation and retained identity. |
| Synchronized output mode | Guards publication timing semantics. |

## Files inspected

| File | Purpose |
| --- | --- |
| `C:\plms\varinomics\varinomics-standards\varinomics_coding_style_guideline.md` | Report style and standards context. |
| `C:\plms\varinomics\varinomics-standards\varinomics_coding_style_llm_addendum.md` | Report style and standards context. |
| `C:\plms\varinomics\varinomics-standards\varinomics_review_scope.md` | Review/report scope expectations. |
| `C:\plms\varinomics\varinomics-standards\varinomics_change_governance.md` | Change-governance framing. |
| `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile.txt` | Profile evidence. |
| `C:\plms\varinomics\vnm_terminal_surface\src\terminal_screen_model.cpp` | Parser, ingest, apply, text writing, dirty-row implementation. |
| `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\terminal_screen_model.h` | Model data structures, result/publication types, dirty-row storage. |
| `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\parser_action.h` | Parser action and print mutation representation. |

## Report artifact

Wrote this report to:

`C:\plms\varinomics\vnm_terminal_surface\docs\profiling_investigations\nelostie_codex_ingest_print_text_report.md`
