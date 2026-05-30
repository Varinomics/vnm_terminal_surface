Report written to `docs/reviews/02_claude_scaling_strategy.md`.

**Top-line evidence from the profile (read-only investigation, no code changes):**

- **GUI thread (48.3 s total)** is dominated by `apply_action::print_text` at **35.4 s** (73%) and `render_snapshot::append_rows` at **7.8 s**. The retained-history ring is **~15 ms total** — immaterial at this stress level (confirms scope assumption that retained-history work is not the lever).
- **Render thread (19.7 s total)** is dominated by `sync_text_resource_nodes` at **10.5 s** (53%) plus the two-pass `build_terminal_render_frame` at **8.3 s**.

**Three high-severity architectural findings**, all with the same shape (work scales with *grid* size, not *dirty* size):

1. `print_text` ASCII fast path makes a full row copy + element-by-element diff per span just to gate a generation counter (`terminal_screen_model.cpp:4524-4535` + `:4181-4197`).
2. `render_snapshot::append_rows` copies *all* 233 rows per snapshot; dirty-row metadata is computed but not used for pruning (`terminal_screen_model.cpp:2856-2889`).
3. QSG text cache is keyed on a frame-wide hash (`qsg_terminal_renderer.cpp:3491-3543`), so any dirty row invalidates the entire text-node graph — the per-row reuse fast paths exist but are unreachable on partial-dirty frames.

**Test gap is the most actionable surprise**: 65+ render-related unit tests but every fixture is = 9-row grids. The only real scaling benchmark (`history_row_record_phase8_benchmark.cpp`, 2000 rows) covers the path that turned out to be immaterial.

The report includes a 5-phase implementation roadmap with measurable validation gates (counter pins, regression tests) that should land in front of the fixes.

