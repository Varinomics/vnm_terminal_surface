# Selection Row Provenance Manual Validation Checklist

## Scope

Use this checklist for the original Codex active-output
selection-drift/vanishing scenario after the retained-line provenance phases.
It complements automated backend/session/surface tests; it does not replace
them.

## Existing repository hints

- `docs/selection_lifetime_implementation.md` names the expected manual target
  as the `vnm_terminal` validation app built against this local
  `vnm_terminal_surface` checkout, with the Qt runtime available on `PATH`.
- The same document names the Codex launch shape as Codex started with
  `--dangerously-bypass-approvals-and-sandbox`.
- The existing surface trace hook is enabled by passing `--selection-trace`
  before `--` in the validation app launch.
- This repository does not provide a single universal app launch command in the
  docs read for Phase 6. Use the launch command from the active validation
  environment and add `--selection-trace` when diagnostics are needed.

## Active-output preservation scenario

- [ ] Launch the validation app built against this checkout.
- [ ] Pass `--selection-trace` before `--` if collecting diagnostics.
- [ ] Start Codex in the terminal using the existing manual-validation launch
  shape.
- [ ] Use the original issue prompt or another Codex interaction that produces
  sustained active output and scrollback growth without rewriting the line that
  will be selected.
- [ ] Select stable visible text while output is active or between output bursts.
- [ ] Continue active output so viewport/scrollback mapping changes while the
  selected retained line remains retained, unchanged, and at the leased logical
  row.
- [ ] Confirm the highlight remains visible while the selected retained line is
  visible, unchanged, and not moved or reordered.
- [ ] Confirm Ctrl+C or the app's `selected_text()` observation still returns the
  selected payload.
- [ ] Record the selected text, the visual observation, whether tracing was
  enabled, and any trace lines around selection, backend drain, and copy.

## Expected fail-closed checks

- [ ] Mutate the selected retained line content. Expected: visual spans detach;
  copy payload remains available until clear, replacement, or session reset.
- [ ] Move or reorder the selected retained line with insert/delete-line,
  scroll-region, or equivalent row-origin changes. Expected: spans detach
  fail-closed instead of projecting by retained-line ID.
- [ ] Evict scrollback before a surviving selected retained line. Expected: v1
  may detach fail-closed because row-origin changed, even if retained provenance
  for the selected line might theoretically still match.
- [ ] Evict or purge the selected retained line from scrollback. Expected:
  visual spans detach; copy payload remains available.
- [ ] Switch to the alternate buffer and back. Expected: spans detach and do not
  automatically reattach across the buffer boundary.
- [ ] Trigger column reflow or an unproven resize path. Expected: spans detach
  fail-closed.
- [ ] If synchronized-output reproduction is available, release synchronized
  output that does not change selected retained lines. Expected: spans preserve.
- [ ] If synchronized-output reproduction is available, release synchronized
  output that mutates or evicts selected retained lines. Expected: spans detach
  and payload remains.

## Trace review notes

The original failure signature included diagnostic fields such as
`dirty_rows_overlap_selection=false`,
`dirty_rows_proof=unstable-mutation-identity`,
`viewport_mapping_match=false`, and `selection_span_count=0`. Treat those as
diagnostic evidence when present in the active trace build.

After the retained-line provenance implementation, dirty-row ambiguity should
not be the semantic reason for detachment when selected retained-line
`retained_line_id` and `content_generation` still match at
`selection_start.row + row_offset`. If trace output lacks those fields in the
active validation build, record the visual result and copy payload result
instead of treating missing trace fields as a failure.
