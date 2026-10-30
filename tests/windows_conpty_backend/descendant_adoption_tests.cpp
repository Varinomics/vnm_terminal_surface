#include "../../src/windows_descendant_adoption.h"
#include "../helpers/test_check.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace term = vnm_terminal::internal;
using vnm_terminal::test_helpers::check;

namespace {

struct process_instance_t
{
    std::uint32_t pid;
    std::uint64_t created;
    bool running = true;
    std::optional<bool> owned = false;
};

class Process_handle
{
public:
    explicit Process_handle(std::shared_ptr<process_instance_t> instance)
    :
        m_instance(std::move(instance))
    {}

    Process_handle(const Process_handle&) = delete;
    Process_handle& operator=(const Process_handle&) = delete;
    Process_handle(Process_handle&&) = default;
    Process_handle& operator=(Process_handle&&) = default;

    const process_instance_t* operator->() const { return m_instance.get(); }
    const std::shared_ptr<process_instance_t>& instance() const { return m_instance; }

private:
    std::shared_ptr<process_instance_t> m_instance;
};

class Injected_source
{
public:
    using Process = Process_handle;
    using Candidates = term::Windows_descendant_candidates<Process>;

    explicit Injected_source(std::uint32_t root = 100)
    {
        processes[root]       = std::make_shared<process_instance_t>(process_instance_t{root, 10, true, true});
        processes[root + 100] = std::make_shared<process_instance_t>(process_instance_t{root + 100, 20});
        parents = {{root, 1}, {root + 100, root}};
    }

    std::optional<std::set<std::uint32_t>> begin_scan()
    {
        if (!enabled || fail_members) {
            return std::nullopt;
        }
        std::set<std::uint32_t> members;
        for (const auto& [pid, process] : processes) {
            if (process->running && process->owned == std::optional<bool>(true)) {
                members.insert(pid);
            }
        }
        return members;
    }

    Candidates prepare_scan(const std::set<std::uint32_t>& members, const term::Windows_process_parents& first)
    {
        return enabled ? term::prepare_windows_descendants(*this, members, first) : Candidates{};
    }

    void finish_scan(const Candidates& candidates, const term::Windows_process_parents& second)
    {
        if (enabled) {
            term::adopt_windows_descendants(*this, candidates, second);
        }
    }

    void scan_failed()
    {
        if (enabled) {
            ++scan_failures;
        }
    }

    std::optional<term::Windows_process_parents> snapshot()
    {
        ++snapshots;
        if (snapshots % 2 == 0 && during_second_snapshot) {
            during_second_snapshot();
        }
        if ((fail_first_snapshot  && snapshots % 2 == 1) ||
            (fail_second_snapshot && snapshots % 2 == 0))
        {
            return std::nullopt;
        }
        return parents;
    }

    std::optional<Process> open_parent(std::uint32_t pid)
    {
        if (before_parent_open) {
            before_parent_open();
        }
        return Process(processes.at(pid));
    }

    std::optional<Process> open_child(std::uint32_t pid)
    {
        if (before_child_open) {
            before_child_open();
        }
        return Process(processes.at(pid));
    }

    std::optional<bool> owned_membership(const Process& process) { return process->owned; }
    bool running(const Process& process) { return process->running; }
    std::optional<std::uint64_t> creation_time(const Process& process) { return process->created; }

    void adopt(const Process& process)
    {
        adopted.push_back(process.instance());
        process.instance()->owned = true;
        if (after_adoption) {
            after_adoption();
        }
    }

