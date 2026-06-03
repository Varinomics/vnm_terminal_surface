# Terminal Text Representation Plan

This plan describes a staged migration from `QString` as the default internal
hot-path text storage toward compact terminal-native text representations, while
keeping Qt-facing APIs and fallback rendering behavior stable.

The intent is not to replace `QString` globally. The intent is to make
`QString` a boundary and fallback type for cases that actually need Qt text
APIs, and to keep common terminal text in cheaper internal forms for as long as
possible.

## Goals

- Reduce `QString` construction, copying, UTF-16 scanning, and `toUtf8()`
  conversion in hot render-frame and renderer paths.
- Preserve public Qt/QML API compatibility.
- Preserve exact behavior for Unicode width, combining characters, wide glyphs,
  emoji, selection, copy/paste, transcript diagnostics, and Qt text-layout
  fallback.
- Keep migration slices small enough to benchmark independently.
- Avoid a one-shot string replacement that would couple parser, model, renderer,
  public API, tests, and diagnostics in one fragile change.

## Non-Goals

- Do not replace public `QString` properties, signals, slots, or invokables.
- Do not remove `QString` from Qt boundary code.
- Do not bypass `QTextLayout` for text that needs shaping, combining behavior,
  non-ASCII glyph fallback, or existing Qt rendering guarantees.
- Do not change terminal Unicode width policy as part of this migration.
- Do not remove current renderer fallback paths until benchmarks and tests prove
  the new representation covers the intended cases.

## Current Shape

The backend/parser path naturally receives bytes and can optimize byte scanning.
The renderer path mostly receives `QString` text runs, because Qt stores text as
UTF-16 and Qt text APIs consume `QString`.

This makes in-place UTF-16 scans cheap for current renderer code, but it also
means ASCII-heavy terminal content can still pay for:

- per-cell or per-run `QString` storage;
- repeated UTF-16 classification;
- conversion to UTF-8 for packed payloads;
- allocations around small text values.

The highest-confidence improvement area is ASCII-heavy render-frame construction
and renderer submission, not public Qt API replacement.

## Proposed Internal Types

Introduce an internal text representation with explicit storage categories.
Names are illustrative; final names should follow local conventions.

```cpp
enum class Terminal_text_kind
{
    EMPTY,
    ASCII_CELL,
    ASCII_BYTES,
    UTF8_BYTES,
    QSTRING,
};

class Terminal_text
{
public:
    Terminal_text_kind kind() const;
    bool is_empty() const;
    bool is_printable_ascii() const;
    bool is_single_ascii_cell() const;
    QByteArrayView ascii_bytes() const;
    QByteArrayView utf8_bytes() const;
    QString to_qstring() const;
};
```

Expected storage strategy:

- `EMPTY`: no allocation.
- `ASCII_CELL`: one byte inline, for the overwhelmingly common one-cell ASCII
  case.
- `ASCII_BYTES`: borrowed or owned byte span for runs known to be printable
  ASCII.
- `UTF8_BYTES`: byte span for text that is valid UTF-8 but not yet materialized
  as `QString`.
- `QSTRING`: fallback storage for Qt text layout, public API handoff, and cases
  where UTF-16 indexing is required.

The first implementation does not need every category. It can start with
`EMPTY`, `ASCII_CELL`, `ASCII_BYTES`, and `QSTRING`, then add `UTF8_BYTES` only
after the ASCII path is proven.

## Boundary Rules

`QString` remains required at these boundaries:

- public `VNM_TerminalSurface` API;
- Qt/QML properties, signals, slots, and invokables;
- `QTextLayout` and any path requiring Qt shaping;
- clipboard/selection return values;
- JSON, transcript, and diagnostic formatting where Qt APIs are used;
- tests that assert public text output.

Internal byte-backed text can be used where the code only needs:

- empty/single-cell checks;
- printable ASCII classification;
- packed render-frame payloads;
- raw glyph fast paths;
- byte-preserving backend-to-model-to-frame transit for simple text.

Any call site that needs locale-sensitive, shaped, or UTF-16-indexed behavior
must materialize or retain `QString`.

## Migration Stages

### Stage 1: Instrument Text Churn

Add focused counters before changing storage:

- `QString` text cell/run creations in render-frame construction.
- `QString::toUtf8()` calls in renderer packing.
- ASCII vs non-ASCII render text volume.
- single-cell ASCII vs multi-cell ASCII counts.
- fallback materialization counts after introducing internal text.

