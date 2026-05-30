# Flat Ring History Design Plan

## Status

This is the reviewed design and implementation-plan document for the flat-ring
retained-history redesign. It passed independent Codex-agent and Claude review
rounds for architecture and phase governance on 2026-05-29.

The prior primary-backing refactor established cleaner ownership around current
retained history, viewport behavior, selection lifetime, public projection, and
repaint recovery. This plan is the next storage architecture: retained history
should become an immutable, sequence-addressed record stream over one flat ring
per terminal/session, not a deque-shaped object store with different allocation.

## Relation To Existing Plans

This plan is a successor architecture for retained primary history storage. It
does not cancel the already-completed primary-backing and Phase R recovery
artifacts. Instead, it builds on their separated domains and replaces the
remaining deque-era retained-history substrate.

Existing recovery policy remains in force: recovered primary repaint rows are
accepted only through the normal retained-history append path and carry explicit
recovered provenance.

## Intent

Replace retained primary history with a terminal-aware, flat-ring-native history
log whose persistent references are absolute byte sequences, row sequences,
epochs, and generations.

The design should reduce sources of truth. It should not carry over the current
model's eviction-time repair machinery, retained-row pointer exposure,
scrollback hyperlink refcounts, or in-place retained-row resize/reflow.

## Aims And Objectives

1. Make one flat ring per terminal/session the source of truth for retained
   primary history records.
2. Store retained rows as immutable, self-contained records.
3. Keep active grid state mutable and separate from retained history.
4. Make selection, viewport, public projection, recovery provenance, hyperlinks,
   and retained-row lookup resolve through live ring bounds and record
   validation.
5. Treat indexes and visual layouts as disposable caches, not ownership state.
6. Make reclamation natural: advancing the live ring window makes old references
   stale without per-row cleanup callbacks.
7. Keep repaint recovery as a policy that appends recovered records through the
   normal retained-history record producer.
8. Delete each obsolete deque-era helper in the same phase that removes its last
   correctness use.

## Non-Goals

1. Do not ring-recordize the active grid in the first redesign. The active grid
   remains mutable terminal working state.
2. Do not keep a permanent deque/ring dual-write path.
3. Do not preserve eviction deltas, hyperlink refcounts, or retained-line scans
   as correctness mechanisms.
4. Do not mutate retained rows in place for resize/reflow.
5. Do not use render snapshots, public projections, or repaint recovery as
   storage evidence.
6. Do not expose pointers or references into retained ring storage outside a
   bounded read scope.
7. Do not leave a late cleanup phase responsible for orphans created by earlier
   migration phases.

## Design Principles

### Ring Records Are The Source Of Truth

Retained history is an append-only stream of immutable records inside one flat
ring. A retained row record contains enough information to materialize that row
without consulting mutable retained-history side tables.

Record references are value handles:

```cpp
struct Terminal_history_ref
{
    std::uint64_t epoch;
    std::uint64_t byte_sequence;
    std::uint64_t row_sequence;
    std::uint32_t record_bytes;
    std::uint64_t content_generation;
};
```

`byte_sequence` is an absolute monotonic byte position in the history log.
Physical memory address is derived from `byte_sequence % ring_capacity`; it is
never the identity of a retained record.

A reference resolves only when the absolute byte range is inside the current
live ring window and the record header still matches the expected sequence, byte
count, generation, kind, and epoch.

### Reclamation Is Bounds Advancement

Overwrite is not a semantic event. Reclaiming old history advances the absolute
tail sequence to a record boundary. Any previous handle that now falls outside
`[oldest_live_byte_sequence, head_byte_sequence)` fails validation. Selection,
viewport, projection, provenance, and lookup state must be written so this
failure is sufficient.

### Active Rows Are Sealed Into Records

The active grid remains the mutable terminal working buffer. A row becomes
retained history only when terminal semantics move it out of the active primary
grid. At that boundary, a single retained-history record producer seals the row
into an immutable record:

1. copies canonical cell/text content;
2. snapshots style references or row-local style data;
3. snapshots row-local hyperlink metadata;
4. assigns the next row sequence;
5. records terminal-storage or recovered provenance;
6. commits the record atomically to the history ring.

Normal scrollout and accepted repaint recovery use this same producer.

### Caches Are Disposable

The row ordinal directory, retained-line lookup, visual-row layout, hyperlink
lookup, and checkpoint directory are caches. They may be kept for performance,
but they must be rebuildable from live records or safely discarded. Cache
entries are never authoritative unless their referenced record validates against
current live bounds.

Checkpoint records may exist in the ring as rebuild accelerators. They do not
define row existence; live row records do.

### Rows Are Self-Contained

A retained row record should carry:

1. record framing: magic, version, kind, record bytes, epoch, absolute byte
   sequence, row sequence;
2. row identity: row sequence, content generation, optional retained-line
   compatibility identity during migration;
