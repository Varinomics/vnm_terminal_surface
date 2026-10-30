#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace vnm_terminal::internal {

using Windows_process_parents = std::map<std::uint32_t, std::uint32_t>;

template<class Process>
struct Windows_descendant_candidate
{
    std::uint32_t pid;
    std::uint32_t parent_pid;
    Process parent;
    Process child;
};

template<class Process>
using Windows_descendant_candidates = std::vector<Windows_descendant_candidate<Process>>;

// Source binds every query and assignment to a retained process handle. A PID
// from either snapshot is only a lookup hint. open_parent returns a previously
// proven member handle, including an exited member retained across discovery.
// Establish lifetime order before S2 so a child PID reused during S2 cannot
// turn a newly created process into a candidate from the first snapshot.
template<class Source>
Windows_descendant_candidates<typename Source::Process> prepare_windows_descendants(
    Source& source,
    const std::set<std::uint32_t>& members,
    const Windows_process_parents& first)
{
    Windows_descendant_candidates<typename Source::Process> candidates;
    for (const auto& [pid, parent_pid] : first) {
        if (!members.contains(parent_pid) || members.contains(pid)) {
            continue;
        }
        auto parent = source.open_parent(parent_pid);
        if (!parent) {
            continue;
        }
        auto child = source.open_child(pid);
        if (child && source.parent_precedes_child(*parent, *child)) {
            candidates.push_back({pid, parent_pid, std::move(*parent), std::move(*child)});
        }
    }
    return candidates;
}

template<class Source>
void adopt_windows_descendants(
    Source& source,
    const Windows_descendant_candidates<typename Source::Process>& candidates,
    const Windows_process_parents& second)
{
    for (const auto& candidate : candidates) {
        const auto entry = second.find(candidate.pid);
        if (entry == second.end() || entry->second != candidate.parent_pid ||
            !source.running(candidate.child) ||
            !source.parent_precedes_child(candidate.parent, candidate.child))
        {
            continue;
        }
        // Query failure is unproven, not evidence of nonmembership. Include
        // Jobs adopted earlier in this same pass to prevent owned Job nesting.
        if (source.owned_membership(candidate.child) == std::optional<bool>(false)) {
            source.adopt(candidate.child);
        }
    }
}

// Family ownership is locked separately for preparation and commit. Candidate
// handles span the shared second snapshot, while terminal cleanup can disable
// a family between phases; finish_scan must refuse adoption after that point.
template<class Family, class Families, class Snapshots>
void scan_windows_descendant_families(const Families& families, Snapshots& snapshots)
{
    class Scan
    {
    public:
        Scan(typename Families::value_type family, std::set<std::uint32_t> members)
        :
            m_family(std::move(family)),
            m_members(std::move(members))
        {}

        // Process handles cannot be copied. Relocation must move the scan
        // even when the member set's move constructor can allocate and throw.
        Scan(const Scan&) = delete;
        Scan& operator=(const Scan&) = delete;
        Scan(Scan&&) = default;
        Scan& operator=(Scan&&) = default;

        typename Families::value_type m_family;
        std::set<std::uint32_t> m_members;
        Windows_descendant_candidates<typename Family::Process> m_candidates;
    };
    std::vector<Scan> scans;
    scans.reserve(families.size());
    for (const auto& family : families) {
        auto members = family->begin_scan();
        if (members) {
            scans.emplace_back(family, std::move(*members));
        }
    }
    if (scans.empty()) {
        return;
    }
    const auto first = snapshots.snapshot();
    if (!first) {
        for (const auto& scan : scans) {
            scan.m_family->scan_failed();
        }
        return;
    }
    bool has_candidates = false;
    for (auto& scan : scans) {
        scan.m_candidates = scan.m_family->prepare_scan(scan.m_members, *first);
        has_candidates = has_candidates || !scan.m_candidates.empty();
    }
    if (!has_candidates) {
        for (const auto& scan : scans) {
            scan.m_family->complete_scan();
        }
        return;
    }
    const auto second = snapshots.snapshot();
    if (!second) {
        for (const auto& scan : scans) {
            if (!scan.m_candidates.empty()) {
                scan.m_family->scan_failed();
            }
            else {
                scan.m_family->complete_scan();
            }
        }
        return;
    }
    for (const auto& scan : scans) {
        if (!scan.m_candidates.empty()) {
            scan.m_family->finish_scan(scan.m_candidates, *second);
        }
        scan.m_family->complete_scan();
    }
}

} // namespace vnm_terminal::internal
