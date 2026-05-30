# Focus D dirty-row semantics, publication cadence, and event-loop scheduling audit

## Executive summary

The profile does not support the hypothesis that excessive render-snapshot publication is the primary cost. It does show very high dirty-mark and model-level pending-publication churn, but that churn is mostly suppressed or coalesced before render publication:

- `mark_requests=4,380,137`, `duplicate_mark_requests=4,258,550` (97.2% duplicate marks).
- `publish_pending_calls=174,953`, but only `published_unique_rows=14,379` (0.082 newly published rows per pending publish).
- `render_snapshot_requests=1,498` and `render_snapshot_publications=1,498`, so the model-level pending-publication loop is reduced by roughly 117x before snapshot publication.
- `dirty_rows_snapshot_calls=1,500`, `dirty_rows_snapshot_rows=14,379`, matching the render request dirty-row volume.
- `dirty_coalescing_attempts=45`, `dirty_coalescing_applied=45`, and `max_unrendered_snapshot_generations=11`, which means the GUI/render side can fall behind and occasionally merges dirty ranges, but coalescing is not frequent enough to explain the whole workload.

The stronger performance evidence is downstream: every render snapshot visits and materializes every visible row despite dirty rows being sparse. The profile has `render_snapshot_rows_visited=352,030` and `render_snapshot_rows_materialized=352,030`, exactly `1,498 snapshots * 235 rows`. It scans `307,322,190` cells, exactly `352,030 rows * 873 columns`, while only `14,379` dirty rows were requested/visible. Dirty rows therefore exist and are coalesced correctly as metadata, but they do not prune snapshot materialization or later render-frame classification.

Conclusion: dirty-row semantics are not obviously dropping rows. Publication cadence has noisy model-level churn, but the direct measured cost of `Terminal_screen_model::publish_pending_changes` is tiny compared with snapshot/render work. Perceived bad performance is more likely driven by full-frame materialization/render-frame rebuild work per published snapshot, plus render-generation backlog during bursts, than by missed batching in the dirty-row publisher alone.

## Dirty-row evidence

Profile counters from `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile_scalar_span.txt`:

- Dirty marking is extremely noisy: `4,380,137` mark requests, `4,258,550` duplicates, `121,587` unique pending row marks.
- Duplicate suppression is active in `Terminal_screen_model::mark_dirty`: same-row repeats return early via `m_last_dirty_row`, and set insertion suppresses non-consecutive duplicates (`src\terminal_screen_model.cpp:6212`).
- `mark_all_dirty_calls=16` can legitimately produce full-grid pending dirty sets. The profile's `max_pending_dirty_rows=235` matches the 235-row geometry.
- Pending dirty rows are snapshotted `1,500` times with `14,379` total rows, and render snapshot dirty rows requested/visible are also `14,379`. That argues against dirty rows being lost between model publication and render snapshot construction.
- `render_snapshot_full_repaint_fallbacks=0`, `render_snapshot_viewport_fallbacks=0`, and `render_snapshot_zero_dirty_publications=0`; the profile does not show viewport fallback repainting or zero-dirty content snapshots as the driver.

Worst 100 ms dirty/publication buckets show very high duplicate churn but limited dirty-row output:

- `90,300-90,400 ms`: `281,033` marks, `279,283` duplicates, `1,750` unique marks, `2,981` pending publishes, `6` dirty-row snapshots / `168` rows, `max_pending=2`.
- `91,300-91,400 ms`: `218,068` marks, `216,252` duplicates, `1,816` unique marks, `3,060` pending publishes, `5` dirty-row snapshots / `177` rows, `max_pending=2`.
- `94,700-94,800 ms`: `115,878` marks, `113,072` duplicates, `2,806` unique marks, `2,875` pending publishes, `5` dirty-row snapshots / `643` rows, `max_pending=235`.

Source evidence:

- `Terminal_screen_model::ingest` clears dirty state before each parser action, applies one action, then either collects synchronized changes or publishes pending changes into an ingest-level publication (`src\terminal_screen_model.cpp:1942`).
- `publish_pending_changes` inserts current `m_dirty_rows` into the aggregate publication, ORs change flags, then clears dirty state (`src\terminal_screen_model.cpp:6598`).
- At ingest finalization, the aggregate publication becomes `m_dirty_rows`, then `result.dirty_rows = dirty_rows()` snapshots that aggregate (`src\terminal_screen_model.cpp:1972`).
- `dirty_rows()` copies the set to a vector and increments snapshot counters (`src\terminal_screen_model.cpp:6538`).
- Synchronized-output paths collect into `m_synchronized_dirty_rows` and only release them later; this profile has `collect_synchronized_calls=0` and `release_synchronized_calls=0`, so synchronized-output batching was not active in the sampled run.

## Publication/coalescing map

Pipeline observed in source:

