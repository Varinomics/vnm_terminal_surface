# Public Surface

`VNM_TerminalSurface` is the public Qt Quick terminal item. It is declared in
`include/vnm_terminal/vnm_terminal_surface.h` and implemented by
`src/vnm_terminal_surface.cpp`.

Hosts use the surface as a C++ `QQuickItem`: create it under a
`QQuickWindow` content item or expose it to QML, size it, configure the Qt
properties, connect the signals that matter to the host, then call
`start_terminal()`. The terminal pipeline itself is C++ code, not a tree of QML
controls.

## Host Contract

The surface owns the Qt-facing boundary for one terminal session:

- maps Qt key, mouse, wheel, hover, focus, IME, geometry, window, screen, and
  scene-graph events into terminal-domain operations;
- creates the native terminal backend used by the session;
- publishes terminal metadata, process state, backend state, grid geometry,
  viewport state, and selection state as read-only Qt properties;
- exposes explicit OSC 8 hyperlink lookup and activation requests against the
  immutable published render snapshot;
- routes host operations such as paste, selection access, viewport scrolling,
  interrupt, terminate, and OSC 52 clipboard decisions into the same ordered
  session pipeline as backend output;
- captures immutable session snapshots in `updatePaintNode()` and renders them
  through the canonical atlas Qt Scene Graph render node.

The GUI thread owns the public surface and the session state behind it. Backend
threads report output, exit, and errors back through queued session callbacks.

## Configuration Properties

Writable Qt properties:

- `fontFamily` and `fontSize` select the terminal font. Positive font sizes are
  rounded to an integer pixel size, up to a supported maximum of 1024 pixels;
  a larger size is accepted and bounded to that maximum. Font,
  device-pixel-ratio, and item geometry changes recompute terminal grid metrics.
- `colorScheme` selects a bundled color scheme by name (the Windows Terminal
  built-in set; `Classic` is the default). The scheme drives the 16 ANSI colors
  plus the default foreground, background, cursor, and selection colors, and is
  switchable at runtime. The light-vs-dark rendering decision derives from the
  scheme background luminance. `available_color_schemes()` lists the scheme names
  and `color_scheme_preview(name)` returns a scheme's swatch colors (background,
  foreground, cursor, selection, and the 16 ANSI colors) for a picker UI. An
  unknown name is ignored and the current scheme is kept.
- `cursorStyle` is `BLOCK`, `BAR`, or `UNDERLINE`.
- `cursorBlinkEnabled` enables the render-side blink override.
- `textRendererMode` is `AUTO`, `MSDF`, or `GLYPH`. `AUTO` lets the atlas
  renderer select MSDF text when available and fall back to shaped glyph-atlas
  text. `MSDF` and `GLYPH` force that text path for diagnostic comparison.
- `lcdSubpixelOrder` is `AUTO`, `NONE`, `RGB`, `BGR`, `VRGB`, or `VBGR`.
  `AUTO` uses the window screen's Qt subpixel hint first and, on Windows, the
  system ClearType orientation when Qt reports no order. Manual values force
  the MSDF LCD sampling order; `NONE` keeps grayscale MSDF coverage.
- `scrollbackLimit` clamps to zero or greater and is applied to a live session.
- `synchronizedOutputStaleTimeoutMs` clamps to at least one millisecond and is
  used to release synchronized-output mode if the application leaves output
  hidden.
- `synchronizedOutputScrollPolicy` controls scroll behavior while DEC
  synchronized output is hiding live content. The default is
  `DEFER_UNTIL_CONTENT_PUBLICATION`; public scroll APIs and app chrome remain
  visually deferred until content is published. `IMMEDIATE_PUBLIC_PROJECTION`
  is opt-in and scrolls a copied public projection without exposing hidden live
  rows. Policy changes during an active hold are latched: the current hold keeps
  its entry policy, a diagnostic is recorded, and the next hold uses the new
  policy.
- `textAreaResizePolicy` is `APPLICATION_CONTROLLED` (the default) or
  `DISABLED`, and controls whether XTWINOPS `CSI 8 ; rows ; columns t` may move
  the text area. Hosts set `DISABLED` whenever the window manager owns their
  window geometry rather than they do, typically maximized, fullscreen, or
  minimized. Requests are then ignored: no grid change, no backend resize, and
  no `text_area_resize_requested()` signal. The policy applies to a live
  session, so hosts update it as their window state changes.