3. provenance: terminal storage, recovered primary repaint, and any future
   recovery source flags;
4. canonical content: cell runs, text clusters, display widths, wrap markers;
5. row-local hyperlink metadata needed to materialize clickable cells;
6. row-local style data, or style ids that point into immutable session-lifetime
   or versioned append-only style state whose lifetime is explicitly decoupled
   from retained-row reclamation;
7. footer and previous-row sequence data sufficient for reverse traversal.

### Anchors Are Sequence Handles

Selection anchors, detached viewport anchors, public projection anchors, and
retained-history test handles should use history handles rather than deque
indexes or raw row pointers. Staleness is detected by resolving the handle.
Consumers then apply explicit policy: exact, generation changed, geometry
changed, stale and clamped, stale and payload-only, or stale and cleared.

### Resize Is Projection

Retained records store canonical logical content and source metadata. Current
screen width produces a visual-row layout cache. Resize invalidates visual
layout and geometry generations; it does not rewrite retained records.

### Recovery Is Normal Append

Primary repaint recovery remains a policy layer. Accepted recovery rows become
ordinary retained row records with recovered provenance. Recovery does not
mutate ring internals, does not bypass record validation, and does not become a
storage correctness proof.

## Invariants

1. Retained records are immutable after commit.
2. The ring has one writer for retained-history publication.
3. A record is visible only after its full payload and footer are written and
   `head_byte_sequence` is published.
4. `oldest_live_byte_sequence` and `head_byte_sequence` are monotonic within an
   epoch.
5. Tail advancement lands only on a record boundary.
6. `row_sequence` is monotonic and is never reused within an epoch.
7. Physical offsets are never used as persistent identity.
8. Every persistent retained-history reference validates epoch, absolute byte
   range, kind, row sequence, byte count, and content generation before use.
9. No correctness-critical state requires a callback before a row is reclaimed.
10. Hyperlink metadata needed to materialize a retained row is row-local by
    default. Any future non-row-local hyperlink catalog must be immutable,
    sequence-addressed, bounded by ring liveness or session lifetime, and
    validated like every other retained-history reference.
11. Style metadata needed to materialize a retained row is row-local or points
    into immutable session-lifetime or versioned append-only style state. It
    must not require retained-row pre-reclaim cleanup.
12. Readers must observe a published `head_byte_sequence` before reading record
    bytes from that published range.
13. Resize changes geometry state and disposable visual caches, not retained
    row records.
14. Recovery rows enter the retained-history log through the same producer as
    normal scrollout rows.
15. Alternate active-grid rows do not become retained primary history.
16. In-memory indexes and layout structures are caches; dropping them cannot
    lose retained-history content.

## Content Generation Policy

`content_generation` is fixed for a committed retained record. Records are
immutable, so generation never changes in place. During migration it preserves
the current retained-line lease contract and provides an extra validation field
for handles derived from existing `retained_line_id` / `content_generation`
surfaces.

If a later operation needs to represent different retained content, it appends a
new record or advances epoch according to the owning policy. Resize alone never
changes `content_generation`; it changes geometry/layout generation.

Recovered rows receive their own committed record and content generation through
the normal retained-history producer.

## Epoch Policy

`ring_epoch` advances when the meaning of the retained-history sequence space is
reset. Initial policy:

1. explicit retained-history clear;
2. full terminal reset that clears retained history;
3. destruction/recreation of the terminal history ring;
4. any future operation that deliberately invalidates every retained-history
   handle.

Resize does not advance `ring_epoch`; it advances geometry/layout generation.
Appending and natural reclamation do not advance `ring_epoch`.

## Record Model

The first concrete design target is a byte ring with variable-size records.
The field layout below is illustrative; the implementation plan may refine it
without changing the invariants.

```cpp
enum class Terminal_history_record_kind : std::uint16_t
{
    Row,
    Checkpoint,
    Epoch_marker,
    Padding,
};

struct Terminal_history_record_header
{
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t kind;
    std::uint32_t header_bytes;
    std::uint32_t record_bytes;
    std::uint64_t epoch;
    std::uint64_t byte_sequence;
    std::uint64_t row_sequence;
    std::uint64_t previous_row_byte_sequence;
    std::uint64_t previous_row_sequence;
    std::uint64_t content_generation;
    std::uint32_t flags;
};

struct Terminal_history_record_footer
{
    std::uint64_t byte_sequence;
    std::uint32_t record_bytes;
    std::uint32_t magic;
};
```

Record kinds:

1. `Row`: a self-contained retained row.
2. `Checkpoint`: an optional rebuild accelerator containing row sequence and
   byte-sequence landmarks. A checkpoint is never the authority for row
   existence.
3. `Epoch_marker`: an optional diagnostic/rebuild marker written around epoch
   transitions.
4. `Padding`: optional record used only if an implementation chooses not to
   straddle physical wrap.

