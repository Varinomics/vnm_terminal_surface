# In-Process Magic Ring Buffer for Terminal History — Design Note

Status: **proposal / design note.** This describes refactoring the storage backend of
`Terminal_history_ring` from a `std::vector<std::byte>` split-on-wrap buffer to an
**in-process, anonymous-backed "magic" (double-mapped) ring**, modeled on the
double-mapping technique in Sintra's `Ring_data` with all inter-process machinery
removed. It is a plan, not an implementation. It is independent of
[gpu_atlas_renderer_plan.md](gpu_atlas_renderer_plan.md) and
[terminal_text_representation_plan.md](terminal_text_representation_plan.md); it
touches only the scrollback history ring and its two copy boundaries.

## Motivation

Terminal scrollback is, by nature, a **fixed-capacity circular buffer**: the host
configures a history size, and once full the oldest content is evicted as new rows
arrive. `Terminal_history_ring` already models that — fixed `m_capacity_bytes`,
monotonic `m_head_byte_sequence`, evict-oldest via tail advance. The question this
note addresses is the *storage backing* and the *copy boundaries*, not the ring
semantics.

The current backing is `std::vector<std::byte> m_storage`, and because a record can
straddle the buffer end, every cross-boundary access is handled by **splitting into
two `memcpy`s** (`src/terminal_history_ring.cpp` write at ~742–744, read at
~758–760). Worse, that split is wrapped in *owning copies* on both hot boundaries:

- **Read** — `read_record()` returns a `Terminal_history_ring_read_scope` that owns a
  freshly allocated `std::vector<std::byte>` reassembled by `copy_record_bytes()`.
  Consumers (`terminal_history_row_traversal.cpp:123/362/392`,
  `terminal_history_row_record_codec.cpp:807+`, `terminal_screen_model.cpp:4002`)
  then decode `read_scope.payload()`. Every record read = one heap allocation + one
  full record copy.
- **Write** — `reserve_record()` returns a reservation that owns a staging
  `std::vector<std::byte>`; the codec encodes into `reservation.payload()`
  (`terminal_history_row_record_codec.cpp:755–793`); `commit()` then copies that
  staging buffer into the ring via `write_record_bytes()` (the split-on-wrap copy).
  Every record write = one staging allocation + one record copy.

A **magic ring** maps the same `cap` bytes of memory **twice, contiguously** in the
virtual address space, so the byte range `[s % cap, s % cap + n)` is *always*
contiguous for `n ≤ cap` even when it crosses the logical end. That collapses the
two-`memcpy` seam to linear addressing and — more importantly — lets both boundaries
become **zero-copy**:

- `read_record()` returns spans pointing **directly into the ring** → the codec
  decodes in place; `copy_record_bytes()` and the read-scope's owned vector disappear.
- `reserve_record()` returns a span pointing **directly into the ring** → the codec
  encodes in place; `commit()` only validates + publishes; the staging vector and
  `write_record_bytes()` copy disappear.

This is the "handle certain situations more trivially" property the history ring was
originally meant to have (modeled on Sintra's rings, minus IPC), and it removes the
three `std::vector<std::byte>` instances on the hot path (`m_storage`, the reservation
staging buffer, the read-scope copy).

## Scope and non-goals

**In scope:** a single-process, single-thread (GUI-owned) magic ring buffer used as
the storage backend of `Terminal_history_ring`, on the product's two targets (Windows
x64, Linux x86_64), with byte-identical record framing and ring semantics, and
zero-copy read/write boundaries.

**Explicitly NOT taken from Sintra** (its `Ring`/`Control` layer, everything past
`Ring_data`): inter-process shared memory, the SPMC protocol, per-reader octile read
guards, `read_access` atomics, reader slots, semaphores, slow-reader eviction, the
control file, ABI fingerprinting, the backing *file* (Sintra's `Ring_data` is
file-backed for cross-process sharing; we use anonymous memory). Only the
**double-mapping technique** in `Ring_data::attach()` is relevant, adapted to an
anonymous, single-process mapping.

**Behavior parity is a hard requirement:** record framing (24-byte `VHR1` header +
payload + 16-byte `VHF1` footer), sequencing (`head`/`oldest` byte sequences,
position = `seq % cap`), the descriptor index (`m_records`), eviction
(`make_room_for` / `discard_oldest_records`), status codes, and the codec output must
be unchanged. Existing `tests/history_ring` and `tests/history_row_record_codec` are
the parity oracle.

## What Sintra's `Ring_data` does, and what we take

Sintra's `Ring_data` (rings.h ~664–896) guarantees "the same file contents appear
twice back-to-back in the virtual address space so wrap-around is linear." Extraction:

