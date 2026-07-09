#include "vnm_terminal/internal/terminal_history_row_traversal.h"
#include "helpers/test_check.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

term::Terminal_history_row_cell make_cell(QString text)
{
    term::Terminal_history_row_cell cell;
    cell.text     = std::move(text);
    cell.occupied = true;
    return cell;
}

term::Terminal_history_row_record make_record(std::uint64_t row_sequence)
{
    term::Terminal_history_row_record record;
    record.provenance.retained_line_id    = row_sequence;
    record.provenance.content_generation  = row_sequence + 1000U;
    record.provenance.source =
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE;
    record.metadata.source_width = 1;
    record.metadata.style_reference =
        term::Terminal_retained_row_style_reference::ROW_LOCAL_RESOLVED_STYLE;
    record.metadata.wrap_state =
        term::Terminal_retained_row_wrap_state::HARD_BOUNDARY;
    record.cells.push_back(make_cell(QStringLiteral("x")));
    return record;
}

term::terminal_history_row_record_identity_t make_identity(
    std::uint64_t                   row_sequence,
    term::terminal_history_handle_t previous_handle)
{
    return {
        9U,
        row_sequence,
        previous_handle.byte_sequence,
        previous_handle.row_sequence,
    };
}

term::Terminal_history_row_record_append_result append_row(
    term::Terminal_history_ring&    ring,
    std::uint64_t                   row_sequence,
    term::terminal_history_handle_t previous_handle)
{
    return term::encode_terminal_history_row_record_to_ring(
        ring,
        make_record(row_sequence),
        make_identity(row_sequence, previous_handle));
}

bool test_forward_and_backward_traversal_across_wrap()
{
    bool ok = true;

    term::Terminal_history_ring ring({2048U, 2048U});
    ok &= check(ring.status() == term::Terminal_history_ring_status::OK,
        "Phase 4C traversal wrap fixture ring initializes");

    std::vector<term::terminal_history_handle_t> handles;
    handles.reserve(10U);

    term::terminal_history_handle_t previous_handle;
    bool fixture_reached_wrap_state = false;
    for (std::uint64_t row_sequence = 1U; row_sequence <= 64U; ++row_sequence) {
        const term::Terminal_history_row_record_append_result append =
            append_row(ring, row_sequence, previous_handle);
        ok &= check(append.status == term::Terminal_history_row_record_codec_status::OK,
            "Phase 4C traversal wrap fixture row encodes");
        handles.push_back(append.history_handle);
        previous_handle = append.history_handle;

        const term::terminal_history_handle_t latest_handle = handles.back();
        const bool latest_wraps =
            latest_handle.byte_sequence % ring.capacity_bytes() +
                latest_handle.record_bytes >
            ring.capacity_bytes();
        const bool first_evicted =
            ring.read_record(handles.front().byte_sequence).status() ==
                term::Terminal_history_ring_status::OUT_OF_LIVE_RANGE;
        if (latest_wraps && first_evicted) {
            fixture_reached_wrap_state = true;
            break;
        }
    }

    const term::terminal_history_handle_t latest_handle = handles.back();
    ok &= check(fixture_reached_wrap_state &&
        latest_handle.byte_sequence % ring.capacity_bytes() +
            latest_handle.record_bytes > ring.capacity_bytes(),
        "Phase 4C fixture produces a physically wrapped live row record");
    ok &= check(ring.read_record(handles.front().byte_sequence).status() ==
            term::Terminal_history_ring_status::OUT_OF_LIVE_RANGE,
        "Phase 4C fixture evicts the first row before traversal");

    std::vector<term::terminal_history_handle_t> live_handles;
    for (const term::terminal_history_handle_t& handle : handles) {
        if (ring.read_record(handle.byte_sequence).status() ==
            term::Terminal_history_ring_status::OK)
        {
            live_handles.push_back(handle);
        }
    }
    ok &= check(live_handles.size() >= 2U,
        "Phase 4C fixture keeps multiple live rows after wrap eviction");
    if (live_handles.size() < 2U) {
        return ok;
    }

    term::Terminal_history_row_traversal traversal(ring);

    term::Terminal_history_row_traversal_result current = traversal.oldest_live_row();
    ok &= check(current.status == term::Terminal_history_row_traversal_status::OK &&
            current.row.history_handle.row_sequence ==
                live_handles.front().row_sequence,
        "Phase 4C oldest live row resolves after wrap eviction");

    for (std::size_t index = 1U; index < live_handles.size(); ++index) {
        current = traversal.next_row_after(current.row.history_handle);
        ok &= check(current.status == term::Terminal_history_row_traversal_status::OK &&
                current.row.history_handle.row_sequence ==
                    live_handles[index].row_sequence,
            "Phase 4C forward traversal advances by live record length");
    }

    const term::Terminal_history_row_traversal_result after_latest =
        traversal.next_row_after(current.row.history_handle);
    ok &= check(after_latest.status == term::Terminal_history_row_traversal_status::NOT_FOUND,
        "Phase 4C forward traversal stops at the published head");

    current = traversal.latest_live_row();
    ok &= check(current.status == term::Terminal_history_row_traversal_status::OK &&
            current.row.history_handle.row_sequence ==
                live_handles.back().row_sequence,
        "Phase 4C latest live row resolves from the rebuildable directory");

    for (std::size_t index = live_handles.size() - 1U; index > 0U; --index) {
        current = traversal.previous_row_before(current.row.history_handle);
        ok &= check(current.status == term::Terminal_history_row_traversal_status::OK &&
                current.row.history_handle.row_sequence ==
                    live_handles[index - 1U].row_sequence,
            "Phase 4C backward traversal follows previous row byte and row sequence");
    }

    const term::Terminal_history_row_traversal_result before_oldest =
        traversal.previous_row_before(current.row.history_handle);
    ok &= check(before_oldest.status == term::Terminal_history_row_traversal_status::NOT_FOUND &&
            before_oldest.ring_status == term::Terminal_history_ring_status::OUT_OF_LIVE_RANGE,
        "Phase 4C backward traversal treats an evicted previous row as not live");

    return ok;
}