    std::map<std::uint32_t, std::shared_ptr<process_instance_t>> processes;
    term::Windows_process_parents parents;
    std::vector<std::shared_ptr<process_instance_t>> adopted;
    std::function<void()> before_parent_open;
    std::function<void()> before_child_open;
    std::function<void()> during_second_snapshot;
    std::function<void()> after_adoption;
    int snapshots = 0;
    int scan_failures = 0;
    bool fail_first_snapshot = false;
    bool fail_second_snapshot = false;
    bool fail_members = false;
    bool enabled = true;
};

void scan(Injected_source& source)
{
    const std::vector<Injected_source*> families{&source};
    term::scan_windows_descendant_families<Injected_source>(families, source);
}

bool test_reused_child_pid()
{
    Injected_source source;
    source.before_child_open = [&] {
        source.processes[200]->running = false;
        source.processes[200] = std::make_shared<process_instance_t>(process_instance_t{200, 30});
        source.parents[200] = 300;
    };
    scan(source);
    return check(source.adopted.empty(), "S2 rejects a reused child PID with another creator");
}

bool test_reused_parent_pid()
{
    Injected_source source;
    source.before_parent_open = [&] {
        source.processes[100]->running = false;
        // A replacement can itself belong to our Job. Membership alone must
        // not turn the old PID's existing child into its own child.
        source.processes[100] = std::make_shared<process_instance_t>(process_instance_t{100, 30, true, true});
    };
    scan(source);
    return check(source.adopted.empty(), "strict creation order rejects a reused parent PID");
}

bool test_retained_instances()
{
    bool ok = true;
    for (const std::uint32_t pid : {100U, 200U}) {
        Injected_source source;
        source.during_second_snapshot = [&] {
            source.processes[pid]->running = false;
            source.processes[pid] = std::make_shared<process_instance_t>(
                process_instance_t{pid, 30, true, pid == 100});
        };
        scan(source);
        ok &= check(source.adopted.empty(), "post-S2 liveness checks use retained handles, not reopened PIDs");
    }
    return ok;
}

bool test_unproven_observations()
{
    bool ok = true;
    for (int failure = 0; failure != 5; ++failure) {
        Injected_source source;
        switch (failure) {
            case 0: source.processes[200]->created = 10;         break;
            case 1: source.processes[100]->owned = std::nullopt; break;
            case 2: source.processes[200]->owned = std::nullopt; break;
            case 3: source.fail_second_snapshot = true;         break;
            case 4: source.fail_members = true;                 break;
        }
        scan(source);
        ok &= check(source.adopted.empty(), "equal creation times and failed observations cannot authorize adoption");
    }
    return ok;
}

bool test_repeated_adoption_and_escape()
{
    Injected_source source;
    scan(source);
    bool ok = check(source.adopted.size() == 1, "a proven escaped child is adopted");
    scan(source);
    ok &= check(source.adopted.size() == 1, "later scans exclude every previously adopted Job");
    source.processes[300] = std::make_shared<process_instance_t>(process_instance_t{300, 30});
    source.parents[300] = 200;
    scan(source);
    ok &= check(source.adopted.size() == 2 && source.adopted.back()->pid == 300,
        "a descendant escaping from an adopted process gets independent custody");
    return ok;
}

bool test_membership_changes_during_scan()
{
    Injected_source source;
    source.processes[300] = std::make_shared<process_instance_t>(process_instance_t{300, 30});
    source.parents[300] = 100;
    source.after_adoption = [&] { source.processes[300]->owned = true; };
    scan(source);
    return check(source.adopted.size() == 1,
        "membership is refreshed against Jobs created earlier in the same pass");
}

bool test_shared_snapshots()
{
    Injected_source first;
    Injected_source second(300);
    first.parents.insert(second.parents.begin(), second.parents.end());
    const std::vector<Injected_source*> families{&first, &second};
    term::scan_windows_descendant_families<Injected_source>(families, first);
    bool ok = check(first.snapshots == 2 && second.snapshots == 0,
        "all candidate families share exactly one S1 and one S2");
    ok &= check(first.adopted.size() == 1 && first.adopted.front()->pid == 200 &&
        second.adopted.size() == 1 && second.adopted.front()->pid == 400,
        "shared snapshots preserve each family's membership and adoption boundary");
    const int snapshots_before = first.snapshots;
    term::scan_windows_descendant_families<Injected_source>(families, first);
    ok &= check(first.snapshots == snapshots_before + 1,
        "a candidate-free pass takes only one shared snapshot for every family");
    first.enabled  = false;
    second.enabled = false;
    term::scan_windows_descendant_families<Injected_source>(families, first);
    ok &= check(first.snapshots == snapshots_before + 1, "inactive families require no system snapshot");
    return ok;
}

bool test_stop_between_shared_snapshots()
{
    Injected_source first;
    Injected_source second(300);
    first.parents.insert(second.parents.begin(), second.parents.end());
    first.during_second_snapshot = [&] {
        first.enabled = false;
        // Both candidate handles must already be retained when shared S2
        // starts. Replacing this live PID cannot change that retained instance.
        second.processes[400]->running = false;
        second.processes[400] = std::make_shared<process_instance_t>(process_instance_t{400, 30});
    };
    const std::vector<Injected_source*> families{&first, &second};
    term::scan_windows_descendant_families<Injected_source>(families, first);
    return check(first.adopted.empty() && second.adopted.empty(),
        "stop prevents post-S2 admission and other families still validate their retained instances");
}

bool test_failed_shared_snapshot_reports_affected_families()
{
    // CPU accounting documents observed-family scope once discovery fails.
    // Failure notification must reach each affected family even without adoption.
    bool ok = true;
    for (const bool fail_first : {true, false}) {
        Injected_source first;
        Injected_source second(300);
        Injected_source candidate_free(500);
        Injected_source disabled(700);
        candidate_free.processes[600]->owned = true;
        disabled.enabled = false;
        first.parents.insert(second.parents.begin(), second.parents.end());
        first.parents.insert(candidate_free.parents.begin(), candidate_free.parents.end());
        first.parents.insert(disabled.parents.begin(), disabled.parents.end());
        first.fail_first_snapshot  =  fail_first;
        first.fail_second_snapshot = !fail_first;
        const std::vector<Injected_source*> families{&first, &second, &candidate_free, &disabled};

        term::scan_windows_descendant_families<Injected_source>(families, first);

        ok &= check(first.scan_failures == 1 && second.scan_failures == 1,
            fail_first ? "failed S1 marks every active family with unknown descendants"
                       : "failed S2 marks every family awaiting candidate validation");
        ok &= check(candidate_free.scan_failures == (fail_first ? 1 : 0),
            "a family without candidates needs S1 but does not depend on S2");
        ok &= check(disabled.scan_failures == 0, "disabled families are not discovery failures");
        ok &= check(first.adopted.empty() && second.adopted.empty(),
            "a failed shared snapshot cannot authorize adoption");
        ok &= check(first.snapshots == (fail_first ? 1 : 2),
            "failure reporting does not add snapshots or retry a failed pass");
    }
    return ok;
}

}

int main()
{
    // The oracle is the reviewed two-snapshot identity proof, not observations
    // copied from a live scan. PID reuse deliberately occurs between steps.
    bool ok = test_reused_child_pid();
    ok &= test_reused_parent_pid();
    ok &= test_retained_instances();
    ok &= test_unproven_observations();
    ok &= test_repeated_adoption_and_escape();
    ok &= test_membership_changes_during_scan();
    ok &= test_shared_snapshots();
    ok &= test_stop_between_shared_snapshots();
    ok &= test_failed_shared_snapshot_reports_affected_families();
    return ok ? 0 : 1;
}