| Sintra `Ring_data` element | Here |
| --- | --- |
| Double-map the same region twice contiguously | **take** (the whole point) |
| Backed by a named *file* on disk | **drop** → anonymous (memfd / pagefile section) |
| `num_elements * sizeof(T)` region, granularity-aligned | **take** (byte region, granularity-aligned) |
| Windows reserve→free→remap; POSIX `mmap(PROT_NONE)` + `MAP_FIXED` ×2 | **take** (adapt to anonymous) |
| `Control`, octiles, readers, semaphores, eviction, ABI fingerprint | **drop** (no IPC) |
| `num_elements % 8 == 0`, trivially-copyable `T` | **drop** (those are `Ring`/guard constraints, not `Ring_data`) |

## The technique: anonymous double mapping

Reserve `2 * cap` of contiguous address space, then map one anonymous `cap`-byte
object into both halves so they alias. The buffer's logical base is the start of the
first half; reads/writes of up to `cap` bytes from any offset stay linear because the
second half mirrors the first.

Illustrative only (the crux of the design; not code to be committed here):

**Linux / POSIX**
```
fd   = memfd_create("vnm_history_ring", MFD_CLOEXEC);   // fallback: shm_open+unlink, or /dev/shm tmpfile
ftruncate(fd, cap);                                     // cap == multiple of page size
base = mmap(NULL, 2*cap, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);   // reserve 2x
mmap(base,       cap, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_FIXED, fd, 0); // half 0
mmap(base + cap, cap, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_FIXED, fd, 0); // half 1 (mirror)
// MAP_SHARED is required so both views alias the same physical pages (MAP_PRIVATE would COW).
// MADV_DONTDUMP optional. Teardown: munmap(base, 2*cap); close(fd). On any partial failure, munmap the 2x span.
```

**Windows (Win10 1803+ placeholder API)**
```
section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, hi, lo, NULL); // pagefile-backed (anonymous)
base    = VirtualAlloc2(NULL, NULL, 2*cap, MEM_RESERVE|MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
VirtualFree(base, cap, MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER);                            // split placeholder
MapViewOfFile3(section, NULL, base,       0, cap, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
MapViewOfFile3(section, NULL, base + cap, 0, cap, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
// cap and base must be allocation-granularity aligned (64 KiB). Teardown: UnmapViewOfFile x2; CloseHandle(section).
```

`VirtualAlloc2`/`MapViewOfFile3` require Windows 10 1803+ (in `onecore`/`kernelbase`,
declared in `<memoryapi.h>`). The product targets modern Windows x64, so this is the
recommended path; the legacy Sintra-style `VirtualAlloc(2x)`→`VirtualFree`→
`MapViewOfFileEx` ×2 retry trick is documented only as a fallback for hosts predating
1803 (it is racy — needs the retry loop — and not recommended unless a real target
requires it).

**Granularity / alignment.** The mapped region size must be a multiple of the OS
mapping granularity — page size on POSIX (typically 4 KiB), **allocation granularity
on Windows (64 KiB)**. `terminal_history_ring_aligned_capacity()` tightens to round
the requested capacity up to that granularity (it already aligns to a smaller value
today, so the alignment concept exists). Transient mapping races (multiple threads,
address-space contention) get a small bounded retry loop, as Sintra does.

## What changes in `Terminal_history_ring`

Only the storage member and the two copy boundaries change; the public ring API
*shapes* stay, but the read-scope and reservation become **non-owning views**.

- `std::vector<std::byte> m_storage` → a `Magic_ring_buffer` RAII type owning the
  double mapping (raw `std::byte* base()`, `size()`, the platform handles). Single
  responsibility: "the same `cap` bytes appear twice, contiguously." It knows nothing
  about records.
- `reserve_record(payload)` → `reservation.payload()` is a `std::span<std::byte>`
  **into the ring** at the reserved offset (linear via the mirror). The codec encodes
  in place. `commit()` validates framing + advances `head` + updates `m_records`; it
  no longer copies. The reservation drops its `std::vector<std::byte> m_bytes`.
- `read_record(seq)` → `read_scope.record()`/`payload()` are `std::span<const
  std::byte>` **into the ring** (contiguous via the mirror). `copy_record_bytes()` and
  the read-scope's owned `std::vector<std::byte> m_bytes` are removed.
- `write_record_bytes()` (the split-on-wrap writer) is removed once writes are
  zero-copy; until then it becomes a single `memcpy` (no split branch) because the
  region is contiguous.
- Unchanged: `m_records` (the descriptor `std::deque`), `m_head_byte_sequence` /
  `m_oldest_live_byte_sequence` atomics, `make_room_for`, `discard_oldest_records`,
  `rebuild_record_index`, `live_record_descriptors`, all status enums, the
  `max_record_bytes = cap/8` cap (see Open Questions), and the codec.

## Lifetime and threading contract (the correctness hinge)

The magic ring trades *owned copies* for *views into live memory*, so the lifetime and
threading model must be stated and verified explicitly.