The logical record stream is addressed by absolute byte sequences. A backend may
use double mapping, two-span access, or scratch linearization to read/write a
record that crosses physical wrap. The semantic model does not depend on
physical contiguity.

### Ring Backend Dependency

The intended Phase 4A backing primitive is Sintra's direct ring helper exposed by
`C:\plms\bsd_licensed\sintra\include\sintra\rings.h`. That public facade includes
the implementation in `sintra/detail/ipc/rings.h`.

This dependency is the canonical Varinomics/Sintra ringbuffer lineage for
terminal retained-history bytes. Phase 4A should first evaluate whether the
direct Sintra helper can be used as-is behind a terminal-local adapter. If the
IPC-oriented surface brings in irrelevant lifecycle, control-file, process-slot,
or reader-eviction behavior that does not fit terminal retained history, Phase
4A may instead extract a reusable subset or implement a local non-IPC variant
derived from the same concepts. The goal is to avoid divergent, incompatible
ringbuffer implementations across Varinomics repositories, not to force terminal
history to adopt Sintra IPC semantics.

Whichever implementation form Phase 4A selects, it is not a dependency on
Sintra's high-level managed-process IPC, publish/subscribe, RPC, barrier, or
lifecycle APIs. The selected primitive must sit behind a terminal-local adapter
whose contract is the history model in this document: absolute byte sequences,
terminal epochs, record-boundary tail advancement, bounded read scopes, explicit
oversize/stale statuses, and header/payload/footer validation.

Sintra concepts such as process indexes, control-file names, reader eviction,
octile guards, semaphores, and IPC lifecycle must not appear in terminal history
APIs, persistent references, public projection state, selection state, or tests
outside the internal ring/backend tests that validate the adapter.

Phase 4A must account for these Sintra constraints before the ring becomes
authoritative:

1. The direct helper is a single-producer/multiple-consumer ring that uses
   double virtual mapping to make physical wrap linear to readers and writers.
2. Capacity must satisfy Sintra's page/alignment and octile constraints. The
   adapter should use `sintra::aligned_capacity<std::byte>` or an equivalent
   checked configuration path.
3. A single Sintra write must fit within one octile of the configured ring. A
   retained-history record larger than the adapter's supported commit window
   must hard-fail with an explicit oversize status before publication. It must
   not silently truncate, split without a reviewed chunking design, or fall back
   to deque storage.
4. Sintra's `done_writing()` publication and reader snapshot lifetime define the
   backend memory-order and read-scope mechanics. Retained-history visibility is
   still defined by this document's record validation and published
   `head_byte_sequence`.
5. Sintra reader eviction or snapshot failure is a backend condition to translate
   into terminal-local stale/invalid/retry status. It is not a retained-history
   reclamation mechanism and must not replace live-window validation.

Oversize records must hard-fail with an explicit status before the ring becomes
authoritative. The implementation plan may later introduce chunking, but silent
truncation, skipped metadata, or fallback to an old representation are not
allowed.

Partial writes are invisible. The writer builds or reserves a complete record,
writes header, payload, and footer, validates the local framing, and only then
publishes the new `head_byte_sequence`.

The row payload should initially prefer a simple, testable encoding over maximum
compression:

1. compact cell runs or dense stored cells;
2. UTF-8 or UTF-16 text-cluster payload;
3. row-local hyperlink table;
4. row-local style table or stable style ids;
5. source-width and wrap metadata.

The exact encoding is an implementation-plan responsibility and must be
benchmarked before it is treated as final.

## Minimal External State

Correctness state outside the ring should be limited to:

1. `oldest_live_byte_sequence`;
2. `head_byte_sequence`;
3. `ring_epoch`;
4. `next_row_sequence`;
5. latest row byte sequence for reverse traversal;
6. active grid, alternate grid, parser, cursor, current style, and current
   hyperlink state;
7. current viewport and selection intent;
8. publication state for synchronized output.

Everything else must be either encoded in records or treated as a rebuildable
cache.

## Behavior Changes To Call Out

These policies must be settled before their owning phase opens:

1. stale selection handle policy: payload-only, clear, or partial clamp;
2. stale detached viewport policy: clamp to oldest live row, clamp to tail, or
   keep last public payload until release;
3. public projection behavior when a copied projection range no longer resolves
   against live history;
4. whether retained-line compatibility identities survive as public/test
   surface or collapse fully into row sequence handles;
5. whether external style ids are immortal session ids, row-local table entries,
   or a separately versioned append-only style catalog.

Owning phases:

1. stale selection policy: Phase 2A;
2. stale detached viewport policy: Phase 2B;
3. stale public projection policy: Phase 2C;
4. retained-line compatibility identity: Phase 1, with cache replacement in
   Phase 5C if a lookup surface remains;
5. style lifetime policy: Phase 3 producer contract, encoded by Phase 4B;
6. first row encoding choice: Phase 4B for the initial encoding and Phase 8
   for later measured tightening;
