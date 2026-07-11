#include "vnm_terminal/internal/terminal_history_ring.h"
#include "helpers/test_check.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <new>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

std::vector<std::byte> bytes_from_text(std::string_view text)
{
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (char ch : text) {
        bytes.push_back(static_cast<std::byte>(ch));
    }
    return bytes;
}

void fill_payload(
    term::Terminal_history_ring_record_reservation& reservation,
    std::string_view                                text)
{
    const std::vector<std::byte> bytes = bytes_from_text(text);
    std::span<std::byte> payload = reservation.payload();
    std::copy(bytes.begin(), bytes.end(), payload.begin());
}

bool payload_equal(
    const term::Terminal_history_ring_read_scope& scope,
    std::string_view                              text)
{
    const std::vector<std::byte> expected = bytes_from_text(text);
    const std::span<const std::byte> payload = scope.payload();
    return payload.size() == expected.size() &&
        std::equal(payload.begin(), payload.end(), expected.begin(), expected.end());
}

term::terminal_history_ring_commit_result_t append_record(
    term::Terminal_history_ring& ring,
    std::string_view             text)
{
    term::Terminal_history_ring_record_reservation reservation =
        ring.reserve_record(text.size());
    fill_payload(reservation, text);
    return ring.commit(std::move(reservation));
}

bool test_capacity_alignment_and_one_octile_limit()
{
    bool ok = true;

    const std::size_t aligned_capacity =
        term::terminal_history_ring_aligned_capacity(513U, 256U);
    ok &= check(aligned_capacity == 768U,
        "ring capacity aligns to the configured page/octet boundary");

    term::Terminal_history_ring ring({513U, 256U});
    ok &= check(ring.status() == term::Terminal_history_ring_status::OK,
        "aligned ring initializes");
    ok &= check(ring.capacity_bytes() == aligned_capacity,
        "ring stores the aligned capacity");
    ok &= check(ring.max_record_bytes() == aligned_capacity / 8U,
        "ring maps Sintra-lineage one-octile commit limit");

    const std::size_t max_payload = ring.max_payload_bytes();
    term::Terminal_history_ring_record_reservation max_reservation =
        ring.reserve_record(max_payload);
    ok &= check(max_reservation.status() == term::Terminal_history_ring_status::OK,
        "one-octile boundary payload reserves");
    const term::terminal_history_ring_commit_result_t max_commit =
        ring.commit(std::move(max_reservation));
    ok &= check(max_commit.status == term::Terminal_history_ring_status::OK,
        "one-octile boundary payload commits");
    ok &= check(max_commit.record_bytes == ring.max_record_bytes(),
        "maximum payload consumes exactly one octile");

    const std::uint64_t head_before_oversize = ring.head_byte_sequence();
    term::Terminal_history_ring_record_reservation oversize =
        ring.reserve_record(max_payload + 1U);
    ok &= check(oversize.status() == term::Terminal_history_ring_status::OVERSIZE_RECORD,
        "oversize payload hard-fails explicitly");
    ok &= check(ring.head_byte_sequence() == head_before_oversize,
        "oversize failure does not publish bytes");

    return ok;
}

bool test_partial_records_are_invisible()
{
    bool ok = true;

    term::Terminal_history_ring ring({512U, 512U});
    ok &= check(ring.status() == term::Terminal_history_ring_status::OK,
        "partial-write test ring initializes");

    {
        term::Terminal_history_ring_record_reservation reservation =
            ring.reserve_record(7U);
        ok &= check(reservation.status() == term::Terminal_history_ring_status::OK,
            "pending reservation succeeds");
        fill_payload(reservation, "pending");
        ok &= check(ring.head_byte_sequence() == 0U,
            "pending reservation does not advance published head");
        ok &= check(ring.read_record(reservation.byte_sequence()).status() ==
                term::Terminal_history_ring_status::OUT_OF_LIVE_RANGE,
            "uncommitted record is invisible to readers");
    }

    term::Terminal_history_ring_record_reservation replacement =
        ring.reserve_record(9U);
    ok &= check(replacement.status() == term::Terminal_history_ring_status::OK,
        "released pending reservation allows a later reservation");
    ok &= check(replacement.byte_sequence() == 0U,
        "cancelled reservation leaves the absolute byte sequence unpublished");
    fill_payload(replacement, "committed");

    const term::terminal_history_ring_commit_result_t commit =
        ring.commit(std::move(replacement));
    ok &= check(commit.status == term::Terminal_history_ring_status::OK,
        "replacement reservation commits");

    const term::Terminal_history_ring_read_scope read =
        ring.read_record(commit.byte_sequence);
    ok &= check(read.status() == term::Terminal_history_ring_status::OK &&
            payload_equal(read, "committed"),
        "committed replacement payload becomes visible");

    return ok;
}

