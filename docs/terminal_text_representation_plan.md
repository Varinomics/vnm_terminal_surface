# Terminal Text Representation Plan

This plan describes a staged migration from `QString` as the default internal
hot-path text storage toward compact terminal-native text representations, while
keeping Qt-facing APIs and fallback rendering behavior stable.

The intent is not to replace `QString` globally. The intent is to make
`QString` a boundary and fallback type for cases that actually need Qt text
APIs, and to keep common terminal text in cheaper internal forms for as long as
possible.

## Goals

- Reduce `QString` copy/refcount traffic, text-object footprint, UTF-16
  classification scans, and remaining `toUtf8()` conversion in hot
  render-frame and renderer paths.
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
- Do not bypass `QTextLayout` for atlas glyph production or for text that needs
  shaping, combining behavior, non-ASCII glyph fallback, or existing Qt
  rendering guarantees.
- Do not change terminal Unicode width policy as part of this migration.
- Do not remove current renderer fallback paths until benchmarks and tests prove
  the new representation covers the intended cases.

## Current Shape

The backend/parser path naturally receives bytes and can optimize byte scanning.
The renderer path mostly receives `QString` text runs, because Qt stores text as
UTF-16 and Qt text APIs consume `QString`.

This makes in-place UTF-16 scans cheap for current renderer code, but it does
not mean the code still pays the original easy cost for every printable ASCII
cell. Current source already includes two important optimizations:

- `printable_ascii_cell_text()` returns references into a static array of 95
  interned printable-ASCII `QString`s, and printable ASCII cell writes store
  those values. The common single-cell ASCII path therefore pays `QString`
  handle assignment/refcount traffic, not a fresh heap allocation per cell.
- Renderer-side ASCII fast paths already exist. The frame collects printable
  ASCII/simple-ASCII counters, and packed text sidecars copy printable ASCII
  UTF-16 code units directly to bytes for representation and accounting. Atlas
  glyph production shapes printable ASCII into the same glyph records as other
  text.

The remaining plausible costs are narrower:

- atomic refcount churn when cells are copied, especially from model rows into
  render snapshots;
- memory footprint from storing a `QString` handle in every model cell and
  emitted render cell;
- repeated UTF-16 classification when the same trusted ASCII text is recognized
  again at later stages;
- `toUtf8()` conversion for non-ASCII fallback and for paths that still lack an
  ASCII side channel.

The highest-confidence improvement area is ASCII-heavy render-frame construction
and renderer submission, not public Qt API replacement and not broad `QString`
replacement.

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

Before committing to the full category set, Stage 3 should also evaluate a
simpler cell-storage design: inline storage for an empty value or one Unicode
scalar (`char32_t` plus display metadata), with a side table or owned fallback
only for multi-codepoint grapheme/combining cases. That shape may remove
`QString` from all single-codepoint cells, not only printable ASCII cells, while
keeping branch count lower than a broad five-kind text variant. The stage should
choose the simpler representation if it covers the measured hot cells and keeps
Qt materialization at the existing boundaries.

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
- renderer-side classification and packing paths;
- byte-preserving backend-to-model-to-frame transit for simple text.

Any call site that needs locale-sensitive, shaped, or UTF-16-indexed behavior
must materialize or retain `QString`.

## Migration Stages

### Stage 1: Instrument Text Churn

Add focused counters before changing storage:

- cells copied from model rows into render snapshots, split by printable ASCII,
  single-codepoint non-ASCII, multi-codepoint text, and empty/unoccupied cells.
- proxy counts for `QString` handle/refcount traffic in the model-to-snapshot
  copy path. Direct Qt atomic-refcount counts are not portable, so the first
  gate should use copy-site counts plus profiler timings.
- render snapshot construction time and renderer packing time, using the
  existing profile scopes where possible.
- `QString::toUtf8()` calls and converted byte volume in renderer packing and
  width-measurement fallback paths.
- ASCII vs non-ASCII render text volume.
- single-cell ASCII vs multi-cell ASCII counts.
- packed-text bytes emitted through the direct printable-ASCII copy path vs
  UTF-8 conversion.
- clean-row/cache reuse rates, so benchmark results are not misread when row
  caching masks run-construction cost.
- fallback materialization counts after introducing any internal text type.

Validation:

- no behavior change;
- benchmark output includes enough counters to decide which storage category
  matters most;
- measurements include both CMDG and at least one ordinary shell workload.

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

### Stage 3: Prototype Compact Cell Text Storage

Change the narrowest internal structure that currently stores common cell text
as `QString` so it can store compact text for the measured hot case.

Candidate areas:

- model cell text storage;
- render cell construction;
- render text run assembly.

The default prototype target is the model `Cell` text field and the
model-to-snapshot copy path, because current ASCII writes already avoid per-cell
allocation but snapshot publication still copies `QString` handles into
`Terminal_render_cell`. The prototype should compare:

- inline printable ASCII only;
- inline single Unicode scalar plus fallback side storage for complex text.

Do not change public Qt/QML APIs or Unicode width policy in this stage. A cell
that cannot be represented by the selected compact form must continue through
the existing `QString` fallback.

Validation:

- parser/model tests;
- screen operation tests;
- selection/copy tests;
- render-frame tests;
- CMDG benchmarks.

### Stage 4: Carry ASCII Runs Through Render Frames

Extend render-frame text runs so ASCII runs can carry byte spans or packed ASCII
payload references without materializing `QString`.

Renderer paths may consume compact ASCII spans directly for packing or report
accounting. The atlas renderer must shape those spans into canonical glyph
records rather than using a separate ASCII glyph producer. Paths that need Qt
text APIs should materialize `QString` at the boundary that consumes them.

This stage is explicitly conditional. Current renderer code already has
printable-ASCII classification, packed ASCII byte copying, batching, resource
prefiltering, and shaped atlas glyph production. Stage 4 should run only if
Stage 1 and Stage 3 show that the remaining renderer/frame boundary still costs
enough to justify another representation crossing.

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

- fewer `QString` handle copies/refcount operations in hot paths;
- lower model-cell or snapshot-cell memory footprint in the target workload;
- fewer UTF-16 rescans or UTF-8 conversions in hot paths;
- lower renderer/frame construction time;
- equal or better CMDG scene/draw/paint throughput;
- no regression in ordinary shell rendering and Unicode behavior.

If the first ASCII storage stage does not move measurable hot-path cost, keep
the existing `QString` boundary model and restrict future work to narrower
renderer-specific optimizations.