7. ring access strategy across physical wrap: Phase 4A.

## Phased Implementation Plan

Each phase must be small enough for one focused implementation context. A phase
may be split, but it must not absorb unrelated later work. Every phase that
orphans deque-era helpers owns their deletion in that same phase.

### Phase 0: Baseline, Invariants, And Evidence Harness

Objective: establish target-contract evidence without changing runtime behavior.

Scope:

1. Add or extend tests that express flat-ring invariants against current storage
   seams where possible.
2. Record baseline behavior for scrollout, clear, eviction, resize, retained
   hyperlinks, selection, viewport anchoring, public projection, and recovered
   provenance.
3. Add disabled or test-only target-contract checks only when they cannot yet
   pass on current storage.

Non-goals:

1. No production storage changes.
2. No new production identity model.
3. No behavior changes.

Likely files/modules:

1. `tests/backend_session/backend_session_tests.cpp`
2. Existing terminal screen model test files, if present.
3. `docs/primary_backing_redesign_plans/primary_backing_failure_ledger.md` if
   new regression/failure evidence is added.

Deletion ownership:

1. Any test-only shim introduced here must name its owning deletion or promotion
   phase in the phase evidence.

Test/gate evidence:

1. Focused backend/session test target.
2. Tests or documented target checks for stale handle validation, row-local
   hyperlink materialization, resize-as-projection, recovered provenance through
   append, and no pointer lifetime dependency.

Review focus:

1. Evidence describes intended invariants, not deque implementation details.
2. No production behavior change is hidden in test setup.

Stop/split triggers:

1. If a test requires changing production behavior, split it into the phase that
   owns that behavior.
2. If recovery behavior becomes ambiguous, stop and record the owning Phase 7
   question.

### Phase 1: History Handle Vocabulary

Objective: introduce one retained-history reference vocabulary without creating
parallel public API.

Scope:

1. Define the internal history handle and resolution status vocabulary around
   epoch, byte sequence placeholder, row sequence, record size, and content
   generation.
2. Back handles from current retained-line identity while ring storage is not
   present.
3. Convert narrow internal proof paths and tests away from raw deque indexes
   where feasible.

Non-goals:

1. No ring backend.
2. No producer rewrite.
3. No selection, viewport, or public projection policy change beyond using the
   new vocabulary internally.

Likely files/modules:

1. `include/vnm_terminal/internal/terminal_screen_model.h`
2. `src/terminal_screen_model.cpp`
3. `include/vnm_terminal/internal/selection_contract.h`
4. Render/projection headers only if they carry retained-line proof types.

Deletion ownership:

1. Delete any newly orphaned local conversion helpers in this phase.
2. Do not leave `_v2`, `_legacy`, or duplicate retained reference types.

Test/gate evidence:

1. Existing retained-line lookup and selection lease tests.
2. New stale-handle status tests backed by current storage.

Review focus:

1. One canonical internal vocabulary.
2. No permanent aliasing between old and new identity shapes.

Stop/split triggers:

1. If converting a consumer changes visible selection/viewport behavior, stop and
   move that consumer to Phase 2A/2B/2C.

### Phase 2A: Selection Handle Resolution

Objective: make selection retained-history attachment resolve through handles
and explicit stale policy.

Scope:

1. Decide and document selection stale policy before production edits.
2. Route retained selection proof/extraction through handle resolution.
3. Preserve finalized payload behavior when visual proof goes stale, according
   to existing selection lifetime policy.

Non-goals:

1. No viewport anchoring migration.
2. No public projection migration.
3. No ring backend.

Likely files/modules:

1. `src/terminal_session.cpp`
2. `src/terminal_screen_model.cpp`
3. `include/vnm_terminal/internal/selection_contract.h`
4. Selection lifetime tests.

Deletion ownership:

1. Delete selection-only eviction-coordinate repair helpers orphaned by this
   phase.
2. Any remaining eviction delta use must name its remaining consumer.

Test/gate evidence:

1. Selection across history/active boundary.
2. Selection after eviction and clear.
3. Selection during synchronized output if already covered.
4. Stale handle resolves to the documented selection policy.

Review focus:

1. Selection no longer depends on scrollback eviction arithmetic for correctness.
2. Finalized copy payload and visual lease semantics remain distinct.

Stop/split triggers:

1. If viewport movement or public projection behavior must change to make tests
   pass, stop and split.

### Phase 2B: Viewport Handle Resolution

Objective: make detached viewport anchoring resolve through handles and explicit
clamp policy.

Scope:

1. Decide and document stale detached viewport policy before code changes.
2. Route live primary detached viewport anchoring through retained-history handle
   resolution.
3. Keep public projection out of scope.

Non-goals:

1. No selection policy changes.
2. No public projection changes.
3. No ring backend.

Likely files/modules:

