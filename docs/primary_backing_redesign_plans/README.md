# Primary Backing Redesign Plan Artifacts

Authoritative current artifacts:

1. `primary_backing_buffer_consolidated_design.md`
2. `recovery_baseline_correction.md`
3. `flat_ring_history_design_plan.md`
4. `flat_ring_phase_1_history_handle_vocabulary.md`
5. `flat_ring_phase_2a_selection_handle_resolution.md`
6. `flat_ring_phase_2b_viewport_handle_resolution.md`
7. `flat_ring_phase_2c_public_projection_handle_resolution.md`
8. `flat_ring_phase_3_active_to_retained_record_producer.md`
9. `flat_ring_phase_4a_flat_ring_primitive.md`
10. `flat_ring_phase_4b_row_record_codec.md`
11. `flat_ring_phase_4c_traversal_checkpoint_rebuild.md`
12. `flat_ring_phase_4d_ring_materialization_parity_harness.md`
13. `flat_ring_phase_5a_resize_visual_projection.md`
14. `flat_ring_phase_5b_retained_hyperlink_metadata_authority.md`
15. `flat_ring_phase_5c_retained_lookup_cache_replacement.md`
16. `flat_ring_phase_6a_ring_cutover_readiness_gate.md`
17. `flat_ring_phase_6b_authoritative_ring_cutover.md`
18. `flat_ring_phase_7_recovery_shared_producer_verification.md`
19. `flat_ring_phase_8_encoding_performance_memory_tightening.md`
20. `flat_ring_phase_8_benchmark_report.json`
21. `flat_ring_phase_9_scaffold_verification_sweep.md`
22. `phase_0a_primary_backing_baseline.md`
23. `phase_0b_primary_backing_guards.md`
24. `phase_1_primary_backing_tests.md`
25. `phase_2_primary_backing_row_domains.md`
26. `phase_3_primary_backing_read_adapter.md`
27. `phase_4_primary_backing_storage_owner.md`
28. `phase_5_backing_deltas.md`
29. `phase_6a_primary_history_append_owner.md`
30. `phase_6b_primary_history_clear_evict_owner.md`
31. `phase_6c_primary_history_reflow_owner.md`
32. `phase_6d_primary_history_read_owner.md`
33. `phase_6e_active_grid_owner_state.md`
34. `phase_7a_viewport_delta_sync.md`
35. `phase_7b_wheel_public_projection_bounds.md`
36. `phase_7c_selection_domains.md`
37. `phase_8_public_projection_reconciliation.md`
38. `phase_9_scrollback_limit_viewport_shrink.md`
39. `phase_10a_scaffold_cleanup.md`
40. `phase_r1_recovery_provenance_and_resize_guard.md`
41. `phase_r2_recovery_delta_viewport_acceptance.md`
42. `phase_r3_recovery_proposal_metadata.md`
43. `phase_r4_recovery_shift_helper_extraction.md`
44. `phase_r5_recovery_disable_switch.md`
45. `primary_backing_failure_ledger.md`

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

Current flat-ring retained-history framing:

1. `flat_ring_history_design_plan.md` is the reviewed successor storage
   architecture for retained primary history.
2. It does not cancel the current primary-backing and Phase R recovery domain
   separation. It builds on those artifacts and replaces the remaining
   deque-era retained-history substrate.
3. The final storage direction is one flat ring per terminal/session, immutable
   self-contained retained records, absolute byte-sequence handles, natural
   reclamation by bounds validation, disposable caches, resize as projection,
   and recovery rows accepted through the normal retained-history producer.

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