Validation:

- no behavior change;
- benchmark output includes enough counters to decide which storage category
  matters most.

### Stage 2: Add Read-Only Terminal Text Views

Introduce a lightweight `Terminal_text_view` that can reference either
`QString` or ASCII bytes without owning storage.

Use it only in helper functions first:

- printable ASCII checks;
- all-space checks;
- non-ASCII checks;
- packed byte append.

Validation:

- focused renderer tests;
- packed payload tests;
- CMDG benchmarks;
- no public API changes.

### Stage 3: Store ASCII Cell Text Compactly

Change the narrowest internal structure that currently stores common one-cell
ASCII text as `QString` so it can store inline ASCII instead.

Candidate areas:

- model cell text storage;
- render cell construction;
- render text run assembly.

This stage should avoid changing non-ASCII storage. A cell that is not exactly
one printable ASCII code point should continue to use the existing path.

Validation:

- parser/model tests;
- screen operation tests;
- selection/copy tests;
- render-frame tests;
- CMDG benchmarks.

### Stage 4: Carry ASCII Runs Through Render Frames

Extend render-frame text runs so ASCII runs can carry byte spans or packed ASCII
payload references without materializing `QString`.

Renderer paths should consume ASCII directly when they already use ASCII glyph
replacement or packed payload emission. They should materialize `QString` only
when falling back to Qt text layout.

Validation:

- QSG renderer tests, including text replacement and fallback tests;
- image/pixel comparisons where available;
- CMDG benchmarks;
- allocation and conversion counters.

### Stage 5: Add UTF-8 Byte Storage For Non-ASCII Candidates

Only after ASCII storage proves useful, evaluate whether valid UTF-8 byte spans
help for non-ASCII text.

This stage is higher risk because terminal rendering needs width, grapheme, and
shaping correctness. UTF-8 byte storage should remain a pre-materialization
format, not a replacement for Qt fallback.

Validation:

- Unicode width policy tests;
- combining and wide-character tests;
- text layout fallback tests;
- transcript/selection/copy tests.

### Stage 6: Remove Redundant `QString` Hot-Path Storage

After earlier stages prove coverage and performance, remove obsolete
`QString`-first storage from the specific hot paths that no longer need it.

This should be done only after counters show that fallback materialization is
rare in the target workloads and behavior tests cover the affected paths.

## Risk Areas

- UTF-16 indexing assumptions hidden in renderer or model code.
- Selection and copy behavior for mixed ASCII/non-ASCII rows.
- Grapheme, combining mark, and wide-character behavior.
- Lifetime bugs if text views reference transient frame/model buffers.
- Increased branching if the representation is too general too early.
- Regressions from materializing `QString` repeatedly at fallback boundaries.
- Benchmark wins that apply only to CMDG but regress normal shell workloads.

## Testing And Benchmarks

Each stage should run at least:

- parser and screen-model tests for model-facing changes;
- render-frame and QSG renderer tests for frame/renderer changes;
- selection and clipboard tests for text extraction changes;
- transcript-enabled and transcript-disabled backend-session tests when session
  or diagnostics are touched;
- `vnm_terminal` CMDG windowed accelerated benchmark suite;
- at least one ordinary shell/render smoke test, not only CMDG.

When a stage changes public output, add tests before removing old behavior.
When a stage changes only internal representation, tests should assert identical
public/rendered output and benchmark counters should prove reduced churn.

## Commit Strategy

Keep changes in small slices:

1. instrumentation only;
2. view helpers only;
3. one storage site;
4. one render-frame path;
5. one renderer path;
6. fallback and cleanup.

Do not combine representation changes with unrelated renderer cleanup. If a
stage needs both `vnm_terminal_surface` and `vnm_terminal` changes, treat it as a
coordinated cross-repo batch under the shared governance rules.

## Decision Gate

Continue the migration only if benchmark and counter evidence shows one of:

- fewer `QString` allocations/conversions in hot paths;
- lower renderer/frame construction time;
- equal or better CMDG scene/draw/paint throughput;
- no regression in ordinary shell rendering and Unicode behavior.

If the first ASCII storage stage does not move measurable hot-path cost, keep
the existing `QString` boundary model and restrict future work to narrower
renderer-specific optimizations.