- `textAreaResizeArbitrationEnabled` turns an XTWINOPS
  `CSI 8 ; rows ; columns t` the terminal captures into a two-phase transaction
  instead of a sequence-point commit. It is off by default. See
  `text_area_resize_arbitration_requested()` below for the protocol, and for the
  one request shape the transaction cannot capture.
- `textAreaResizeArbitrationTimeoutMs` bounds how long backend output is held
  waiting for that answer. The default is 250 ms; 0 removes the bound and leaves
  only the session byte limit. Negative values clamp to 0.
- `mouseReportingPolicy` is `APPLICATION_CONTROLLED` or `DISABLED`. With the
  application-controlled policy, mouse-reporting terminal modes receive mouse
  input. Holding Shift forces local selection when no terminal mouse grab is
  active.
- `copyShortcutPolicy` controls plain Ctrl+C:
  `TERMINAL_INPUT` always sends terminal input,
  `COPY_SELECTION_OR_TERMINAL_INPUT` copies an attached local selection when
  present and otherwise sends terminal input, and `COPY_SELECTION_OR_IGNORE`
  never sends Ctrl+C to the child process. Retained `PAYLOAD_ONLY` text remains
  readable through `selected_text()`, but it is not treated as copyable by the
  built-in plain Ctrl+C shortcut.
- `copyOnSelect` controls whether completing a non-empty local mouse selection
  immediately copies its plain text to the system clipboard. It defaults to
  `false`.
- `wheelEventPolicy` controls primary-screen wheel routing:
  `APPLICATION_CONTROLLED` tries terminal mouse or alternate-screen behavior
  before local scrollback, `LOCAL_SCROLLBACK_FIRST` prefers local scrollback
  when it can move, and `LOCAL_SCROLLBACK_ONLY` does not route ordinary wheel
  events to the child process.
- `alternateScreenWheelPolicy` controls wheel events in the alternate screen:
  `MOUSE_REPORTING_FIRST` lets mouse-reporting applications consume wheel
  events before key fallback, `CURSOR_KEYS` maps wheel movement to Up and Down
  key input, and `PAGE_KEYS` maps it to PageUp and PageDown key input.
- `bracketedPastePolicy` is `DISABLED`, `APPLICATION_CONTROLLED`, or `ENABLED`.
  Application-controlled paste frames pasted text only when the terminal has
  enabled bracketed paste mode.
- `audibleBellPolicy` and `visualBellPolicy` enable or disable the corresponding
  bell effects.

`backend_output_capture_config()` and `set_backend_output_capture_config()` are
C++ diagnostic accessors rather than Qt properties. When configured before
process start, the session retains a bounded suffix of raw backend output in
capture-owned segments below the configured local path prefix. Each accepted
chunk is flushed before the backend callback returns, favoring crash recovery
and external-reader visibility over write throughput. The public recovery API
validates capture artifacts, distinguishes finalized and interrupted sessions,
and returns the retained segments in sequence order. Capture paths must reside
in an existing trusted local directory; remote drives, UNC paths, and reparse
traversal are rejected.

`retained_history_capacity_bytes()` and
`set_retained_history_capacity_bytes()` are C++ pre-session configuration
accessors for the retained-history byte ring. The default, minimum, and maximum
supported capacities are available from
`default_retained_history_capacity_bytes()`,
`minimum_retained_history_capacity_bytes()`, and
`maximum_retained_history_capacity_bytes()`. The requested capacity is rounded
up to the native ring alignment. A smaller capacity can evict retained rows
before `scrollbackLimit` is reached; the row limit and byte capacity are
independent bounds. If one encoded row exceeds the ring's bounded per-record
size, that row is discarded from retained history and the live session
continues. Capacity changes are ignored after a session has started.

## Published State

Read-only Qt properties expose the host-visible state:

- `terminalTitle` and `terminalIconName` come from terminal metadata updates.
- `processState` is `NOT_STARTED`, `STARTING`, `RUNNING`, `EXITED`, or
  `FAILED`.
- `backendReady` reports whether the backend accepted the active launch.
- `backendGeometryInSync` reports whether the backend has acknowledged the
  current terminal grid size.
- `rows` and `columns` are the grid size derived from item geometry and font
  metrics.
