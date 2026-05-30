Report delivered above. Summary of dominant measured costs from `nelostie_profile.txt` and the source-level root causes I traced:

- **35.4s / 73% of gui wall** in `apply_action::print_text` self time ? per-write 871-cell row-vector copy + compare in `write_printable_ascii_span` / `install_cell_span` (terminal_screen_model.cpp:4530, 4677). Largest single lever.
- **7.8s / 16%** in `render_snapshot::append_rows` ? loops all 233 rows per snapshot, `viewport_row_cells` returns full row by-value (terminal_screen_model.cpp:6850).
- **~6.4s of throw-away work**: 4.27 snapshots produced per painted frame because `model_result_warrants_render_snapshot` (terminal_session.cpp:44) publishes on every backend segment with no producer-side coalescing.
- **991K publish_pending_changes calls** and **147× dirty-row write amplification** from the per-action clear/mark/publish loop over `std::set<int> m_dirty_rows`.

Test coverage is strong on correctness but has no perf contracts or stress-grid fixture, so any of these architectural regressions would be invisible to the existing suite.