bool test_directory_drop_and_missing_cache_rebuild_preserves_content()
{
    bool ok = true;

    term::Terminal_history_ring ring({4096U, 4096U});
    term::Terminal_history_row_traversal traversal(ring);

    term::terminal_history_handle_t previous_handle;
    for (std::uint64_t row_sequence = 1U; row_sequence <= 3U; ++row_sequence) {
        const term::Terminal_history_row_record_append_result append =
            append_row(ring, row_sequence, previous_handle);
        ok &= check(append.status == term::Terminal_history_row_record_codec_status::OK,
            "Phase 4C directory fixture row encodes");
        previous_handle = append.history_handle;
    }

    const term::Terminal_history_row_traversal_rebuild_result rebuild =
        traversal.rebuild_directory();
    ok &= check(rebuild.status == term::Terminal_history_row_traversal_status::OK &&
            rebuild.row_count == 3U,
        "Phase 4C row directory rebuilds from live records");

    traversal.discard_directory_cache();
    ok &= check(traversal.directory_cache_size() == 0U,
        "Phase 4C directory cache can be dropped explicitly");

    const term::Terminal_history_row_traversal_result rebuilt_row =
        traversal.find_row_sequence(2U);
    ok &= check(rebuilt_row.status == term::Terminal_history_row_traversal_status::OK &&
            rebuilt_row.row.history_handle.row_sequence == 2U &&
            rebuilt_row.row.record.cells.front().text == QStringLiteral("x"),
        "Phase 4C dropped directory rebuild loses no row content");

    const term::Terminal_history_row_record_append_result fourth =
        append_row(ring, 4U, previous_handle);
    ok &= check(fourth.status == term::Terminal_history_row_record_codec_status::OK,
        "Phase 4C missing-cache fixture appends a later row");

    const term::Terminal_history_row_traversal_result later_row =
        traversal.find_row_sequence(4U);
    ok &= check(later_row.status == term::Terminal_history_row_traversal_status::OK &&
            later_row.row.history_handle.row_sequence == 4U,
        "Phase 4C stale missing directory rebuilds when live bounds change");

    return ok;
}