- `scrollbackRows`, `viewportVisibleRows`, `viewportOffsetFromTail`, and
  `viewportAtTail` describe the published primary-screen viewport. During an
  opt-in immediate synchronized-output hold, these properties describe only the
  public projection. If that projection is invalidated before release, the
  values freeze at the last visible public state until live content is
  published. Hidden live scrollback growth does not change these properties.
- `selectionState` is `NONE` or `ACTIVE`.
- `searchQuery` is the current literal query. `searchResultState` distinguishes
  inactive search, an unavailable safe source, no matches, and matches.
  `searchMatchCount` is the total retained-public match count and
  `currentSearchMatch` is its one-based current index (zero when absent).

Every property has a matching notify signal. Grid size changes emit
`grid_geometry_changed()`. Viewport changes emit `viewport_changed()`. Backend
resize synchronization changes emit `geometry_sync_changed()`.

## Process Launch

`start_terminal(Terminal_process_start_request)` is the sole public launch
boundary and starts one native terminal backend:

- Windows uses ConPTY.
- Linux and macOS use POSIX PTY APIs.
- Platforms without a native backend fail with `backend_error()`.

The request carries ordered argv, a caller-chosen absolute working directory,
a policy-sanitized explicit base environment, and a separate optional
capability contribution. `argv[0]` names the executable; the surface never
parses a shell command string. The surface reads no ambient environment. It
validates platform name identity and collisions, composes the final child
environment, resolves a bare executable from that environment, revalidates the
working directory immediately at the native boundary, and makes at most one
native dispatch.

The base is trusted to contain Windows drive pseudo-variables such as `=C:` and
preserves them through the native environment block. Capability contributions
cannot contain pseudo-variables, collide with the base, or alter terminal-owned
`TERM`, `COLORTERM`, and `NO_COLOR` names or executable-lookup names. The
surface replaces terminal-owned base entries with its own identity. Embedded
NULs, invalid names, and platform-equivalent duplicates are rejected before
native dispatch.

`Terminal_process_start_result` reports acceptance, whether native dispatch
occurred, and whether the outcome is determinate. A rejected determinate result
with no dispatch can be corrected and submitted as a new request. A dispatched
indeterminate result must never be retried because a child may already exist.
Starting again after a determinately exited process resets the old session
before launching the new one.

Startup errors use a flat taxonomy. Structurally invalid or policy-invalid
requests report `INVALID_LAUNCH_CONFIG`; a well-formed executable that cannot be
resolved or admitted reports `START_FAILED`. Error classification is orthogonal
to native dispatch and determinacy. Callers must use `native_dispatch_occurred`
and `determinacy` themselves to decide whether native dispatch occurred and
whether retry is safe.

Initial rows and columns come from the item size, font metrics, and device pixel
ratio. Hosts should size the item before launch. Later geometry, font, screen,
or device-pixel-ratio changes refresh the grid and send ordered resize requests
through the session.

Surface-owned product sessions opt into viewport-stable selection visuals for
live primary scrollback: visible selection spans remap as the published viewport
moves. The lower-level backend/session contract keeps that projection disabled
by default, so direct session owners must enable it explicitly when they want
the same visual behavior.

Attachment survival across content publications is based on exact retained-row
provenance. When every selected row remains exactly provable, the attachment can
survive primary or alternate scrolling, clearing scrollback for active rows, and
a same-column height resize. Width reflow, buffer transitions, or loss or
mutation of any selected row detach the visual selection while preserving its
immutable payload. Starting a replacement session clears both the attachment
and payload. See
[Selection and row provenance](selection_and_provenance.md) for the complete
boundary contract.

`interrupt_process()` and `terminate_process()` forward lifecycle requests to
the active session. They return `false` and emit a backend error if there is no
active session or if the backend rejects the request. Destroying a surface with a
live process requests termination during teardown.

## Host Operations

The C++-only `submit_utf8_message(QByteArray message_utf8)` operation validates
one complete UTF-8 message, rejects unsupported terminal controls, and admits
the encoded body plus its submit action as one terminal command and backend
write. It reports a typed outcome for invalid or empty input, raw or encoded
size overflow, a non-running session, queue pressure, and backend rejection.
The public message ceiling bounds both the raw UTF-8 input and the fully encoded
terminal command; platform input encoding may expand some characters before
the second check. Bracketed-paste framing, when active, closes before the final
submit byte.

