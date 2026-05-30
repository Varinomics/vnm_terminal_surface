# Flat Ring History Phase 2A Evidence: Selection Handle Resolution

## Phase 2A Scope

Phase 2A routes retained selection payload proof and visual attachment proof
through `terminal_history_handle_t` resolution. It does not change viewport
anchoring, public projection anchoring, the retained-history backend, producer
storage, resize projection, or later storage work.

## Selection Stale Policy

Selection stale policy is payload-only. When retained visual proof no longer
resolves against live history, the selection keeps its finalized copy payload,
clears visual proof, and reports `PAYLOAD_ONLY` as the selection anchor domain.
It does not clamp to a successor/predecessor row and does not clear the
copyable payload.

This preserves the established selection lifetime behavior: finalized payload
identity is separate from visual attachment proof, and stale visual proof must
fail closed for rendering while remaining copyable until the user clears or
replaces the selection.

## Phase Gate

| Gate | Phase 2A outcome |
|---|---|
| Scope | Selection retained-history proof and extraction now resolve through `terminal_history_handle_t`; no viewport anchoring, public projection, ring backend, producer rewrite, resize projection, or storage migration was implemented. |
| Behavior axis | Stale retained selection proof becomes payload-only: cached payload remains copyable, visual lease is removed, and no stale selection spans are emitted. |
| Recovery state | Recovered repaint rows keep existing provenance and use the same retained-history handle resolution seam when selected; recovery policy and producer behavior are unchanged. |
| Evidence | `cmd.exe /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 && cmake --build build --target vnm_terminal_backend_session --config Debug && ctest --test-dir build -R "vnm_terminal_backend_session" --output-on-failure'` passed on 2026-05-30. |
| Baseline outcome | Existing baseline behavior kept finalized selection payloads after retained-row mutation, eviction, clear, resize, and synchronized-output release; Phase 2A preserves that payload lifetime while replacing retained extraction proof with handle resolution. |
| Exit predicate | Retained selections store handle leases, selected text extraction uses resolved leases when retained proof exists, stale retained proof produces payload-only state, and no selection-only eviction-coordinate repair remains. |
| Deletion ownership | Phase 2A deletes the selection-only backing eviction event and controller repair method. Remaining eviction-delta consumption is viewport synchronization in `terminal_backing_delta_viewport_sync.cpp` and selection row-origin generation/deferred synchronized release accounting in `terminal_session.cpp`. |
| Rollback mechanism | Revert this Phase 2A change set: `selection_contract.h`, `selection_contract.cpp`, `terminal_screen_model.h`, `terminal_screen_model.cpp`, `terminal_session.cpp`, focused backend-session tests, this evidence artifact, and the README entry. |
| Split triggers | No viewport movement or public projection behavior was changed. If future tests require viewport clamp or public projection stale-anchor changes, that work belongs to Phase 2B or Phase 2C instead of Phase 2A. |
