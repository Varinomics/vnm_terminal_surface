Report delivered above as required. Key conclusions:

- **Residual print_text comparison attribution is real.** Tree at `nelostie_profile_span_local.txt:1171–1172` proves 876 318 of 887 639 global comparisons (98.7%) are directly under `apply_action::print_text`, all coming from the `install_cell_span` / `append_zero_width_scalar` paths reachable via `put_text ? put_scalar`. The optimised printable-ASCII path bypasses `advance_row_content_generation_if_changed` and is no longer a contributor.
- **Counter integrity validates** (876 318 + 6 127 control_seq + 5 194 publish = 887 639 = global counter), but counter math alone cannot attribute work to callers because `row_content_generation_comparisons` and `row_content_generation_advances` are single global buckets.
- **Missing counters** that block the next slice's validation: per-caller attribution for the two `row_content_generation_*` totals; `print_text_total_characters` + `print_text_non_ascii_scalar_calls` workload denominators; a split of `printable_ascii_local_cells_inspected` into matched vs short-circuit; a timer scope on `printable_ascii_span_changes_selection_content`; a profile workload fingerprint.
- **Test coverage is weak** — no unit test asserts any model_profile_stats field, despite `VNM_TERMINAL_PROFILING_ENABLED=1` being set for the test build.
- **Dead struct fields** `printable_ascii_row_copies` / `printable_ascii_row_copy_cells` are defined but unincremented; emitted as zeros in the profile output.