bool test_wrap_traversal_tail_boundaries()
{
    bool ok = true;

    term::Terminal_history_ring ring({512U, 512U});
    ok &= check(ring.status() == term::Terminal_history_ring_status::OK,
        "wrap traversal test ring initializes");

    std::vector<term::terminal_history_ring_commit_result_t> commits;
    commits.reserve(10U);
    commits.push_back(append_record(ring, "record-00-abcdef"));
    commits.push_back(append_record(ring, "record-01-abcdef"));
    commits.push_back(append_record(ring, "record-02-abcdef"));
    commits.push_back(append_record(ring, "record-03-abcdef"));
    commits.push_back(append_record(ring, "record-04-abcdef"));
    commits.push_back(append_record(ring, "record-05-abcdef"));
    commits.push_back(append_record(ring, "record-06-abcdef"));
    commits.push_back(append_record(ring, "record-07-abcdef"));
    commits.push_back(append_record(ring, "record-08-abcdef"));
    commits.push_back(append_record(ring, "record-09-abcdef"));

    for (const term::terminal_history_ring_commit_result_t& commit : commits) {
        ok &= check(commit.status == term::Terminal_history_ring_status::OK,
            "wrap traversal fixture commit succeeds");
    }

    const term::terminal_history_ring_commit_result_t& wrap_commit = commits.back();
    ok &= check(
        wrap_commit.byte_sequence % ring.capacity_bytes() + wrap_commit.record_bytes >
            ring.capacity_bytes(),
        "fixture commits a record that physically wraps");
    ok &= check(wrap_commit.tail_advanced,
        "wrapped commit advances tail when live bytes exceed capacity");
    ok &= check(
        ring.oldest_live_byte_sequence() ==
            commits.front().byte_sequence + commits.front().record_bytes,
        "tail advances to the next committed record boundary");

    ok &= check(ring.read_record(commits.front().byte_sequence).status() ==
            term::Terminal_history_ring_status::OUT_OF_LIVE_RANGE,
        "evicted record falls outside live byte bounds");

    const term::Terminal_history_ring_read_scope wrapped_read =
        ring.read_record(wrap_commit.byte_sequence);
    ok &= check(wrapped_read.status() == term::Terminal_history_ring_status::OK &&
            payload_equal(wrapped_read, "record-09-abcdef"),
        "two-span read reconstructs a physically wrapped record");

    return ok;
}

bool test_read_scope_boundaries()
{
    bool ok = true;

    term::Terminal_history_ring ring({512U, 512U});
    const term::terminal_history_ring_commit_result_t commit =
        append_record(ring, "scope");
    ok &= check(commit.status == term::Terminal_history_ring_status::OK,
        "read-scope fixture commit succeeds");
    ok &= check(ring.read_record(commit.byte_sequence + 1U).status() ==
            term::Terminal_history_ring_status::NOT_RECORD_BOUNDARY,
        "read scope rejects live byte positions that are not record boundaries");
    ok &= check(ring.read_record(commit.head_byte_sequence).status() ==
            term::Terminal_history_ring_status::OUT_OF_LIVE_RANGE,
        "read scope treats the published head as outside the live range");

    return ok;
}

bool test_explicit_discard_advances_live_bounds()
{
    bool ok = true;

    term::Terminal_history_ring ring({512U, 512U});
    const term::terminal_history_ring_commit_result_t first =
        append_record(ring, "first");
    const term::terminal_history_ring_commit_result_t second =
        append_record(ring, "second");
    const term::terminal_history_ring_commit_result_t third =
        append_record(ring, "third");
    ok &= check(first.status  == term::Terminal_history_ring_status::OK &&
            second.status == term::Terminal_history_ring_status::OK &&
            third.status  == term::Terminal_history_ring_status::OK,
        "discard fixture commits three records");

    const term::terminal_history_ring_discard_result_t discard =
        ring.discard_oldest_records(2U);
    ok &= check(discard.status == term::Terminal_history_ring_status::OK &&
            discard.discarded_records == 2U &&
            discard.oldest_live_byte_sequence == third.byte_sequence,
        "explicit discard advances the live byte lower bound");
    ok &= check(ring.read_record(first.byte_sequence).status() ==
            term::Terminal_history_ring_status::OUT_OF_LIVE_RANGE &&
            ring.read_record(second.byte_sequence).status() ==
                term::Terminal_history_ring_status::OUT_OF_LIVE_RANGE,
        "explicitly discarded records fail live-range validation");

    const term::Terminal_history_ring_read_scope live_read =
        ring.read_record(third.byte_sequence);
    ok &= check(live_read.status() == term::Terminal_history_ring_status::OK &&
            payload_equal(live_read, "third"),
        "surviving record remains readable after explicit discard");

    return ok;
}

