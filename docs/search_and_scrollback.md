# Search And Scrollback

Terminal search is a session-owned view over published terminal content. The
public entry point is `VNM_TerminalSurface`; `Terminal_search_controller` keeps
match identity and `Terminal_render_snapshot::search_match_spans` carries only
the visible paint overlay. Search state is separate from selection state and
never changes selected text.

## Literal Match Semantics

Search uses a deliberately small contract:

- the query is a non-empty, case-sensitive `QString` literal;
- matches are non-overlapping;
- each physical terminal row is searched independently;
- each reflow fragment is a physical row, so a match never crosses a fragment
  or row boundary;
- occupied cell text and interior gaps are searchable (gaps are spaces), while
  trailing unoccupied grid fill is not appended to the searchable row;
- a wide cell maps a match back to the complete cell width.

No regular expressions, case folding, normalization, or whole-word mode are
applied. Hosts that want a different query language can build it above this
literal API without changing terminal row identity rules.

## Public API

`searchQuery` is writable. `set_search_query()` replaces it and
`clear_search()` removes the query and all match overlays. `search_next()` and
`search_previous()` wrap at the ends and return `false` when no match can be
selected.

Query evaluation runs asynchronously. Submitting a non-empty query publishes
`SEARCHING` immediately; match counts, navigation, and visible overlays become
available only after the completion for the latest query and published-content
generation is accepted. Rapid edits coalesce, and stale completions are ignored.

The observable result properties are:

- `searchResultState`: `INACTIVE`, `SOURCE_UNAVAILABLE`, `SEARCHING`,
  `NO_MATCH`, or `MATCH`;
- `searchMatchCount`: the number of matches in the retained public source;
- `currentSearchMatch`: a one-based display index, or zero when there is no
  current match.

All four search properties share `search_changed()`. A non-empty query is kept
across session replacement; until the next session has a safe published source,
its state is `SOURCE_UNAVAILABLE`.

Activating or changing a query chooses the first match at or after the published
viewport, wrapping to the beginning when necessary, and reveals it. A refresh
of the same query preserves an exact surviving match identity and updates
visible highlights without moving the viewport. A manual viewport move or
accepted deferred scroll intent supersedes an undelivered query reveal;
ordinary content refreshes do not create a new reveal intent. Explicit search
navigation scrolls the primary viewport just enough to reveal the current
match. Alternate-screen search covers its published grid only because that
buffer has no scrollback.

## Source And Provenance Safety

Primary search incrementally maintains a compact corpus of the complete
retained public row set at proven content-publication boundaries. Query scans
run away from the GUI thread and do not reconstruct missing history from a
visible render snapshot. Each match is tied to the active-buffer epoch,
retained history handle, and its ordinal within that retained logical line. The
physical row and columns are refreshed from the new safe corpus after content,
reflow, eviction, history reset, or buffer changes.

If the exact current identity survives a refresh, it remains current even when
reflow moves its fragment. If it does not survive, search selects the first
remaining match at or after the prior public row (with normal wrap). Evicted
matches disappear, active-buffer changes cannot reuse stale matches, and spans
are emitted only when their visible line provenance agrees with the match.

During DEC synchronized output, search never scans or publishes hidden live
content. An already-active search or a query first activated during a hold uses
the compact corpus from the last safe content publication. Completed visible
highlights and navigation are republished over that same safe snapshot.
Revealing an offscreen match materializes at most one viewport from live
backing, and only after every row handle, including its content generation,
exactly matches the published corpus. This proof applies equally to retained
and active-grid rows; the location of the last safe content viewport is not
treated as evidence that a match is visible. The snapshot is mapped back to
published coordinates and metadata, so hidden rows cannot enter it. A
successfully revealed viewport remains the release-visible viewport unless a
newer accepted viewport, input, query, or navigation action supersedes it. If no
safe source has been published, or eviction, reset, reflow, or mutation
prevents that exact bounded proof, the result is `SOURCE_UNAVAILABLE`; this is
intentionally different from `NO_MATCH`. Hidden output becomes searchable only
after the hold releases and a content snapshot is published.

## Rendering

Visible matches use `search_match_spans`, distinct normal/current colors, and
the existing overlay render pass. Selection spans remain separate and take
paint precedence where the two overlap. This keeps a search highlight from
looking like selected, copyable terminal text.