- **Assumed model: single-writer, single-thread, GUI-owned.** Per
  [architecture.md](architecture.md) "Threading And Ownership," the GUI thread owns the
  screen model and history; backend worker threads do not touch model/render state; and
  the render path consumes the **immutable published snapshot**, not the live ring. If
  that holds, there are no concurrent record reads, so none of Sintra's reader-guard
  machinery is needed.
- **Verification gate (must pass before zero-copy read lands):** confirm that no thread
  other than the owner ever *dereferences record bytes*. The ring exposes
  `std::atomic` `head`/`oldest` sequences and a `Terminal_history_ring_backend_snapshot_status`
  (`OK`/`STALE`/`RETRY`) — confirm those cross-thread paths read only the **sequence
  range** (for a best-effort liveness/snapshot check), never record payloads. If any
  path reads payload bytes off-thread, that specific path must keep a copy (as today)
  or the ring must add guards; the zero-copy change applies only to the owner-thread
  read/write sites.
- **Span validity contract:** a `read_scope` span (and a `reservation.payload()` span)
  is valid only until the next **mutating** ring operation (`commit`, `discard`, or any
  eviction) that could overwrite or evict that region. Consumers must read/decode
  before the next mutation, or copy explicitly to retain. The current owned-copy
  read-scope is retain-safe by construction; the zero-copy version makes this a
  documented contract. Verified consumers (traversal, codec, buffer-transition copy in
  `terminal_screen_model.cpp:4002`) decode immediately into structured data and do not
  stash the raw span across a mutation — this must be re-checked per site during
  migration. In debug builds, poison/guard the reserved-but-uncommitted and
  just-evicted regions to catch contract violations.

## Failure handling

Anonymous double-mapping can fail (address-space pressure, OS limits, a host predating
the Windows placeholder API, `memfd` unavailable on a very old kernel). Policy:

- The magic mapping is the **single** backend; there is no second full ring
  implementation kept in parallel (that would be a permanent parallel path — change
  governance Rule 1). On unrecoverable mapping failure after the bounded retry, the
  ring constructs into a **non-OK status** and operates with zero usable scrollback
  capacity (history disabled, the visible screen still works), rather than silently
  degrading correctness. This mirrors the existing `INVALID_CAPACITY` posture.
- All error paths must release every partial mapping (unmap the 2× reservation / both
  views, close the fd/section) — leak-free, matching Sintra's cleanup discipline.
- `memfd` fallback chain on Linux (`memfd_create` → `shm_open`+`unlink` → tmpfile under
  `/dev/shm`) is an implementation detail; the abstraction reports success/failure.

If a real deployment target turns out to lack the mapping primitives, re-introducing a
non-mapped buffer becomes a *platform-support* decision (documented, owner-approved),
not a default convenience fallback — same reasoning as the renderer plan.

## Capacity and configuration

- Capacity stays **byte-valued**; `aligned_capacity` rounds up to OS granularity
  (page / 64 KiB). Tests that assert an exact capacity must accept the granularity-
  rounded value.
- The host's natural unit is **rows** (Windows Terminal exposes a history-line count).
  Mapping "N rows" → byte capacity (N × estimated bytes/row, granularity-rounded) is a
  higher-level concern and **out of scope** for the storage swap, but the magic ring
  makes a future fixed "N rows" model cleaner; noted as a follow-on, not a deliverable.
- `max_record_bytes = cap/8` is Sintra's octile bound, which existed to bound a writer
  to a single reader-guard octile. **Without reader guards that bound is no longer
  required** — a single record may span the wrap with no special handling. Keep `cap/8`
  for parity initially; relaxing it (e.g. to `cap/2` minus overhead) is a separate,
  test-gated tweak (see Open Questions).

## Testing and validation

Parity first, then the new primitive, then the zero-copy boundaries.

- **Parity oracle:** `tests/history_ring`, `tests/history_row_record_codec`, plus the
  history-traversal tests stay green unchanged through Stage 2; record bytes produced
  for given inputs must be byte-identical to the current ring.
- **Magic-buffer unit tests** (new, `tests/magic_ring_buffer` or folded into
  `history_ring`):
  - *Aliasing:* a write through `base[i]` is visible at `base[i + cap]` and vice-versa.
  - *Linear wrap:* write a pattern spanning `[cap-k, cap+m)`; assert the contiguous
    read at `base + (cap-k)` equals the logically-wrapped bytes, and equals what the
    old split-copy produced.
  - *Granularity:* `aligned_capacity` rounds to page/64 KiB; a non-granularity request
    is rounded (not rejected) and the mapping succeeds at the rounded size.
  - *Lifecycle:* construct/destruct N times with no leaked mappings/handles (ASAN +
    `/proc/self/maps` count on Linux; handle count on Windows).
  - *Failure:* an induced mapping failure yields a non-OK ring and leaks nothing.