bool test_clear_preserves_capacity_and_sequence()
{
    bool ok = true;

    term::Terminal_history_ring ring({512U, 512U});
    const term::terminal_history_ring_commit_result_t first =
        append_record(ring, "first");
    const std::uint64_t capacity = ring.capacity_bytes();
    ring.clear();

    ok &= check(ring.capacity_bytes() == capacity,
        "ring clear preserves allocated capacity");
    ok &= check(
        ring.oldest_live_byte_sequence() == first.head_byte_sequence &&
            ring.head_byte_sequence() == first.head_byte_sequence,
        "ring clear publishes an empty live range at the existing head");
    ok &= check(
        ring.read_record(first.byte_sequence).status() ==
            term::Terminal_history_ring_status::OUT_OF_LIVE_RANGE,
        "ring clear invalidates prior records");

    const term::terminal_history_ring_commit_result_t second =
        append_record(ring, "second");
    ok &= check(
        second.status == term::Terminal_history_ring_status::OK &&
            second.byte_sequence == first.head_byte_sequence,
        "ring append after clear continues the byte sequence");

    return ok;
}

bool test_descriptor_allocation_failure_does_not_start_commit()
{
    bool ok = true;

    term::Terminal_history_ring ring({512U, 512U});
    constexpr std::string_view records[] = {
        "record-00-abcdef",
        "record-01-abcdef",
        "record-02-abcdef",
        "record-03-abcdef",
        "record-04-abcdef",
        "record-05-abcdef",
        "record-06-abcdef",
        "record-07-abcdef",
        "record-08-abcdef",
    };
    std::vector<term::terminal_history_ring_commit_result_t> commits;
    commits.reserve(std::size(records));
    for (std::string_view record : records) {
        commits.push_back(append_record(ring, record));
        ok &= check(commits.back().status == term::Terminal_history_ring_status::OK,
            "ring allocation-failure fixture commit succeeds");
    }

    const std::uint64_t tail_before_failure = ring.oldest_live_byte_sequence();
    const std::uint64_t head_before_failure = ring.head_byte_sequence();

    bool allocation_failed = false;
    try {
        term::Terminal_history_ring_record_reservation reservation =
            ring.reserve_record(std::string_view("record-09-abcdef").size());
        fill_payload(reservation, "record-09-abcdef");
        ring.fail_next_record_descriptor_allocation_for_testing();
        (void)ring.commit(std::move(reservation));
    }
    catch (const std::bad_alloc&) {
        allocation_failed = true;
    }
    ok &= check(allocation_failed,
        "ring descriptor allocation failure propagates before commit");

    ok &= check(
        ring.oldest_live_byte_sequence() == tail_before_failure &&
            ring.head_byte_sequence() == head_before_failure,
        "ring descriptor allocation failure preserves published byte bounds");

    for (std::size_t index = 0U; index < commits.size(); ++index) {
        const term::Terminal_history_ring_read_scope read_after_failure =
            ring.read_record(commits[index].byte_sequence);
        ok &= check(
            read_after_failure.ok() &&
                read_after_failure.record_bytes() == commits[index].record_bytes &&
                payload_equal(read_after_failure, records[index]),
            "ring descriptor allocation failure preserves every live record");
    }

    const term::terminal_history_ring_commit_result_t retry =
        append_record(ring, "record-09-abcdef");
    ok &= check(retry.status == term::Terminal_history_ring_status::OK &&
            retry.tail_advanced,
        "ring descriptor allocation failure releases the reservation for a later commit");

    return ok;
}

}

int main()
{
    bool ok = true;
    ok &= test_capacity_alignment_and_one_octile_limit();
    ok &= test_partial_records_are_invisible();
    ok &= test_wrap_traversal_tail_boundaries();
    ok &= test_read_scope_boundaries();
    ok &= test_explicit_discard_advances_live_bounds();
    ok &= test_clear_preserves_capacity_and_sequence();
    ok &= test_descriptor_allocation_failure_does_not_start_commit();
    return ok ? 0 : 1;
}