1. Parser output enters `Terminal_screen_model::ingest`.
2. For each parser action, dirty state is reset, action logic marks rows, then `publish_pending_changes` merges row marks and flags into a single `ingest_publication_t` unless synchronized output is active.
3. End of ingest stores the aggregate row set and returns `Terminal_screen_model_result` with dirty rows.
4. `Terminal_session::ingest_backend_output_segment` stores the model result in `m_render_snapshot_model_result` and only calls `publish_render_snapshot` if the result warrants a snapshot, metadata changed, or visual bell is active (`src\terminal_session.cpp:4322`).
5. `make_render_snapshot_request` copies `m_render_snapshot_model_result->dirty_rows` and viewport/mouse-reporting flags into the request (`src\terminal_session.cpp:5277`).
6. `Terminal_screen_model::render_snapshot` compacts dirty rows into ranges with `compact_dirty_row_ranges(viewport_dirty_rows(...))` (`src\terminal_screen_model.cpp:2852`).
7. The same render snapshot then unconditionally loops `row = 0 .. grid_size.rows - 1`, materializes every row, appends cells, hyperlink metadata, and line provenance (`src\terminal_screen_model.cpp:2874`). Dirty rows are metadata here, not a pruning input.
8. `Terminal_session::publish_render_snapshot` constructs the snapshot synchronously, then, if the previous generation has not been synced by the surface, coalesces dirty row ranges with the latest content/render snapshot (`src\terminal_session.cpp:5990`). This coalescing happens after full snapshot construction, so it preserves damage metadata but does not reduce snapshot-construction cost.
9. `VNM_TerminalSurface::sync_from_session` observes `render_snapshot_generation`, installs the latest snapshot handle, calls `mark_render_snapshot_synced`, and updates viewport state (`src\vnm_terminal_surface.cpp:4984`).
10. `request_render_update` coalesces QQuickItem `update()` calls while one render update is pending (`src\vnm_terminal_surface.cpp:1557`).
11. `updatePaintNode` builds a full `Terminal_render_frame` from the current snapshot and passes it to `Qsg_terminal_renderer::update_node` (`src\vnm_terminal_surface.cpp:5302`).
12. `build_terminal_render_frame` and packed-data construction iterate over `snapshot->cells`, and dirty-row checks are per-cell classification inputs (`src\qsg_terminal_renderer.cpp:6040`, `src\qsg_terminal_renderer.cpp:5957`).
13. `sync_text_resource_nodes` has clean-row reuse paths, but the first clean-row skip is gated by `same_text_frame_key`; `text_frame_cache_key(frame, text_font_key)` is frame-wide (`src\qsg_terminal_renderer.cpp:3474`). If any dirty row changes the frame key, some clean-row reuse paths become unreachable.

Important distinction: the model's `publish_pending_changes` churn is not the same as render publication churn. The profile shows ~175k pending publishes but only ~1.5k render snapshot publications.

## Scheduling/backpressure hypotheses

- Backend callback scheduling is already GUI-wakeup coalesced. `queue_backend_callback_drain` uses `session_drain_queued` and posts one `Qt::QueuedConnection` lambda; additional backend callbacks do not post unlimited wakeups while the latch is set (`src\vnm_terminal_surface.cpp:4868`).
- `process_pending_commands` supports budgeted backend-output slicing when a deadline is supplied, pushing the remainder back to the front of the queue (`src\terminal_session.cpp:3082`). This protects the GUI loop from one huge backend-output command monopolizing a drain call, but each processed slice can still produce a full render snapshot if it warrants publication.
- Output backpressure is present: enqueueing output tracks queue high-water state and can pause backend output (`src\terminal_session.cpp:3025`, `src\terminal_session.cpp:6356`). `pause_backend_output_from_callback_ingress` can also activate backpressure when callback/output queues are high (`src\terminal_session.cpp:4059`).
- QQuickItem render invalidation is coalesced: repeated `request_render_update` calls while the same window has a pending update increment `coalesced_requests` and avoid another `update()` (`src\vnm_terminal_surface.cpp:1571`).
- The profile's `max_unrendered_snapshot_generations=11` shows the producer can publish snapshots faster than the surface/render loop consumes them during bursts. Session dirty coalescing catches 45 such cases, but because it runs after snapshot construction, it does not reduce the expensive `render_snapshot::append_rows` work.
- Bursty buckets show 4-7 render snapshots per 100 ms in some windows, and each snapshot costs several milliseconds in `render_snapshot::append_rows`; that is enough to create visible backlog even with update coalescing.
- The profile aggregate has `Terminal_screen_model::publish_pending_changes` around 20 ms total for ~175k calls, while `Terminal_screen_model::render_snapshot::append_rows` is about 10.6 s and `Terminal_session::publish_render_snapshot` about 11.4 s. Therefore, pending-publication churn is a scalability smell but not the current dominant measured cost.

## Low-risk batching options