- **Zero-copy read tests:** `read_scope.payload().data()` lies within `[base, base+cap)`
  (or its mirror); decoding the in-place span yields the same row record as the
  owned-copy path did; a record crossing the boundary decodes correctly.
- **Zero-copy write tests:** encoding into `reservation.payload()` in place then
  `commit()` then `read_record()` round-trips byte-identically; a reservation that wraps
  is handled with no split.
- **Lifetime contract test:** in debug builds, using a `read_scope` span after a
  subsequent `commit`/`discard` trips an assertion/poison (documents and guards the
  contract).
- **Eviction/index parity:** `make_room_for`, `discard_oldest_records`,
  `live_record_descriptors`, `rebuild_record_index` produce identical descriptor
  sequences before/after the swap.
- **Both platforms:** Linux exercised in CI-linux; Windows mapping path exercised in
  CI-windows (these tests are pure logic — no Qt/offscreen needed).

## Migration stages

Small, individually-revertable, test-gated slices (change-governance posture). Each
stage keeps the public ring contract and the existing tests green.

1. **Stage 1 — `Magic_ring_buffer` primitive.** Standalone RAII double-map type
   (Linux + Windows), granularity-aligned, with the unit tests above. No consumer
   change; nothing in `Terminal_history_ring` uses it yet.
2. **Stage 2 — swap the backend, keep the copies.** Replace `m_storage` (vector) with
   `Magic_ring_buffer`; `copy_record_bytes`/`write_record_bytes` collapse from
   two-`memcpy` splits to single `memcpy`s (the region is now contiguous) but still
   copy. Behavior-identical; all existing tests green. This proves the mapping in
   isolation from the API contract change.
3. **Stage 3 — zero-copy read.** `read_record` returns in-ring spans; delete
   `copy_record_bytes` and the read-scope owned vector; document the span-validity
   contract; convert the traversal/codec/buffer-transition read sites; add the
   zero-copy-read and lifetime-contract tests. (Gated on the threading verification.)
4. **Stage 4 — zero-copy write.** `reserve_record` returns an in-ring span; `commit`
   drops the staging copy; delete `write_record_bytes`; convert the codec write site;
   add zero-copy-write tests.
5. **Stage 5 — cleanup.** Remove dead split-copy helpers and the now-unused vectors;
   decide and apply the `cap/8` bound question; dead-code sweep (`git grep`).

## Risks

- **Platform VM code on two OSes.** The mapping and its teardown are the only new
  platform-specific code; both the POSIX and Windows paths must be leak-free on every
  error branch (Sintra's biggest source of subtlety). Mitigated by the lifecycle/leak
  tests and a strict RAII boundary.
- **Lifetime contract.** Zero-copy spans must not outlive a mutation. This is the
  principal new correctness hazard; mitigated by single-thread ownership, read-then-
  decode consumers, and debug poisoning, but it is a real contract every read site must
  honor.
- **Threading assumption.** The whole "no guards needed" argument rests on record bytes
  never being read off the owner thread. If the verification gate fails, zero-copy read
  is scoped to owner-thread sites only and the cross-thread path keeps copying.
- **Granularity-rounded capacity** changes the exact byte capacity; capacity-exact
  assertions in tests must move to the rounded value.
- **Windows API floor** (VirtualAlloc2/MapViewOfFile3 → Win10 1803). State the floor;
  only add the legacy fallback if a target needs it.
- **Address-space for the 2× reservation** is trivial for scrollback sizes on 64-bit;
  noted for completeness.

## Open questions

- On unrecoverable mapping failure: hard-degrade to zero scrollback (recommended) vs.
  an owner-approved non-mapped fallback for a specific platform.
- Keep `max_record_bytes = cap/8` for parity, or relax now that there are no reader
  guards?
- Linux backing: `memfd_create` (cleanest, Linux 3.17+) vs. `shm_open`; fallback order.
- Should the descriptor index (`m_records` `std::deque`) also become a fixed-capacity
  ring of descriptors (Sintra `Index_stack`-style), removing the last growable
  container? Out of scope for the buffer swap; noted.
- Expose a host-facing "N rows" scrollback configuration on top of the byte capacity?
  Natural follow-on the magic ring enables; not part of this swap.

## Relationship to governance and other plans

Independent storage refactor of one component. Change-governance posture: small slices
gated by the existing parity tests (Rule 13 / Rule 10); single canonical backend, no
permanent parallel path (Rule 1); dead-code sweep at Stage 5 (Rule 4). The primary
justification is correctness/ergonomics and realizing the originally-intended Sintra-
style design; the copy/allocation elision is a secondary, measurable benefit — if it is
claimed as a performance win, it is measured end-to-end on a scrollback-heavy workload
(Rule 11), not asserted.
