# Primary Backing Redesign Plan Artifacts

Authoritative current artifacts:

1. `primary_backing_buffer_consolidated_design.md`
2. `recovery_baseline_correction.md`
3. `phase_0a_primary_backing_baseline.md`
4. `phase_0b_primary_backing_guards.md`
5. `phase_1_primary_backing_tests.md`
6. `phase_2_primary_backing_row_domains.md`
7. `phase_3_primary_backing_read_adapter.md`
8. `phase_4_primary_backing_storage_owner.md`
9. `phase_5_backing_deltas.md`
10. `phase_6a_primary_history_append_owner.md`
11. `phase_6b_primary_history_clear_evict_owner.md`
12. `phase_6c_primary_history_reflow_owner.md`
13. `phase_6d_primary_history_read_owner.md`
14. `phase_6e_active_grid_owner_state.md`
15. `phase_7a_viewport_delta_sync.md`
16. `phase_7b_wheel_public_projection_bounds.md`
17. `phase_7c_selection_domains.md`
18. `phase_8_public_projection_reconciliation.md`
19. `phase_9_scrollback_limit_viewport_shrink.md`
20. `phase_10a_scaffold_cleanup.md`
21. `phase_r1_recovery_provenance_and_resize_guard.md`
22. `phase_r2_recovery_delta_viewport_acceptance.md`
23. `phase_r3_recovery_proposal_metadata.md`
24. `phase_r4_recovery_shift_helper_extraction.md`
25. `phase_r5_recovery_disable_switch.md`
26. `primary_backing_failure_ledger.md`

Historical review inputs were used to build these artifacts, but they are not
kept in this product documentation tree. Older archived reports are not current
implementation instructions when they conflict with the authoritative artifacts.

Current recovery-policy framing:

1. The existing repaint-recovery policy is not permanently rejected.
2. Core backing, viewport, resize, selection, and publication phases must not
   use recovery as storage evidence or as a passing condition.
3. Recovery is deferred to Phase R, where it must be restructured on top of the
   canonical backing model and accepted through normal storage APIs with
   recovered provenance.
4. Older recommendations to delete the recovery flag or make the tree unable to
   accept the heuristic are superseded by this framing.

Current phase order begins with `recovery_baseline_correction.md`, then Phase
0A, Phase 0B, Phase 1 (`phase_1_primary_backing_tests.md`), the
Phase 2 row-domain refactor (`phase_2_primary_backing_row_domains.md`), Phase
3 read-adapter refactor (`phase_3_primary_backing_read_adapter.md`), the
Phase 4 storage-owner refactor
(`phase_4_primary_backing_storage_owner.md`), the Phase 5 backing-delta surface
(`phase_5_backing_deltas.md`), the Phase 6A append-owner extraction
(`phase_6a_primary_history_append_owner.md`), the Phase 6B clear/evict owner
extraction (`phase_6b_primary_history_clear_evict_owner.md`), the Phase 6C
retained-history reflow owner extraction
(`phase_6c_primary_history_reflow_owner.md`), the Phase 6D retained-history
read owner extraction (`phase_6d_primary_history_read_owner.md`), the Phase 6E
active-grid owner-state extraction (`phase_6e_active_grid_owner_state.md`), the
Phase 7A viewport delta-sync adapter
(`phase_7a_viewport_delta_sync.md`), the Phase 7B public scroll/wheel bounds
fix (`phase_7b_wheel_public_projection_bounds.md`), the Phase 7C selection
domain/invalidation hardening (`phase_7c_selection_domains.md`), the Phase 8
public projection/reconciliation regression hardening
(`phase_8_public_projection_reconciliation.md`), the Phase 9 scrollback-limit
viewport-shrink regression hardening
(`phase_9_scrollback_limit_viewport_shrink.md`), the Phase 10A scaffold
cleanup (`phase_10a_scaffold_cleanup.md`), Phase R1 recovery provenance and
resize-adjacent false-positive suppression
(`phase_r1_recovery_provenance_and_resize_guard.md`), Phase R2 recovery delta
viewport acceptance (`phase_r2_recovery_delta_viewport_acceptance.md`), and
Phase R3 recovery proposal metadata
(`phase_r3_recovery_proposal_metadata.md`), Phase R4 recovery shift helper
extraction (`phase_r4_recovery_shift_helper_extraction.md`), and Phase R5
recovery disable switch (`phase_r5_recovery_disable_switch.md`), followed by
any later Phase R recovery-policy extraction if that extraction is approved.