1. Add an early no-op fast path in `publish_pending_changes` when `m_dirty_rows` is empty and all current change flags are false. This preserves the per-action publication structure but avoids set insertion and stats work for no-op actions.
2. Move ordinary, non-synchronized `publish_pending_changes` out of the per-action loop and publish once at the end of `ingest`, if no consumer depends on action-by-action intermediate publication. This should reduce `publish_pending_calls` by roughly the parser-actions-per-ingest factor without changing render snapshot cadence.
3. Keep the per-action clear/mark semantics but aggregate into a fixed row bitset plus compact vector instead of `std::set<int>`. This targets the high duplicate mark volume while preserving sorted unique output at `dirty_rows()` time.
4. Move unsynced-generation dirty coalescing before expensive snapshot construction. If `m_render_snapshot_synced_generation < m_render_snapshot_generation`, merge prior dirty ranges into the new request's dirty rows before calling `m_screen_model->render_snapshot`. This will not reduce current full-row materialization by itself, but it makes the coalescing location compatible with later dirty-pruned snapshot construction.
5. Batch render snapshot publication per GUI drain/tick under backend-output burst conditions. Instead of constructing every intermediate content snapshot synchronously, accumulate dirty rows and latest metadata until the end of a budgeted drain or until the GUI is ready to sync. This is higher risk than options 1-4 because selection, viewport, synchronized-output, and transcript sequencing all depend on publication order.
6. Treat dirty rows as a pruning input only after the snapshot contract supports retained clean rows. Current snapshots carry a full `cells` vector and validation/consumers assume complete coverage. A safe incremental design needs per-row generation/provenance and consumer caches before `render_snapshot::append_rows` can skip clean rows.

## Risks

- Dirty rows are render damage metadata, not semantic identity. Selection and provenance paths already avoid treating dirty rows as stable mutation proof; batching must not reintroduce that coupling.
- Moving `publish_pending_changes` out of the action loop could change behavior if any generated action or recovery path expects publication flags to be cleared after each action. The current loop deliberately clears dirty state after each publish.
- Synchronized-output release is a separate batching path. This profile did not exercise it, so any batching change must validate `collect_synchronized_changes` and `release_synchronized_changes` independently.
- Pre-snapshot coalescing must preserve full repaint semantics for viewport changes. `compact_dirty_row_ranges(..., full_repaint=true)` currently converts viewport changes to full dirty ranges.
- Render snapshot generation is also used as a surface synchronization/backpressure signal. Suppressing intermediate generations may improve performance but can affect transcript capture, selection-derived snapshots, geometry-derived snapshots, and public projection scroll snapshots.
- QSG text reuse is partly gated by frame-wide keys. Even perfect dirty-row batching upstream may not produce expected render-thread wins until clean-row reuse paths are reachable when only dirty rows changed.

## Validation gates

- Profile gate: `publish_pending_calls` should drop if batching option 1 or 2 is applied, while `published_unique_rows`, `dirty_rows_snapshot_rows`, and `render_snapshot_dirty_rows_visible` remain equal for the same replay.
- Profile gate: `render_snapshot_requests` and `render_snapshot_publications` should not increase. For render-cadence batching, they should decrease during burst buckets without increasing zero-dirty publications.
- Profile gate: `max_unrendered_snapshot_generations` should decrease under burst replay; if it rises, batching did not relieve producer/consumer imbalance.
- Profile gate: until true incremental snapshots land, `render_snapshot_rows_visited` will remain `render_snapshots_constructed * rows`. After incremental snapshots, this counter must shrink toward dirty-row volume, and full repaint/viewport fallback counters must explain any full-frame cases.
- Correctness gate: dirty ranges remain sorted, non-overlapping, in-bounds, and full-grid on viewport/public-projection repaint paths.
- Correctness gate: synchronized-output tests/replays must show held output is not published early and forced release still emits complete dirty damage.
- Correctness gate: selection-derived and geometry-derived snapshots still advance or preserve content basis exactly as before.
- UI gate: render invalidation stats should show update requests coalescing rather than one scheduled update per snapshot during backend bursts.
- Render gate: QSG stats should show clean row reuse increasing only after frame-key/per-row reuse gating is addressed; dirty batching alone should not be credited for that unless the metric proves it.

## Files inspected

- `C:\plms\varinomics\varinomics-standards\varinomics_coding_style_guideline.md`
- `C:\plms\varinomics\varinomics-standards\varinomics_coding_style_llm_addendum.md`
- `C:\plms\varinomics\varinomics-standards\varinomics_review_scope.md`
- `C:\plms\varinomics\vnm_terminal\build\profiles\nelostie_profile_scalar_span.txt`
- `C:\plms\varinomics\vnm_terminal_surface\src\terminal_screen_model.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\src\terminal_session.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\src\vnm_terminal_surface.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\src\qsg_terminal_renderer.cpp`
- `C:\plms\varinomics\vnm_terminal_surface\include\vnm_terminal\internal\render_snapshot.h`