1. `src/terminal_session.cpp`
2. `src/terminal_screen_model.cpp`
3. Viewport controller/internal viewport state modules, if separate.
4. Backend session viewport tests.

Deletion ownership:

1. Delete viewport-specific eviction arithmetic helpers orphaned by this phase.
2. Remaining eviction delta producers must list remaining consumers.

Test/gate evidence:

1. Detached viewport after append, clear, eviction, scrollback-limit shrink, and
   resize.
2. Tail-following remains unchanged.

Review focus:

1. Detached viewport uses handle validity or documented clamp policy, not raw
   row-count repair.

Stop/split triggers:

1. If public projection bounds or synchronized-output release behavior must
   change, stop and move to Phase 2C.

### Phase 2C: Public Projection Handle Resolution

Objective: make public projection retained-history anchoring use handle
resolution and explicit stale policy.

Scope:

1. Decide public projection stale policy before code changes.
2. Replace retained-row/deque assumptions in projection capture and release
   reconciliation with handle resolution.
3. Preserve hidden-live-output isolation during synchronized output.

Non-goals:

1. No general storage cutover.
2. No selection mutation redesign.
3. No resize projection migration unless a minimal proof field is needed.

Likely files/modules:

1. `include/vnm_terminal/internal/terminal_public_projection.h`
2. Public projection implementation source.
3. `src/terminal_session.cpp`
4. Public projection/backend session tests.

Deletion ownership:

1. Delete old eviction-delta producers if this removes their last consumer.
2. Otherwise record exact remaining consumer and owner phase.

Test/gate evidence:

1. Existing public projection synchronized-output tests.
2. Public scroll during hold.
3. Hidden live scrollback growth does not leak.
4. Stale public anchors fail closed or reconcile by documented policy.

Review focus:

1. Public projection remains a publication layer, not storage.
2. No hidden live history read through stale public anchors.

Stop/split triggers:

1. If projection needs a broader storage read adapter, stop and create a
   separate no-behavior adapter phase.

### Phase 3: Active-To-Retained Record Producer

Objective: create one producer that seals active-grid or recovered rows into
final retained-record values while still targeting current storage.

Scope:

1. Extract a single sealing path for normal scrollout and accepted recovery.
2. Produced value contains canonical content, provenance, row-local hyperlinks,
   chosen style lifetime representation, source width, and wrap metadata.
3. Decide style lifetime policy for produced values.

Non-goals:

1. No byte ring.
2. No authoritative storage cutover.
3. No recovery policy changes beyond shared producer use.

Likely files/modules:

1. `src/terminal_screen_model.cpp`
2. `include/vnm_terminal/internal/terminal_screen_model.h`
3. Recovery proposal/provenance paths.
4. Hyperlink materialization helpers.

Deletion ownership:

1. Delete helper paths made redundant by producer consolidation.
2. Delete any recovery-only append path bypassing the shared producer.

Test/gate evidence:

1. Normal scrollout and recovered rows carry equivalent retained-record fields.
2. Hyperlink/style/provenance data round-trips through current storage
   materialization.
3. Recovery-disabled ordinary behavior unchanged.

Review focus:

1. One producer, no sibling producer fork.
2. Producer output is storage-neutral and ring-ready.

Stop/split triggers:

1. If style policy cannot be decided from existing behavior, stop and document
   owner decision before implementation.

### Phase 4A: Flat Ring Primitive

Objective: implement the terminal-local byte-ring primitive and liveness
semantics in isolation, using Sintra's direct ring helper from
`C:\plms\bsd_licensed\sintra\include\sintra\rings.h`, a reusable subset of that
code, or a local non-IPC variant derived from the same concepts as the backing
ringbuffer. The Phase 4A evidence must document which form was selected and why.

Scope:

1. Absolute byte sequence model.
2. Physical wrap access strategy chosen before phase opens.
3. Record reservation, commit publication, tail advancement, padding/wrap
   behavior, oversize hard failure, partial-write invisibility.
4. Terminal-local adapter boundary over Sintra's direct ring helper, including
   capacity alignment, one-octile write limit handling, publication mapping, and
   snapshot failure translation.
5. If the direct helper is not used as-is, document the concrete incompatibility
   and the exact subset or local variant chosen.

Non-goals:

1. No terminal row codec.
2. No production retained-history writes.
3. No production dual-write mirror.

Likely files/modules:

1. New internal ring/backend module under `include/vnm_terminal/internal/` and
   `src/`.
2. Sintra direct ring dependency through `sintra/rings.h`, or a local
   terminal-owned subset/variant derived from that code when the direct helper is
   a poor fit.
3. No dependency on `sintra/sintra.h` or high-level Sintra IPC APIs.
4. Unit tests for the primitive and adapter.

Deletion ownership:

1. Delete any experimental primitive variants not selected before phase close.
2. If Phase 4A extracts or implements a local non-IPC variant, document the
   ownership boundary and remove any unused direct-Sintra adapter scaffolding in
   the same phase.