Invokable methods:

- `selected_text()` returns the local terminal selection, including retained
  payload-only text after a visual detachment. When synchronized output is
  hiding unpublished model changes, it reads from the visible render snapshot
  so host copy behavior matches what the user can see.
- `clear_selection()` clears local selection and drag state.
- `refresh_grid_from_item_geometry()` re-derives the terminal grid from the
  current item geometry, resizing the model and the backend when the two have
  diverged. It issues no resize when they already agree, though it still
  re-resolves render state, so it is not free. See the
  `text_area_resize_requested()` notes under [Runtime Signals](#runtime-signals)
  for when a host needs it.
- `set_search_query(QString query)` sets the literal terminal query;
  `clear_search()` clears it. `search_next()` and `search_previous()` navigate
  with wraparound and reveal the current match, returning `false` when no match
  can be selected. See [Search and scrollback](search_and_scrollback.md) for
  exact row, reflow, refresh, and synchronized-output semantics.
- `paste_text(QString text)` writes paste text through the terminal input path.
  Paste text is UTF-8 encoded, CR and CRLF are normalized to LF, C0/C1 controls
  other than LF and TAB are removed, and bracketed-paste framing follows
  `bracketedPastePolicy`.
- `scroll_viewport_lines(int line_delta)` moves the published primary-screen
  scrollback viewport by lines.
- `scroll_to_offset_from_tail(int offset_from_tail)` moves the published
  primary-screen scrollback viewport to an offset from the tail.
- `respond_clipboard_write(quint64 request_id, Clipboard_response_decision
  decision)` answers a pending OSC 52 write request.
- `explicit_hyperlink_at(qreal x, qreal y)` returns the untrusted OSC 8 target
  bytes at an item-coordinate point in the current published render snapshot,
  or an empty byte array when the point is outside an explicit hyperlink.
  This method never scans plain text for URL-like content.

Viewport operations return `true` when the visible viewport moves, or when an
invalidated immediate public projection accepts a deferred release intent. They
return `false` for no session, zero or no-op movement, alternate-screen state,
hidden synchronized output under the default deferred policy, or an
already-boundary request with no accepted deferred intent. Large offset requests
may clamp to the available scrollback and still return `true` if the viewport
moved.

Under `IMMEDIATE_PUBLIC_PROJECTION`, valid synchronized-output holds are an
exception to the default hidden-output behavior: `scroll_viewport_lines()` and
`scroll_to_offset_from_tail()` publish visible
`basis=PUBLIC_PROJECTION, purpose=SCROLL` snapshots and return `true` when the
public projection moves. Clamping uses public projection bounds only. After
projection invalidation, these methods still record a deferred release intent
without consulting hidden live bounds; that accepted deferred intent returns
`true` even though visible properties remain frozen. If neither visible
movement nor deferred intent is possible, the methods return `false`.

The C++ diagnostic/source overloads used by application chrome attach transcript
source labels such as `api.lines`, `api.offset`, `key.page`,
`surface.text_area.wheel`, `app.scrollbar.wheel`, `app.scrollbar.page`,
`app.scrollbar.track`, and `app.scrollbar.thumb`. In the app scrollbar,
`app.scrollbar.page` is the plain track-page route and `app.scrollbar.track` is
the Ctrl-track absolute-position route. The labels distinguish replay routes
only; they do not change scrolling semantics. `surface.text_area.wheel` replays
through the same published-state wheel path used by direct text-area wheel
handling. `key.page`, `app.scrollbar.wheel`, and `app.scrollbar.page` replay
through the public line-scroll path, while `api.offset`, `app.scrollbar.track`,
and `app.scrollbar.thumb` replay through the public offset path. Wheel trace
events use the same `source` taxonomy, but their `route` field describes how
that wheel event was handled, such as local scroll, mouse tracking, alternate
screen keys, or control zoom. Diagnostics report visible scroll, deferred
intent, and event acceptance separately; an accepted deferred intent is not a
visible/local scroll application.

The diagnostic strings emitted in transcript and snapshot diagnostics, including
invalidated-projection fallback names, are diagnostic schema values rather than
stable public API enum names. Hosts should branch on the documented surface
policy and method return values, not on diagnostic string spelling.

## Diagnostic Replay Tool

When built with `VNM_TERMINAL_ENABLE_TRANSCRIPT_CAPTURE_REPLAY`, the
`vnm_terminal_transcript_replay` diagnostic CLI accepts
`[--strict-all-snapshots] <transcript.ndjson>`. Strict all-snapshot comparison is
the default, and the named option lets corpus and test callers make that intent
explicit. The tool compares every recorded snapshot event against replayed
semantic state runs from the same validated causal input group. It rejects
missing or conflicting session-sequence ownership and validates known derived
event order. In particular, every `surface.scroll` result must follow a matching
`surface.scroll_intent` and agree with the result of the one application of that
intent, including request, action, applied delta, and before/after viewport; the
tool never reapplies a mismatched result.

Recorded and replayed causal layouts are derived and validated independently;
recorded group identifiers are never imposed on replay publications. The tool
then requires their normalized group counts, owner kinds, driver order, and
payload signatures to agree. Deadline-driven `backend.output` fragments retain
one logical group only when each continuation follows a full 4096-byte slice;
the final slice may be shorter. Replay queues and drains that logical byte stream
under one replay owner, so callback slicing cannot manufacture or conceal a
causal group boundary. A `session.resize` result in request/result-schema
transcripts must retain the sequence owned by its immediately applicable
`session.resize_request`; legacy direct `session.resize` events remain accepted.
Legacy direct results and replay-produced modern pairs compare through the same
exact host-resize outcome signature. Modern pairs additionally require their
request fields to agree with the result and retain exact request/result driver
comparison, so the compatibility normalization does not admit corrupt modern
metadata.
Parser-produced `session.text_area_resize_request` events must retain their
current `backend.output` transition owner.

For each causal group, recorded and replayed snapshot streams must either both
be empty or both be nonempty. Their final semantic runs are compared tail to
tail. Earlier recorded checkpoints must then occur in order before that final
run; replay-only runs are accepted only before or between those checkpoints.
Snapshot sequence numbers and dirty-row ranges remain diagnostics because
callback budgets can coalesce a publication or expose an additional
publication without changing the model. Dirty ranges are compared tail to tail
for every publication in a matched run, and mismatches plus unpaired recorded or
replayed publications are counted but remain nonfatal. A transcript without
snapshot diagnostics fails the strict gate. Final-only snapshot replay is not a
supported mode.

The summary reports recorded/replayed causal-group counts, causal-driver and
causal-protocol divergences, semantic run and scheduling-surplus counts,
fixed-digest object checks,
`snapshot_alignment_comparison_work`, dirty mismatches, and both unpaired dirty
publication counts. Alignment uses ordered per-digest positions, so ordinary
comparison work is amortized linear in the recorded and replayed run counts;
full `QJsonObject` equality is used only after fixed-size semantic digests match.

The C++ diagnostic scroll overloads (`scroll_viewport_lines_with_diagnostics()`,
`scroll_to_offset_from_tail_with_diagnostics()`) return a
`wheel_scroll_diagnostic_result_t` carrying the TYPED enums
`Scroll_noop_cause no_op_cause` and `Scroll_action scroll_action` alongside
`event_accepted`/`session_present`. Hosts that need to react to a scroll outcome
(for example, the bundled scrollbar distinguishing a `BOUNDARY_OR_CLAMP` no-op
from a real movement) MUST branch on these enum values. `scroll_noop_cause_name()`
and `scroll_action_name()` exist ONLY to format those enums for transcript/debug
output (`NONE` maps to an empty string); they are not control-flow API, and their
string spellings are diagnostic schema, not a stability contract.

## Hyperlink Interaction

OSC 8 hyperlink interaction is snapshot-owned and host-mediated. Explicit
hyperlinks are underlined, and hovering a cell whose current published snapshot
contains hyperlink metadata uses the pointing-hand cursor. Ctrl+left-click is
the activation gesture. A press becomes a request only when release still lands
on the same OSC 8 identity and target in the then-current published snapshot.
Moving to another target, changing the modifier chord, or replacing the target
before release cancels the gesture.

The activation gesture takes precedence over a new terminal mouse-reporting
press when the pointer is on an explicit hyperlink. An existing terminal mouse
grab is not interrupted. Plain left-click and Shift-drag retain their existing
mouse-reporting and local-selection routes, and URL-like plain text is never
made interactive.

On a completed gesture, the surface emits
`explicit_hyperlink_activation_requested(QByteArray target)`. The target is
untrusted terminal data. The surface does not parse it as an application URL,
validate a scheme, or open an external resource. Hosts must apply their own
supported-scheme and URL policy before dispatch, and must not dispatch merely
because lookup or hover identified a target.

## Clipboard Policy

There are three clipboard paths:

- Local copy uses the host clipboard directly when `copyShortcutPolicy` chooses
  to copy a local selection or `copyOnSelect` is enabled.
- Host paste can call `paste_text()` when the host already has text to paste, or
  `paste_clipboard_text()` to ask the surface to read paste text and route it
  through the normal paste path. `paste_clipboard_text()` uses the reader
  installed by `set_clipboard_text_reader()`; if no reader is installed, it falls
  back to `QClipboard::text(QClipboard::Clipboard)`. Hosts that need a bounded
  or policy-aware clipboard read should install a reader before exposing
  clipboard paste shortcuts. The bundled app uses this hook for both keyboard
  paste and right-click paste.
- OSC 52 clipboard writes are mediated by the host. The parser decodes the
  payload and the surface emits
  `clipboard_write_requested(request_id, target_selection, payload)`. The host
  must call `respond_clipboard_write()` with the matching request id.

`DENY` consumes the pending request and leaves the clipboard unchanged. `ALLOW`
writes the decoded payload to `QClipboard::Clipboard` only for target `c` or
`clipboard`. Wrong ids, stale ids, duplicate responses, and attempts to allow an
unsupported target return `false` and report `CALLBACK_MISSING`.

## Runtime Signals

Process and runtime signals:

- `process_started()`;
- `process_exited(Exit_reason reason, int exit_code)`;
- `backend_error(Backend_error_code code, QString message)`;
- `output_activity()`;
- `output_backpressure_changed(bool active)`;
- `bell_requested()`;
- `text_area_resize_requested(int rows, int columns)`;
- `clipboard_write_requested(quint64 request_id, QString target_selection,
  QByteArray payload)`;
- `explicit_hyperlink_activation_requested(QByteArray target)`.

`bell_requested()` reports a policy-enabled bell event. The surface has already
handled audible playback; hosts must not treat the signal as a request to play
sound.

`Exit_reason` is `EXITED`, `INTERRUPTED`, `TERMINATED`, or
`FAILED_TO_START`. `Backend_error_code` mirrors backend and session failure
classes such as invalid launch config, unavailable working directory, start,
write, resize, interrupt, terminate, output overflow, callback, and read
failures.

On Windows ConPTY, classification of exit code 130 is best effort when a later
input write overlaps final child output and exit. ConPTY can acknowledge that a
write reached its input pipe but cannot confirm that the child consumed it, so
`process_exited()` may report either `INTERRUPTED` or `EXITED` in that narrow
ordering. The exit code remains 130, and exit delivery and output drain are
unchanged.

`text_area_resize_requested()` is emitted for `CSI 8 ; rows ; columns t`
xterm text-area resize requests. The terminal model applies the requested grid
at the parser sequence point so following output is interpreted against the new
text area. Hosts that choose to honor this signal should resize the surrounding
item/window so the visual shell mirrors the terminal grid.

Because the grid moves at the sequence point, the decision cannot be answered
from the signal handler; by then it has already been applied and the backend
resized. A host that cannot move its text area therefore declares
`textAreaResizePolicy` DISABLED in advance, and the request is ignored outright:
no grid change, no backend resize, and no signal. Applying it and having the
host revert would resize the pty twice per request and would never converge
against a client that re-asserts its size on every resize it is told about.

A host that honors the signal but cannot land on the requested geometry, for
example when the window manager clamps the resize or the target equals the
current size, calls `refresh_grid_from_item_geometry()` to put the grid back on
the item. No geometry change arrives to reconcile it otherwise. That call
resizes synchronously, so invoking it from the signal handler nests inside
session notification delivery. Notifications still queued behind it keep their
order, but the nested call republishes surface state, so property change signals
can arrive ahead of them; deferring it to the next event loop turn avoids that.

`text_area_resize_arbitration_requested(request_id, rows, columns)` replaces
that whole shape for a host that sets `textAreaResizeArbitrationEnabled`. A
captured sequence then commits nothing on its own. Backend output stops at the
sequence point, the host resizes its window, and it answers with
`respond_text_area_resize(request_id, ACCEPT, actual_rows, actual_columns)`
carrying the grid it actually got, or
`respond_text_area_resize(request_id, REJECT, 0, 0)`. That window resize is not
the answer, but it does reach the grid and the backend, exactly as it would
with no request in flight, and the answer then commits the grid the host
reports. Honoring a request therefore costs one reflow and one backend resize
in total, whether the host's own resize or the answer applied it, and refusing
adds nothing to what that resize already cost. That grid is all the answer
decides: a C0 control byte the child embedded in the captured sequence, which
the parser applies ahead of the resize, takes effect whether the host answered
with the requested grid or with a clamped one. A captured request emits no
`text_area_resize_requested()`, because an arbitrating host has already moved
its window by the time the request settles.

The transaction captures a request out of the backend byte stream, ahead of the
parser that would dispatch it. One shape escapes that: a
`CSI 8 ; rows ; columns t` carrying an embedded C0 control byte and split across
a backend read boundary that falls after the control. The parser applies such a
control as it scans and carries only the stripped prefix across the boundary, so
the two halves are no longer joinable in the byte stream the terminal watches.
That request is not arbitrated. It commits the grid and resizes the backend at
the sequence point, and it emits `text_area_resize_requested()`, exactly as it
does with arbitration disabled.

A host that enables arbitration therefore connects both signals.
`text_area_resize_arbitration_requested()` is where it resizes its window and
answers; `text_area_resize_requested()` is where it handles an already-committed
request, the same handler it would write without arbitration. Leaving that
second signal unconnected moves the grid and the backend with nothing telling
the host to follow, which is the divergence the transaction exists to remove.

Resizing the window before answering is the expected order and does not settle
the request. The item geometry reaches the grid the way it always does, so
`rows()` and `columns()` report the grid the host actually got and the answer
can be truthful, while the held output stays held until the answer arrives. A
window resize is never itself an answer: only `respond_text_area_resize`, the
timeout, or the session ends the request.

At most one request is actionable to the host at a time. Delivery is part of
the optional arbitration capability rather than the common session-notification
stream. If the process exits after the session captured a request but before
the surface presents it, the surface emits neither the now-stale request nor a
matching historical settlement. Held output still replays once, followed by
the process-exit notification.

Output that arrives after the sequence is held until the host answers. The hold
is bounded twice: by `textAreaResizeArbitrationTimeoutMs`, and by the session's
own byte limit for a host that answers eventually while the process floods.
If the request itself exceeds the bounded control-sequence allowance, or if the
remainder of its callback already exceeds the byte limit, the session declines
the request at its sequence point without presenting it to the host and
continues processing normally.
`text_area_resize_arbitration_settled(request_id, outcome, rows, columns)`
reports every ending of a request that reached the host, whether it was the
host's `ACCEPTED` or `REJECTED` answer or a settlement the terminal had to make:
`HOLD_LIMIT_REACHED`,
`TEXT_AREA_RESIZE_DISABLED`, `ARBITRATION_DISABLED`, `PROCESS_EXITED`, or
`TIMED_OUT`. An `ACCEPT` carrying a grid the terminal cannot support is settled
as `REJECTED` instead, and `respond_text_area_resize` returns false with a
`RESIZE_FAILED` backend error. Held output always replays, against the answered
grid when the request was accepted and against the grid then current otherwise;
`rows` and `columns` are that grid. A host that armed UI on the request tears it
down on this signal rather than on its own answer.

Arbitration is an optional capability and adds no transcript event kind. A
transcript captured with it enabled replays under a session without it as the
sequence-point behavior, because the recorded backend byte stream is identical
either way and the decision was never part of it.

## Event And Render Overrides

The surface is an item with contents, input-method support, focus, hover, and
all mouse buttons enabled. Hosts normally interact through properties,
invokables, and signals; they do not call the protected event handlers.

At a high level:

- `keyPressEvent()` handles copy policy, page-scroll keys for primary
  scrollback, then terminal key encoding.
- `mousePressEvent()`, `mouseMoveEvent()`, `mouseReleaseEvent()`, and
  `hoverMoveEvent()` maintain explicit-hyperlink activation and feedback, route
  terminal mouse reporting when active, and otherwise maintain local selection.
  Shift-drag forces local selection.
- `wheelEvent()` handles Ctrl+wheel font zoom, terminal mouse wheel routing,
  alternate-screen wheel policy, and local scrollback policy.
- `inputMethodEvent()` sends commit text through the terminal input path and
  keeps preedit state render-only.
- `inputMethodQuery()` reports IME enablement and the cursor rectangle.
- `geometryChange()` and relevant `itemChange()` cases refresh metrics, bind
  window and screen signals, report focus changes, and handle scene-graph
  invalidation.
- `updatePaintNode()` captures immutable render inputs and updates the QSG
  render node. The render node builds the render frame and prepares atlas
  resources in `QSGRenderNode::prepare()`. `releaseResources()` schedules
  render-node release through the Qt Scene Graph lifecycle.

## Host Responsibilities

A host constructs a `QQuickWindow`, creates `VNM_TerminalSurface`, sizes it from
window geometry, and starts a process after the item is attached to a window.
The host owns surrounding application behavior such as command-line parsing,
window chrome, titlebar policy, clipboard policy decisions, hyperlink target
validation and external dispatch, and packaging.

The standalone Varinomics terminal application lives in the `vnm_terminal`
repository and uses this surface as its terminal engine.

## Internal Headers And Privileged First-Party Consumers

Headers under `include/vnm_terminal/internal/` are implementation detail, not
consumer API. They are never installed: the package smoke test
(`tests/package_smoke`) hard-fails if any `vnm_terminal/internal` header reaches
the install tree, and the public install interface exposes only
`vnm_terminal/vnm_terminal_surface.h`, `vnm_terminal/font_metrics.h`, and the
`vnm_terminal/diagnostics/` subtree. Embedders that consume the installed
package therefore cannot include internal headers and must rely on the public
surface, the public `diagnostics/` serializers, and the public font/metrics API.
Renderer diagnostics are exposed through the public `qsg_atlas` serializer.
Internal headers carry no source- or binary-stability guarantee and may change
without notice.

The first-party Varinomics terminal application (`vnm_terminal`) is a
deliberately privileged consumer. It builds this surface from source in-tree
(via `add_subdirectory`), not against the install interface, so it may include
`vnm_terminal/internal/*` for first-party development tooling, notably
render-profiler attachment (`VNM_TerminalSurface_render_bridge::set_render_profiler`)
and the app's own GUI-thread profiler (`Hierarchical_profiler`), both compiled
only under `VNM_TERMINAL_PROFILING_ENABLED`, plus the test/render handoff used by
the app's integration tests. This privilege is intentional and is not extended to
installed embedders; the app accepts that these internal types may change and
migrates in lockstep with the surface (build breaks are an accepted migration
tool).

Everything the app serializes *from the surface* goes through the public
`vnm_terminal/diagnostics/` serializers, which take only `VNM_TerminalSurface`.
Two internal writer headers exist for what the app serializes from its own
privileged state instead, so that the shared encodings have exactly one owner:

- `vnm_terminal/internal/profile_text_writers.h` writes a
  `Profile_node_snapshot` / `Profile_timeline_snapshot` tree in the profile TEXT
  format. The app holds those snapshot types already, through its own
  `Hierarchical_profiler`, and its GUI-thread section must match the format the
  surface uses for the render thread.
- `vnm_terminal/internal/metrics_json_writers.h` writes a 64-bit counter into a
  `QJsonObject` as a decimal string, because a JSON number is a double. The app
  emits its own sections of the runtime-metrics document next to the
  surface-owned ones and has to encode counters identically.

A third internal header exists for behaviour the app must not reimplement rather
than for an encoding: `vnm_terminal/internal/wheel_gesture.h` normalizes a
vertical wheel gesture into whole notches and owns the ctrl+wheel zoom range. The
app's scrollbar sits beside the surface and answers the same gesture, so two
implementations of it are visible to the user as a discontinuity when the pointer
crosses between them.

None of the three belongs in the installed API: the profile writers take internal
snapshot types, the counter writer is an encoding detail of the metrics document
rather than a capability an embedder should call, and the wheel normalization is
this application's input policy rather than the surface's contract.