bool test_cache_hits_validate_live_identity()
{
    bool ok = true;

    term::Terminal_history_ring ring({4096U, 4096U});
    term::Terminal_history_row_traversal traversal(ring);

    const term::Terminal_history_row_record_append_result first =
        append_row(ring, 1U, {});
    ok &= check(first.status == term::Terminal_history_row_record_codec_status::OK,
        "Phase 4C cache validation first row encodes");
    const term::Terminal_history_row_record_append_result second =
        append_row(ring, 2U, first.history_handle);
    ok &= check(second.status == term::Terminal_history_row_record_codec_status::OK,
        "Phase 4C cache validation second row encodes");

    const term::Terminal_history_row_traversal_result cached_row =
        traversal.find_row_sequence(2U);
    ok &= check(cached_row.status == term::Terminal_history_row_traversal_status::OK,
        "Phase 4C row-directory cache hit resolves before mismatch test");

    term::terminal_history_handle_t generation_mismatch = second.history_handle;
    ++generation_mismatch.content_generation;
    const term::Terminal_history_row_traversal_result mismatch =
        traversal.resolve_cached_row(generation_mismatch);
    ok &= check(mismatch.status ==
            term::Terminal_history_row_traversal_status::CACHE_ENTRY_INVALID,
        "Phase 4C cached handle with mismatched row header identity is invalidated");
    ok &= check(mismatch.codec_status ==
            term::Terminal_history_row_record_codec_status::CONTENT_GENERATION_MISMATCH,
        "Phase 4C cache validation reports the row-record identity mismatch");

    term::terminal_history_handle_t byte_mismatch = second.history_handle;
    ++byte_mismatch.byte_sequence;
    const term::Terminal_history_row_traversal_result wrong_boundary =
        traversal.resolve_cached_row(byte_mismatch);
    ok &= check(wrong_boundary.status ==
            term::Terminal_history_row_traversal_status::CACHE_ENTRY_INVALID,
        "Phase 4C cached handle at a non-record boundary is invalidated");

    const term::Terminal_history_row_traversal_result valid_again =
        traversal.find_row_sequence(2U);
    ok &= check(valid_again.status == term::Terminal_history_row_traversal_status::OK &&
            valid_again.row.history_handle == second.history_handle,
        "Phase 4C invalid external cache entries do not replace the live row record");

    return ok;
}

bool test_non_monotonic_row_sequence_chain_is_rejected()
{
    bool ok = true;

    term::Terminal_history_ring ring({4096U, 4096U});
    term::Terminal_history_row_traversal traversal(ring);

    const term::Terminal_history_row_record_append_result first =
        append_row(ring, 2U, {});
    ok &= check(first.status == term::Terminal_history_row_record_codec_status::OK,
        "Phase 4C non-monotonic fixture first row encodes");

    const term::Terminal_history_row_record_append_result second =
        append_row(ring, 1U, first.history_handle);
    ok &= check(second.status == term::Terminal_history_row_record_codec_status::OK,
        "Phase 4C non-monotonic fixture remains codec-decodable");

    const term::Terminal_history_row_traversal_rebuild_result rebuild =
        traversal.rebuild_directory();
    ok &= check(rebuild.status == term::Terminal_history_row_traversal_status::ROW_LINK_MISMATCH,
        "Phase 4C rebuild rejects a non-monotonic physical row sequence chain");

    const term::Terminal_history_row_traversal_result next =
        traversal.next_row_after(first.history_handle);
    ok &= check(next.status == term::Terminal_history_row_traversal_status::ROW_LINK_MISMATCH,
        "Phase 4C forward traversal rejects a non-monotonic next row sequence");

    const term::Terminal_history_row_traversal_result previous =
        traversal.previous_row_before(second.history_handle);
    ok &= check(previous.status == term::Terminal_history_row_traversal_status::OK &&
            previous.row.history_handle == first.history_handle,
        "Phase 4C backward traversal still validates the explicit previous-row link");

    return ok;
}

}

int main()
{
    bool ok = true;
    ok &= test_forward_and_backward_traversal_across_wrap();
    ok &= test_directory_drop_and_missing_cache_rebuild_preserves_content();
    ok &= test_cache_hits_validate_live_identity();
    ok &= test_non_monotonic_row_sequence_chain_is_rejected();
    return ok ? 0 : 1;
}