Test/gate evidence:

1. Wrap traversal.
2. Tail advancement to record boundaries.
3. Oversize failure.
4. Partial records invisible.
5. Rebuild after cache drop from live range.
6. Sintra capacity alignment and one-octile write-limit boundary behavior.
7. Snapshot/read-scope failure translation into terminal-local status.

Review focus:

1. Physical offsets never become identity.
2. Sintra is used only behind the terminal-local adapter. `Ring_R`, `Ring_W`,
   process-slot, control-file, reader-eviction, semaphore, publish/subscribe,
   RPC, and lifecycle concepts must not leak into terminal history references or
   non-backend APIs.
3. The adapter translates Sintra constraints into explicit terminal statuses
   instead of weakening retained-history invariants.
4. If a subset or local variant is chosen, it should stay recognizably aligned
   with the Sintra ring concepts so future ringbuffer fixes can be shared or
   ported intentionally.

Stop/split triggers:

1. If double mapping pulls in platform-specific complexity, split backend
   strategy from semantic primitive.
2. If Sintra's one-octile write limit conflicts with the required retained-row
   record size, split record chunking or row-size-limit policy into a reviewed
   plan amendment before implementation continues.
3. If Sintra reader eviction semantics would be needed for correctness, stop and
   redesign the adapter boundary; retained-history reclamation must remain live
   bounds advancement plus record validation.
4. If using the direct Sintra helper would import more IPC machinery than the
   terminal-local contract can justify, select and document a subset or local
   non-IPC variant instead of forcing the dependency.

### Phase 4B: Row Record Codec

Objective: encode/decode retained row records against the ring framing.

Scope:

1. Encode/decode content, wide cells, text clusters, row-local hyperlinks, style
   representation, provenance, source width, and wrap metadata.
2. Bounded read scopes only; materialized data must be owned by caller/snapshot.

Non-goals:

1. No production storage authority.
2. No traversal directory replacement.
3. No performance tuning beyond avoiding pathological obvious waste.

Likely files/modules:

1. Record codec module.
2. `terminal_screen_model` retained row materialization boundary.
3. Focused codec tests.

Deletion ownership:

1. Delete obsolete test encoders/temporary format helpers introduced during the
   phase.
2. If style policy introduces temporary adapters, name their deletion owner.

Test/gate evidence:

1. Dense/blank rows.
2. Wide spans and continuations.
3. Combining/cluster text.
4. Styles.
5. Hyperlinks.
6. Recovered provenance.
7. Header/footer validation failure cases.

Review focus:

1. Records are self-contained under the selected style policy.
2. Decode never returns raw ring pointers outside a bounded scope.

Stop/split triggers:

1. If both dense and run encodings are implemented, stop and keep one; defer
   alternative to performance phase.

### Phase 4C: Traversal And Checkpoint Rebuild

Objective: provide live-record traversal and rebuildable lookup acceleration.

Scope:

1. Forward traversal by record length.
2. Backward traversal by previous-row sequence/byte sequence.
3. Optional checkpoint records and in-memory directories as caches only.
4. Cache drop/rebuild tests.

Non-goals:

1. No production retained-line lookup migration.
2. No authoritative storage cutover.
3. No selection/viewport policy changes.

Likely files/modules:

1. Ring traversal module.
2. Checkpoint/cache module if introduced.
3. Primitive/codec tests.

Deletion ownership:

1. Delete any authoritative checkpoint assumptions or temporary direct-index maps.

Test/gate evidence:

1. Forward/backward traversal across wrap.
2. Missing/stale checkpoint rebuild.
3. Dropping directories loses no content.
4. Header mismatch invalidates cache entries.

Review focus:

1. Checkpoints accelerate; row records define existence.
2. All cache hits validate live record identity.

Stop/split triggers:

1. If checkpoint format becomes broad, split checkpoint writing from cache
   rebuild.

### Phase 4D: Ring Materialization Parity Harness

Objective: compare ring materialization against current retained-history
behavior without creating a production mirror.

Scope:

1. Test-only or bounded non-production harness.
2. Feed producer values into ring codec and compare materialized output with
   current retained storage output.

Non-goals:

1. No production dual writes.
2. No new public API.
3. No cutover.

Likely files/modules:

1. Tests and test-only support.
2. Producer/codec integration tests.

Deletion ownership:

1. Phase 6B owns deletion of bounded mirror fixtures/harness parts that become
   obsolete at cutover.

Test/gate evidence:

1. Parity for text, styles, hyperlinks, provenance, blank rows, wide spans,
   recovery rows, stale handles.

Review focus:

1. Harness cannot ship as a permanent fallback.
2. Test-only boundaries are explicit.

Stop/split triggers:

1. If parity requires production mirror state, stop and redesign as isolated
   fixtures.

### Phase 5A: Resize As Visual Projection

Objective: make retained-history resize behavior projection/layout-generation
based before immutable ring cutover.

Scope:

1. Replace retained-row mutation semantics with visual layout cache invalidation
   and geometry generation.
2. Selection visual leases use geometry generation plus handle resolution.
3. Preserve documented user-facing resize behavior or explicitly classify any
   behavior change.

Non-goals:

1. No ring authority switch.
2. No hyperlink or lookup migration unless directly affected.

Likely files/modules:

1. `src/terminal_screen_model.cpp`
2. Render snapshot materialization path.
3. Selection visual lease handling.
4. Resize tests.

Deletion ownership:

1. Delete old in-place retained resize helpers orphaned by this phase.

Test/gate evidence:

1. Resize height/width scenarios.
2. Detached scrollback.
3. Selection after resize.
4. Wide spans/trailing blanks.
5. No retained content mutation required.

Review focus:

1. Immutable retained records will not need a resize escape hatch.
2. Geometry generation and content generation stay separate.

Stop/split triggers:

1. If resize behavior changes broadly, split characterization from behavior
   migration.

### Phase 5B: Retained Hyperlink Metadata Authority

Objective: remove retained-history correctness dependence on scrollback-wide
hyperlink refcounts.

Scope:

1. Retained rows materialize hyperlink metadata from row-local retained data.
2. Remaining hyperlink maps are active/parser state or explicitly rebuildable
   caches.

Non-goals:

1. No general storage cutover.
2. No style-policy migration unless tied to record codec.

Likely files/modules:

1. `src/terminal_screen_model.cpp`
2. Hyperlink metadata materialization helpers.
3. Render snapshot hyperlink tests.

Deletion ownership:

1. Delete scrollback hyperlink refcount and identity-key side maps if this
   removes their last retained-history use.
2. Any remaining map must have explicit active/parser/cache ownership.

Test/gate evidence:

1. Hyperlinks in retained scrollback after active hyperlinks are gone.
2. Hyperlink-heavy output.
3. Eviction/clear requires no retained hyperlink cleanup for correctness.

Review focus:

1. No pre-reclaim hyperlink cleanup remains necessary.
2. Row-local retained metadata is authoritative.

Stop/split triggers:

1. If active-grid hyperlink lifetime becomes entangled, split active-grid
   cleanup from retained-history authority.

### Phase 5C: Retained Lookup Cache Replacement

Objective: replace retained-line scans and ordinal lookup correctness with
rebuildable caches over validated handles.

Scope:

1. Retained-line lookup is a cache over live retained-history handles.
2. Row ordinal lookup can rebuild from retained rows/checkpoints.
3. Current-storage-backed implementation is allowed until cutover.

Non-goals:

1. No ring authority switch.
2. No public retained-line contract removal unless Phase 1 decided it.

Likely files/modules:

1. `src/terminal_screen_model.cpp`
2. Retained lookup/test provenance helpers.
3. Selection/public projection lookup consumers.

Deletion ownership:

1. Delete scan-only correctness helpers orphaned by this migration.

Test/gate evidence:

1. Retained-line exact, generation mismatch, nearest successor/predecessor.
2. Cache drop/rebuild.
3. Stale handle validation.

Review focus:

1. Caches are not source of truth.
2. No deque scan fallback remains needed for cutover.

Stop/split triggers:

1. If public/test retained-line identity policy is unresolved, stop and return
   to Phase 1 decision.

### Phase 6A: Ring Cutover Readiness Gate

Objective: prove all prerequisites are present before making the ring
authoritative.

Scope:

1. Run integrated gates over primitive, codec, traversal, producer, resize
   projection, row-local hyperlinks, lookup caches, and parity harness.
2. Record baseline git revision and worktree state.

Non-goals:

1. No production storage switch.
2. No cleanup ownership except deleting readiness-only artifacts if added.

Likely files/modules:

1. Test runner/docs evidence only.
2. No production files unless a gate-only assertion is needed.

Deletion ownership:

1. Any readiness-only diagnostics added here are deleted before phase close or
   assigned to Phase 6B if needed for cutover review.

Test/gate evidence:

1. Phase 4D parity green.
2. Grep confirms no production caller depends on pointer-returning retained-row
   APIs, resize mutation helpers, scrollback hyperlink refcounts, or scan-only
   lookup.
3. Focused backend/session and storage tests.

Review focus:

1. Cutover is small and mechanical after this phase.
2. No unresolved owning-phase policy remains.

Stop/split triggers:

1. Any failed prerequisite sends work back to its owning phase; do not expand
   Phase 6A.

### Phase 6B: Authoritative Ring Cutover

Objective: make ring records the authoritative retained-history storage.

Scope:

1. Replace retained deque storage with ring-backed retained-history owner.
2. Remove pointer-returning retained-row APIs.
3. Remove Phase 4D bounded mirror fixtures.
4. Ensure scrollout and recovery append through shared producer.

Non-goals:

1. No new feature behavior.
2. No performance encoding changes.
3. No fallback to deque storage.

Likely files/modules:

1. `include/vnm_terminal/internal/terminal_screen_model.h`
2. `src/terminal_screen_model.cpp`
3. Ring owner/codec/traversal modules.
4. Tests that referenced old retained storage.

Deletion ownership:

1. Delete `scrollback_row_t`, retained deque storage, retained-row pointer APIs,
   and bounded parity mirror fixtures in this phase.
2. Delete any helpers orphaned by removing those APIs.

Test/gate evidence:

1. Full focused terminal/backend session gate.
2. Storage unit tests.
3. Grep for old storage symbols and fallback names.
4. Manual or automated resize/selection/public projection scenarios if already
   established.

Review focus:

1. No permanent mirror/fallback path.
2. No dangling pointer/view exposure.
3. Natural reclamation semantics remain intact.

Stop/split triggers:

1. If cutover touches unrelated selection/public projection policy, stop and
   move that behavior to the owning phase.

### Phase 7: Recovery Shared-Producer Verification

Objective: harden the recovery policy boundary on ring-native storage.

Scope:

1. Verify recovered rows carry recovered provenance in ring records.
2. Verify recovery cannot bypass the retained-history producer.
3. Verify recovery-disabled normal history behavior.

Non-goals:

1. No recovery heuristic redesign.
2. No storage format changes unless a missing provenance field violates
   invariants.

Likely files/modules:

1. Recovery proposal paths in `terminal_screen_model`.
2. Phase R recovery tests/docs if needed.

Deletion ownership:

1. Delete any recovery-only append bypass found in this phase.

Test/gate evidence:

1. Recovery-enabled recovered provenance.
2. Recovery-disabled normal scrollout.
3. Resize-adjacent recovery scenarios covered by existing Phase R tests where
   applicable.

Review focus:

1. Recovery is policy over storage, not storage evidence.
2. Shared producer remains single source.

Stop/split triggers:

1. If recovery policy itself must change, split into a Phase R-owned plan
   amendment.

### Phase 8: Encoding, Performance, And Memory Tightening

Objective: tune representation only after semantic migration is complete.

Scope:

1. Benchmark append, materialization, traversal, resize projection, selection
   extraction, cache rebuild, hyperlink-heavy output.
2. Decide dense versus run-length or other compact encoding using measurements.
3. Document ring size and row-size limits.

Non-goals:

1. No semantic storage model changes.
2. No fallback to old representation.

Likely files/modules:

1. Benchmarks.
2. Codec implementation.
3. Docs for limits/diagnostics.

Deletion ownership:

1. Delete superseded experimental encoding paths in the same phase.
2. Do not leave both dense and compact encoders unless one is strictly test-only
   and named for deletion before close.

Test/gate evidence:

1. Benchmark report.
2. Regression tests green.
3. Oversize behavior documented and tested.

Review focus:

1. Optimization does not weaken invariants.
2. Pathological rows hard-fail or follow documented chunking.

Stop/split triggers:

1. If chunking is needed, split it into a separate behavior/format phase.

### Phase 9: Scaffold Verification Sweep

Objective: verify no migration scaffold remains; this is not cleanup ownership.

Scope:

1. Grep/static audit for deque/refcount/eviction-delta/resize-mutation helpers.
2. Audit for `_legacy`, `_v2`, permanent mirror, compatibility fallback.
3. Record final evidence.

Non-goals:

1. No first-time deletion of earlier orphans.
2. No behavior changes.

Likely files/modules:

1. Evidence document or phase notes.
2. No production edits expected unless an earlier phase missed a deletion, in
   which case return to that phase.

Deletion ownership:

1. None. If this phase finds orphaned production scaffold, the plan is not ready
   to close; assign it back to the owning migration phase.

Test/gate evidence:

1. Grep/static audit.
2. Focused test gate.
3. Recorded git revision and worktree status.

Review focus:

1. Verification-only role is preserved.
2. No hidden permanent dual path remains.

Stop/split triggers:

1. Any actual cleanup need blocks this phase and returns to the owning phase.

## Implementation Review Protocol

Each implementation phase must include a short gate table before review: scope,
behavior axis, recovery state, evidence, baseline outcome, exit predicate,
deletion ownership, rollback mechanism, and split triggers.

Reviewers should classify findings as blockers only when they show an invariant
contradiction, unsafe source of truth, missing deletion owner, phase too large
for one context, permanent dual path, or unstated behavior change. Later-phase
local helper shape is not a current blocker unless the current phase commits to
the wrong contract.

No phase may close with a production fallback, mirror, `_legacy` path, or
orphaned helper unless the phase document names the exact successor owner and the
reason the current phase cannot delete it. Verification phases cannot own
first-time cleanup.
