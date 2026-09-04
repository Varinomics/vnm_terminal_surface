#include "vnm_terminal/internal/terminal_search.h"

#include <QByteArray>
#include <QThreadPool>
#include <Qt>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <utility>

namespace vnm_terminal::internal {

namespace {

struct search_match_span_t
{
    int first_column = 0;
    int column_count = 0;
};

struct search_match_row_key_t
{
    bool          active_grid = false;
    std::uint64_t position = 0U;

    friend bool operator==(
        const search_match_row_key_t&,
        const search_match_row_key_t&) = default;

    friend bool operator<(
        const search_match_row_key_t& left,
        const search_match_row_key_t& right)
    {
        return left.active_grid != right.active_grid
            ? left.active_grid < right.active_grid
            : left.position < right.position;
    }
};

struct Search_match_row
{
    terminal_history_handle_t        history_handle;
    std::vector<search_match_span_t> spans;
    int                              first_match_ordinal_in_retained_line = 0;
};

using Search_match_rows = std::map<search_match_row_key_t, Search_match_row>;

struct Search_corpus_row
{
    std::uint64_t            retained_line_id = 0U;
    std::uint64_t            content_generation = 0U;
    QByteArray               identity_latin1_text;
    std::unique_ptr<Terminal_search_row_text>
                              complex_text;
    std::int32_t             source_width = 0;
};

struct Search_query_text
{
    QStringView utf16;
    QByteArray  latin1;
    bool        latin1_exact = false;
};

struct Search_corpus
{
    std::deque<Search_corpus_row> primary_retained_rows;
    std::vector<Search_corpus_row> primary_active_rows;
    std::vector<Search_corpus_row> alternate_active_rows;
    std::uint64_t                           first_retained_ordinal = 0U;
    std::uint64_t                           end_retained_ordinal = 0U;
    terminal_search_source_identity_t       identity;
    bool                                    initialized = false;
};

Search_corpus_row make_search_corpus_row(Terminal_search_source_row& source)
{
    Search_corpus_row row;
    row.retained_line_id  = source.retained_line_id;
    row.content_generation = source.content_generation;
    row.source_width       = source.text.source_width;

    const bool latin1_identity = source.text.identity_spans &&
        std::all_of(
            source.text.units.begin(),
            source.text.units.end(),
            [](char16_t code_unit) { return code_unit <= 0x00ffU; });
    if (!latin1_identity) {
        row.complex_text =
            std::make_unique<Terminal_search_row_text>(std::move(source.text));
        return row;
    }

    row.identity_latin1_text.reserve(
        static_cast<qsizetype>(source.text.units.size()));
    for (char16_t code_unit : source.text.units) {
        row.identity_latin1_text.push_back(static_cast<char>(code_unit));
    }
    return row;
}

Search_query_text make_search_query_text(const QString& query)
{
    Search_query_text text;
    text.utf16 = QStringView(query);
    text.latin1_exact = std::all_of(
        query.cbegin(),
        query.cend(),
        [](QChar code_unit) { return code_unit.unicode() <= 0x00ffU; });
    if (text.latin1_exact) {
        text.latin1 = query.toLatin1();
    }
    return text;
}

std::size_t searchable_unit_count(
    const Terminal_search_row_text& row,
    int                             column_count)
{
    if (column_count >= row.source_width) {
        return row.units.size();
    }

    if (row.identity_spans) {
        return std::min(
            row.units.size(),
            static_cast<std::size_t>(std::max(0, column_count)));
    }

    std::size_t count = 0U;
    while (count < row.spans.size() &&
           row.spans[count].end_column <= column_count)
    {
        ++count;
    }
    return count;
}

std::optional<std::vector<search_match_span_t>> search_row(
    const Search_corpus_row&          row,
    const Search_query_text&          query,
    int                               column_count,
    const std::atomic<std::uint64_t>& desired_work_generation,
    std::uint64_t                     work_generation)
{
    if (desired_work_generation.load(std::memory_order_acquire) != work_generation) {
        return std::nullopt;
    }

    std::vector<search_match_span_t> matches;
    if (row.complex_text == nullptr) {
        const std::size_t unit_count = column_count >= row.source_width
            ? static_cast<std::size_t>(row.identity_latin1_text.size())
            : std::min(
                static_cast<std::size_t>(row.identity_latin1_text.size()),
                static_cast<std::size_t>(std::max(0, column_count)));
        if (!query.latin1_exact || query.latin1.isEmpty() ||
            static_cast<std::size_t>(query.latin1.size()) > unit_count)
        {
            return matches;
        }

        const char* const text_begin = row.identity_latin1_text.constData();
        const char* const text_end   = text_begin + unit_count;
        const char* search_from      = text_begin;
        while (search_from <= text_end - query.latin1.size()) {
            const char* const found = std::search(
                search_from,
                text_end,
                query.latin1.cbegin(),
                query.latin1.cend());
            if (found == text_end) {
                break;
            }
            const int first_column = static_cast<int>(found - text_begin);
            matches.push_back({
                first_column,
                static_cast<int>(query.latin1.size()),
            });
            search_from = found + std::max<qsizetype>(1, query.latin1.size());
        }
        return matches;
    }

    const Terminal_search_row_text& complex = *row.complex_text;
    const std::size_t unit_count = searchable_unit_count(complex, column_count);
    const QStringView text(
        complex.units.data(),
        static_cast<qsizetype>(unit_count));
    if (query.utf16.isEmpty() || query.utf16.size() > text.size()) {
        return matches;
    }

    qsizetype search_from = 0;
    while (search_from <= text.size() - query.utf16.size()) {
        const qsizetype found =
            text.indexOf(query.utf16, search_from, Qt::CaseSensitive);
        if (found < 0) {
            break;
        }

        const qsizetype last = found + query.utf16.size() - 1;
        const int first_column = complex.identity_spans
            ? static_cast<int>(found)
            : complex.spans[static_cast<std::size_t>(found)].first_column;
        const int end_column = complex.identity_spans
            ? static_cast<int>(last) + 1
            : complex.spans[static_cast<std::size_t>(last)].end_column;
        matches.push_back({first_column, std::max(1, end_column - first_column)});
        search_from = found + std::max<qsizetype>(1, query.utf16.size());
    }
    return matches;
}

}

struct Terminal_search_controller::Shared_state
    : std::enable_shared_from_this<Terminal_search_controller::Shared_state>
{
    struct Desired_work
    {
        QString                           query;
        terminal_search_source_identity_t source_identity;
        std::uint64_t                     query_generation = 0U;
        std::uint64_t                     source_revision = 0U;
        std::uint64_t                     work_generation = 0U;
        std::int64_t                      preferred_public_row = 0;
        bool                              source_available = false;
    };

    struct Corpus_changes
    {
        std::set<std::uint64_t> changed_retained_ordinals;
        std::set<int>           changed_active_rows;
        bool                    rescan_all = false;
    };

    struct Result
    {
        Search_match_rows                     matches;
        terminal_search_source_identity_t     source_identity;
        QString                               query;
        std::optional<search_match_row_key_t> current_row;
        int                                   current_row_match_ordinal = 0;
        int                                   match_count = 0;
        int                                   current_match_number = 0;
        std::uint64_t                         first_retained_ordinal = 0U;
        std::uint64_t                         end_retained_ordinal = 0U;
        std::uint64_t                         query_generation = 0U;
        std::uint64_t                         source_revision = 0U;
        bool                                  completion_ready = false;
    };

    explicit Shared_state(std::function<void()> notifier)
    :
        completion_notifier(std::move(notifier))
    {}

    void request(
        Desired_work                             desired,
        std::optional<Terminal_search_source_update> update = std::nullopt)
    {
        bool schedule = false;
        {
            std::lock_guard<std::mutex> lock(command_mutex);
            if (closed) {
                return;
            }
            desired.work_generation =
                desired_work_generation.fetch_add(1U, std::memory_order_acq_rel) + 1U;
            desired_work = std::move(desired);
            if (update.has_value()) {
                pending_source_updates.push_back(std::move(*update));
            }
            if (!work_scheduled) {
                work_scheduled = true;
                schedule = true;
            }
        }
        if (schedule) {
            const std::shared_ptr<Shared_state> self = shared_from_this();
            QThreadPool::globalInstance()->start([self] { self->drain(); });
        }
    }

    void close()
    {
        {
            std::lock_guard<std::mutex> lock(command_mutex);
            closed = true;
            pending_source_updates.clear();
            completion_notifier = {};
            desired_work_generation.fetch_add(1U, std::memory_order_acq_rel);
        }
        idle.notify_all();
    }

    bool wait_until_idle(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(command_mutex);
        return idle.wait_for(lock, timeout, [this] { return !work_scheduled || closed; });
    }

    std::optional<terminal_search_match_t> current_match() const
    {
        std::lock_guard<std::mutex> lock(result_mutex);
        return current_match_locked();
    }

    std::optional<std::vector<terminal_history_handle_t>> source_row_handles(
        const terminal_search_source_identity_t& source_identity,
        std::int64_t                             first_public_row,
        int                                      row_count) const
    {
        if (first_public_row < 0 || row_count <= 0) {
            return std::nullopt;
        }

        std::lock_guard<std::mutex> lock(corpus_mutex);
        if (!corpus.initialized || corpus.identity != source_identity) {
            return std::nullopt;
        }

        const std::size_t retained_rows = corpus.identity.active_buffer ==
                Terminal_buffer_id::PRIMARY
            ? corpus.primary_retained_rows.size()
            : 0U;
        const std::vector<Search_corpus_row>& active_rows =
            corpus.identity.active_buffer == Terminal_buffer_id::PRIMARY
                ? corpus.primary_active_rows
                : corpus.alternate_active_rows;
        const std::uint64_t first = static_cast<std::uint64_t>(first_public_row);
        const std::uint64_t count = static_cast<std::uint64_t>(row_count);
        const std::uint64_t total =
            static_cast<std::uint64_t>(retained_rows + active_rows.size());
        if (first > total || count > total - first) {
            return std::nullopt;
        }

        std::vector<terminal_history_handle_t> handles;
        handles.reserve(static_cast<std::size_t>(row_count));
        for (std::uint64_t offset = 0U; offset < count; ++offset) {
            const std::uint64_t public_row = first + offset;
            const Search_corpus_row& row = public_row < retained_rows
                ? corpus.primary_retained_rows[static_cast<std::size_t>(public_row)]
                : active_rows[static_cast<std::size_t>(public_row - retained_rows)];
            handles.push_back(terminal_history_handle_from_retained_identity(
                row.retained_line_id,
                row.content_generation));
        }
        return handles;
    }

    terminal_search_result_state_t committed_result_state() const
    {
        std::lock_guard<std::mutex> lock(result_mutex);
        return {
            result.matches.empty()
                ? Terminal_search_result_status::NO_MATCH
                : Terminal_search_result_status::MATCH,
            result.match_count,
            result.current_match_number,
        };
    }

    bool completion_matches(
        std::uint64_t                           query_generation,
        std::uint64_t                           source_revision,
        const terminal_search_source_identity_t& source_identity)
    {
        std::lock_guard<std::mutex> lock(result_mutex);
        if (!result.completion_ready) {
            return false;
        }
        result.completion_ready = false;
        return
            result.query_generation == query_generation &&
            result.source_revision  == source_revision  &&
            result.source_identity  == source_identity;
    }

    bool select_next()
    {
        std::lock_guard<std::mutex> lock(result_mutex);
        if (result.matches.empty()) {
            return false;
        }

        if (!result.current_row.has_value()) {
            result.current_row = result.matches.begin()->first;
            result.current_row_match_ordinal = 0;
            result.current_match_number = 1;
            return true;
        }

        auto row = result.matches.find(*result.current_row);
        if (row == result.matches.end()) {
            result.current_row = result.matches.begin()->first;
            result.current_row_match_ordinal = 0;
            result.current_match_number = 1;
            return true;
        }
        if (result.current_row_match_ordinal + 1 <
            static_cast<int>(row->second.spans.size()))
        {
            ++result.current_row_match_ordinal;
        }
        else {
            ++row;
            if (row == result.matches.end()) {
                row = result.matches.begin();
            }
            result.current_row = row->first;
            result.current_row_match_ordinal = 0;
        }
        result.current_match_number = result.current_match_number < result.match_count
            ? result.current_match_number + 1
            : 1;
        return true;
    }

    bool select_previous()
    {
        std::lock_guard<std::mutex> lock(result_mutex);
        if (result.matches.empty()) {
            return false;
        }

        if (!result.current_row.has_value()) {
            auto row = std::prev(result.matches.end());
            result.current_row = row->first;
            result.current_row_match_ordinal =
                static_cast<int>(row->second.spans.size()) - 1;
            result.current_match_number = result.match_count;
            return true;
        }

        auto row = result.matches.find(*result.current_row);
        if (row == result.matches.end()) {
            row = std::prev(result.matches.end());
            result.current_row = row->first;
            result.current_row_match_ordinal =
                static_cast<int>(row->second.spans.size()) - 1;
            result.current_match_number = result.match_count;
            return true;
        }
        if (result.current_row_match_ordinal > 0) {
            --result.current_row_match_ordinal;
        }
        else {
            if (row == result.matches.begin()) {
                row = std::prev(result.matches.end());
            }
            else {
                --row;
            }
            result.current_row = row->first;
            result.current_row_match_ordinal =
                static_cast<int>(row->second.spans.size()) - 1;
        }
        result.current_match_number = result.current_match_number > 1
            ? result.current_match_number - 1
            : result.match_count;
        return true;
    }

    std::vector<Terminal_render_search_match_span> spans_for_snapshot(
        const Terminal_render_snapshot& snapshot,
        std::uint64_t                   active_buffer_epoch) const
    {
        std::lock_guard<std::mutex> lock(result_mutex);
        std::vector<Terminal_render_search_match_span> spans;
        if (result.matches.empty() ||
            result.source_identity.active_buffer != snapshot.viewport.active_buffer ||
            result.source_identity.active_buffer_epoch != active_buffer_epoch       ||
            result.source_identity.row_origin_generation !=
                snapshot.metadata.row_origin_generation                            ||
            !grid_sizes_match(result.source_identity.grid_size, snapshot.grid_size))
        {
            return spans;
        }

        const std::int64_t scrollback_rows = static_cast<std::int64_t>(
            result.end_retained_ordinal - result.first_retained_ordinal);
        for (int viewport_row = 0;
             viewport_row < static_cast<int>(snapshot.visible_line_provenance.size());
             ++viewport_row)
        {
            const Terminal_render_line_provenance& provenance =
                snapshot.visible_line_provenance[static_cast<std::size_t>(viewport_row)];
            const bool active_grid =
                snapshot.viewport.active_buffer == Terminal_buffer_id::ALTERNATE ||
                provenance.logical_row >= scrollback_rows;
            const std::uint64_t position = active_grid
                ? static_cast<std::uint64_t>(
                    snapshot.viewport.active_buffer == Terminal_buffer_id::ALTERNATE
                        ? provenance.logical_row
                        : provenance.logical_row - scrollback_rows)
                : result.first_retained_ordinal +
                    static_cast<std::uint64_t>(provenance.logical_row);
            const auto match_row = result.matches.find({active_grid, position});
            if (match_row == result.matches.end()) {
                continue;
            }

            const terminal_history_handle_t history_handle =
                terminal_history_handle_from_retained_identity(
                    provenance.retained_line_id,
                    provenance.content_generation);
            if (match_row->second.history_handle != history_handle) {
                continue;
            }
            for (int ordinal = 0;
                 ordinal < static_cast<int>(match_row->second.spans.size());
                 ++ordinal)
            {
                const search_match_span_t match =
                    match_row->second.spans[static_cast<std::size_t>(ordinal)];
                spans.push_back({
                    viewport_row,
                    match.first_column,
                    match.column_count,
                    result.current_row == match_row->first &&
                        result.current_row_match_ordinal == ordinal,
                });
            }
        }
        return spans;
    }

    mutable std::mutex                       command_mutex;
    std::condition_variable                  idle;
    std::vector<Terminal_search_source_update> pending_source_updates;
    Desired_work                             desired_work;
    std::atomic<std::uint64_t>               desired_work_generation{0U};
    std::function<void()>                    completion_notifier;
    bool                                     work_scheduled = false;
    bool                                     closed = false;

    Search_corpus                            corpus;
    mutable std::mutex                       corpus_mutex;
    QString                                  worker_query;
    std::uint64_t                            worker_query_generation = 0U;
    bool                                     worker_matches_valid = false;

    mutable std::mutex                       result_mutex;
    Result                                   result;

private:
    void discard_result_on_worker()
    {
        Result discarded;
        {
            std::lock_guard<std::mutex> lock(result_mutex);
            discarded = std::move(result);
            result = {};
        }
    }

    void drain()
    {
        std::optional<terminal_search_match_t> refresh_anchor;
        for (;;) {
            Desired_work desired;
            std::vector<Terminal_search_source_update> updates;
            {
                std::lock_guard<std::mutex> lock(command_mutex);
                if (closed) {
                    work_scheduled = false;
                    idle.notify_all();
                    return;
                }
                desired = desired_work;
                updates = std::move(pending_source_updates);
                pending_source_updates.clear();
            }

            {
                std::lock_guard<std::mutex> corpus_lock(corpus_mutex);
                const Corpus_changes changes = apply_updates(std::move(updates));
                if (desired.query.isEmpty() || !desired.source_available) {
                    refresh_anchor.reset();
                    worker_query = desired.query;
                    worker_query_generation = desired.query_generation;
                    worker_matches_valid = false;
                }
                else {
                    const bool refresh_anchor_matches_source =
                        !refresh_anchor.has_value() ||
                        (refresh_anchor->identity.buffer_id ==
                                desired.source_identity.active_buffer &&
                            refresh_anchor->identity.active_buffer_epoch ==
                                desired.source_identity.active_buffer_epoch);
                    if (worker_query != desired.query ||
                        worker_query_generation != desired.query_generation ||
                        !refresh_anchor_matches_source)
                    {
                        refresh_anchor.reset();
                    }
                    const bool full_scan =
                        !worker_matches_valid                    ||
                        worker_query != desired.query            ||
                        worker_query_generation != desired.query_generation ||
                        changes.rescan_all;
                    const bool committed = full_scan
                        ? run_full_scan(desired, refresh_anchor)
                        : run_incremental_scan(desired, changes, refresh_anchor);
                    if (!committed) {
                        worker_matches_valid = false;
                    }
                    else {
                        refresh_anchor.reset();
                    }
                }
            }
            if (desired.query.isEmpty() || !desired.source_available) {
                discard_result_on_worker();
            }

            std::function<void()> notify;
            bool done = false;
            {
                std::lock_guard<std::mutex> lock(command_mutex);
                if (closed) {
                    work_scheduled = false;
                    idle.notify_all();
                    return;
                }
                if (desired.work_generation ==
                        desired_work_generation.load(std::memory_order_acquire) &&
                    pending_source_updates.empty())
                {
                    work_scheduled = false;
                    if (!desired.query.isEmpty() && desired.source_available) {
                        notify = completion_notifier;
                    }
                    done = true;
                    idle.notify_all();
                }
            }
            if (notify) {
                notify();
            }
            if (done) {
                return;
            }
        }
    }

    Corpus_changes apply_updates(std::vector<Terminal_search_source_update> updates)
    {
        Corpus_changes changes;
        for (Terminal_search_source_update& update : updates) {
            changes.rescan_all = changes.rescan_all || update.rescan_all_rows;
            if (!corpus.initialized || update.reset_retained_rows) {
                corpus.primary_retained_rows.clear();
                corpus.first_retained_ordinal = update.first_retained_ordinal;
                corpus.end_retained_ordinal = update.first_retained_ordinal;
                changes.rescan_all = true;
            }

            while (!corpus.primary_retained_rows.empty() &&
                   corpus.first_retained_ordinal < update.first_retained_ordinal)
            {
                corpus.primary_retained_rows.pop_front();
                ++corpus.first_retained_ordinal;
            }
            corpus.first_retained_ordinal = update.first_retained_ordinal;

            for (Terminal_search_source_row& row : update.retained_rows) {
                changes.changed_retained_ordinals.insert(row.retained_ordinal);
                corpus.primary_retained_rows.push_back(
                    make_search_corpus_row(row));
            }
            corpus.end_retained_ordinal = update.end_retained_ordinal;

            std::vector<Search_corpus_row>& active_rows =
                update.identity.active_buffer == Terminal_buffer_id::PRIMARY
                    ? corpus.primary_active_rows
                    : corpus.alternate_active_rows;
            if (update.reset_active_rows) {
                active_rows.clear();
                active_rows.resize(static_cast<std::size_t>(
                    std::max(0, update.identity.grid_size.rows)));
                changes.rescan_all = true;
            }
            for (Terminal_search_source_row& row : update.active_rows) {
                if (row.active_grid_row < 0 ||
                    row.active_grid_row >= update.identity.grid_size.rows)
                {
                    changes.rescan_all = true;
                    continue;
                }
                const std::size_t active_index =
                    static_cast<std::size_t>(row.active_grid_row);
                if (active_index >= active_rows.size()) {
                    changes.rescan_all = true;
                    continue;
                }
                changes.changed_active_rows.insert(row.active_grid_row);
                active_rows[active_index] = make_search_corpus_row(row);
            }
            if (corpus.initialized &&
                corpus.identity.active_buffer != update.identity.active_buffer)
            {
                changes.rescan_all = true;
            }
            corpus.identity = update.identity;
            corpus.initialized = true;
        }
        return changes;
    }

    bool run_full_scan(
        const Desired_work&                       desired,
        std::optional<terminal_search_match_t>& refresh_anchor)
    {
        if (!refresh_anchor.has_value()) {
            std::lock_guard<std::mutex> lock(result_mutex);
            if (result.query == desired.query                                   &&
                result.query_generation == desired.query_generation             &&
                result.source_identity.active_buffer ==
                    desired.source_identity.active_buffer                       &&
                result.source_identity.active_buffer_epoch ==
                    desired.source_identity.active_buffer_epoch)
            {
                refresh_anchor = current_match_for_result(result);
            }
        }

        Search_match_rows matches;
        const Search_query_text query = make_search_query_text(desired.query);
        const int columns = desired.source_identity.grid_size.columns;
        const auto add_row = [&](
            const Search_corpus_row& row,
            search_match_row_key_t   key) -> bool {
            const auto row_matches = search_row(
                row,
                query,
                columns,
                desired_work_generation,
                desired.work_generation);
            if (!row_matches.has_value()) {
                return false;
            }
            if (!row_matches->empty()) {
                matches.emplace(
                    key,
                    Search_match_row{
                        terminal_history_handle_from_retained_identity(
                            row.retained_line_id,
                            row.content_generation),
                        std::move(*row_matches),
                        0,
                    });
            }
            return true;
        };

        if (desired.source_identity.active_buffer == Terminal_buffer_id::PRIMARY) {
            std::uint64_t retained_ordinal = corpus.first_retained_ordinal;
            for (const Search_corpus_row& row : corpus.primary_retained_rows) {
                if (!add_row(row, {false, retained_ordinal})) {
                    return false;
                }
                ++retained_ordinal;
            }
            for (std::size_t active_row = 0U;
                 active_row < corpus.primary_active_rows.size();
                 ++active_row)
            {
                const Search_corpus_row& row = corpus.primary_active_rows[active_row];
                if (!add_row(
                        row,
                        {true, static_cast<std::uint64_t>(active_row)}))
                {
                    return false;
                }
            }
        }
        else {
            for (std::size_t active_row = 0U;
                 active_row < corpus.alternate_active_rows.size();
                 ++active_row)
            {
                const Search_corpus_row& row = corpus.alternate_active_rows[active_row];
                if (!add_row(
                        row,
                        {true, static_cast<std::uint64_t>(active_row)}))
                {
                    return false;
                }
            }
        }
        if (desired_work_generation.load(std::memory_order_acquire) !=
            desired.work_generation)
        {
            return false;
        }

        Result completed;
        completed.matches = std::move(matches);
        finish_result(completed, desired, refresh_anchor);
        if (desired_work_generation.load(std::memory_order_acquire) !=
            desired.work_generation)
        {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(result_mutex);
            if (desired_work_generation.load(std::memory_order_acquire) !=
                desired.work_generation)
            {
                return false;
            }
            std::swap(result, completed);
        }
        worker_query = desired.query;
        worker_query_generation = desired.query_generation;
        worker_matches_valid = true;
        return true;
    }

    bool run_incremental_scan(
        const Desired_work&                       desired,
        const Corpus_changes&                     changes,
        std::optional<terminal_search_match_t>& refresh_anchor)
    {
        Search_match_rows replacements;
        const Search_query_text query = make_search_query_text(desired.query);
        const int columns = desired.source_identity.grid_size.columns;
        for (std::uint64_t retained_ordinal : changes.changed_retained_ordinals) {
            if (retained_ordinal < corpus.first_retained_ordinal ||
                retained_ordinal >= corpus.end_retained_ordinal)
            {
                continue;
            }
            const std::size_t retained_index = static_cast<std::size_t>(
                retained_ordinal - corpus.first_retained_ordinal);
            if (retained_index >= corpus.primary_retained_rows.size()) {
                return false;
            }
            const Search_corpus_row& row =
                corpus.primary_retained_rows[retained_index];
            const auto row_matches = search_row(
                row,
                query,
                columns,
                desired_work_generation,
                desired.work_generation);
            if (!row_matches.has_value()) {
                return false;
            }
            if (!row_matches->empty()) {
                replacements.emplace(
                    search_match_row_key_t{false, retained_ordinal},
                    Search_match_row{
                        terminal_history_handle_from_retained_identity(
                            row.retained_line_id,
                            row.content_generation),
                        std::move(*row_matches),
                        0,
                    });
            }
        }

        const std::vector<Search_corpus_row>& active_rows =
            desired.source_identity.active_buffer == Terminal_buffer_id::PRIMARY
                ? corpus.primary_active_rows
                : corpus.alternate_active_rows;
        for (int active_row : changes.changed_active_rows) {
            if (active_row < 0 ||
                static_cast<std::size_t>(active_row) >= active_rows.size())
            {
                return false;
            }
            const Search_corpus_row& row =
                active_rows[static_cast<std::size_t>(active_row)];
            const auto row_matches = search_row(
                row,
                query,
                columns,
                desired_work_generation,
                desired.work_generation);
            if (!row_matches.has_value()) {
                return false;
            }
            if (!row_matches->empty()) {
                replacements.emplace(
                    search_match_row_key_t{
                        true,
                        static_cast<std::uint64_t>(active_row),
                    },
                    Search_match_row{
                        terminal_history_handle_from_retained_identity(
                            row.retained_line_id,
                            row.content_generation),
                        std::move(*row_matches),
                        0,
                    });
            }
        }
        if (desired_work_generation.load(std::memory_order_acquire) !=
            desired.work_generation)
        {
            return false;
        }

        Result completed;
        {
            std::lock_guard<std::mutex> lock(result_mutex);
            if (desired_work_generation.load(std::memory_order_acquire) !=
                desired.work_generation)
            {
                return false;
            }
            std::swap(result, completed);
        }
        if (!refresh_anchor.has_value()                                  &&
            completed.query == desired.query                             &&
            completed.query_generation == desired.query_generation       &&
            completed.source_identity.active_buffer ==
                desired.source_identity.active_buffer                    &&
            completed.source_identity.active_buffer_epoch ==
                desired.source_identity.active_buffer_epoch)
        {
            refresh_anchor = current_match_for_result(completed);
        }
        for (auto it = completed.matches.begin(); it != completed.matches.end();) {
            const bool evicted_retained =
                !it->first.active_grid &&
                (it->first.position < corpus.first_retained_ordinal ||
                    it->first.position >= corpus.end_retained_ordinal);
            const bool changed_retained =
                !it->first.active_grid &&
                changes.changed_retained_ordinals.contains(it->first.position);
            const bool changed_active =
                it->first.active_grid &&
                changes.changed_active_rows.contains(
                    static_cast<int>(it->first.position));
            if (changed_active || evicted_retained || changed_retained) {
                it = completed.matches.erase(it);
            }
            else {
                ++it;
            }
        }
        completed.matches.insert(
            std::make_move_iterator(replacements.begin()),
            std::make_move_iterator(replacements.end()));
        finish_result(completed, desired, refresh_anchor);
        if (desired_work_generation.load(std::memory_order_acquire) !=
            desired.work_generation)
        {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(result_mutex);
            if (desired_work_generation.load(std::memory_order_acquire) !=
                desired.work_generation)
            {
                return false;
            }
            std::swap(result, completed);
        }
        worker_matches_valid = true;
        return true;
    }

    void finish_result(
        Result&                                      completed,
        const Desired_work&                          desired,
        const std::optional<terminal_search_match_t>& refresh_anchor)
    {
        completed.source_identity = desired.source_identity;
        completed.query = desired.query;
        completed.query_generation = desired.query_generation;
        completed.source_revision = desired.source_revision;
        completed.first_retained_ordinal = corpus.first_retained_ordinal;
        completed.end_retained_ordinal = corpus.end_retained_ordinal;
        completed.match_count = 0;
        std::map<std::pair<std::uint64_t, std::uint64_t>, int> next_match_ordinals;
        for (auto& [key, row] : completed.matches) {
            (void)key;
            int& next_match_ordinal = next_match_ordinals[{
                row.history_handle.byte_sequence,
                row.history_handle.content_generation,
            }];
            row.first_match_ordinal_in_retained_line = next_match_ordinal;
            next_match_ordinal += static_cast<int>(row.spans.size());
            completed.match_count += static_cast<int>(row.spans.size());
        }

        completed.current_row.reset();
        completed.current_row_match_ordinal = 0;
        completed.current_match_number = 0;
        if (refresh_anchor.has_value()) {
            int number = 0;
            for (const auto& [key, row] : completed.matches) {
                const int row_match_count = static_cast<int>(row.spans.size());
                const int match_ordinal =
                    refresh_anchor->identity.match_ordinal_in_retained_line -
                    row.first_match_ordinal_in_retained_line;
                if (row.history_handle == refresh_anchor->identity.history_handle &&
                    match_ordinal >= 0 && match_ordinal < row_match_count)
                {
                    completed.current_row = key;
                    completed.current_row_match_ordinal = match_ordinal;
                    completed.current_match_number = number + match_ordinal + 1;
                    break;
                }
                number += row_match_count;
            }
        }
        if (!completed.current_row.has_value() && !completed.matches.empty()) {
            const std::int64_t preferred_public_row = refresh_anchor.has_value()
                ? refresh_anchor->public_row
                : desired.preferred_public_row;
            auto selected = completed.matches.begin();
            while (selected != completed.matches.end() &&
                   public_row_for_key(completed, selected->first) <
                       preferred_public_row)
            {
                ++selected;
            }
            if (selected == completed.matches.end()) {
                selected = completed.matches.begin();
            }
            completed.current_row = selected->first;
            completed.current_row_match_ordinal = 0;
            int number = 1;
            for (auto row = completed.matches.begin(); row != selected; ++row) {
                number += static_cast<int>(row->second.spans.size());
            }
            completed.current_match_number = number;
        }
        completed.completion_ready = true;
    }

    static std::int64_t public_row_for_key(
        const Result&          completed,
        search_match_row_key_t key)
    {
        if (!key.active_grid) {
            return static_cast<std::int64_t>(
                key.position - completed.first_retained_ordinal);
        }
        const std::int64_t active_row = static_cast<std::int64_t>(key.position);
        return completed.source_identity.active_buffer == Terminal_buffer_id::PRIMARY
            ? static_cast<std::int64_t>(
                completed.end_retained_ordinal -
                    completed.first_retained_ordinal) + active_row
            : active_row;
    }

    static std::optional<terminal_search_match_t> current_match_for_result(
        const Result& completed)
    {
        if (!completed.current_row.has_value()) {
            return std::nullopt;
        }
        const auto row = completed.matches.find(*completed.current_row);
        if (row == completed.matches.end() ||
            completed.current_row_match_ordinal < 0 ||
            completed.current_row_match_ordinal >=
                static_cast<int>(row->second.spans.size()))
        {
            return std::nullopt;
        }

        const search_match_span_t span = row->second.spans[
            static_cast<std::size_t>(completed.current_row_match_ordinal)];
        return terminal_search_match_t{
            {
                completed.source_identity.active_buffer,
                completed.source_identity.active_buffer_epoch,
                row->second.history_handle,
                row->second.first_match_ordinal_in_retained_line +
                    completed.current_row_match_ordinal,
            },
            public_row_for_key(completed, row->first),
            span.first_column,
            span.column_count,
        };
    }

    std::optional<terminal_search_match_t> current_match_locked() const
    {
        return current_match_for_result(result);
    }
};

Terminal_search_controller::Terminal_search_controller(
    std::function<void()> completion_notifier)
:
    m_shared(std::make_shared<Shared_state>(std::move(completion_notifier)))
{
}

Terminal_search_controller::~Terminal_search_controller()
{
    m_shared->close();
}

void Terminal_search_controller::clear()
{
    m_query.clear();
    ++m_query_generation;
    m_searching = false;
    request_current_query();
}

void Terminal_search_controller::set_source_unavailable(QString query)
{
    m_query = std::move(query);
    ++m_query_generation;
    m_source_available = false;
    m_searching = false;
    request_current_query();
}

void Terminal_search_controller::set_query(
    QString      query,
    std::int64_t preferred_public_row)
{
    if (query == m_query && m_source_available) {
        return;
    }

    m_query = std::move(query);
    m_preferred_public_row = preferred_public_row;
    ++m_query_generation;
    m_searching = m_source_available && !m_query.isEmpty();
    request_current_query();
}

void Terminal_search_controller::update_source(Terminal_search_source_update update)
{
    m_source_identity = update.identity;
    m_source_revision = update.revision;
    m_source_available = true;
    if (!m_query.isEmpty()) {
        m_searching = true;
    }

    Shared_state::Desired_work desired;
    desired.query                   = m_query;
    desired.source_identity         = m_source_identity;
    desired.query_generation        = m_query_generation;
    desired.source_revision         = m_source_revision;
    desired.preferred_public_row    = m_preferred_public_row;
    desired.source_available        = m_source_available;
    m_shared->request(std::move(desired), std::move(update));
}

bool Terminal_search_controller::process_completion()
{
    if (!m_searching) {
        return false;
    }
    if (!m_shared->completion_matches(
            m_query_generation,
            m_source_revision,
            m_source_identity))
    {
        return false;
    }

    m_searching = false;
    return true;
}

bool Terminal_search_controller::wait_for_completion_for_testing(
    std::chrono::milliseconds timeout)
{
    return m_shared->wait_until_idle(timeout) && process_completion();
}

bool Terminal_search_controller::wait_for_idle_for_testing(
    std::chrono::milliseconds timeout)
{
    return m_shared->wait_until_idle(timeout);
}

terminal_search_result_state_t Terminal_search_controller::result_state() const
{
    if (m_query.isEmpty()) {
        return {};
    }
    if (!m_source_available) {
        return {Terminal_search_result_status::SOURCE_UNAVAILABLE, 0, 0};
    }
    if (m_searching) {
        return {Terminal_search_result_status::SEARCHING, 0, 0};
    }
    return m_shared->committed_result_state();
}

std::optional<terminal_search_match_t> Terminal_search_controller::current_match() const
{
    return !m_searching && m_source_available && !m_query.isEmpty()
        ? m_shared->current_match()
        : std::nullopt;
}

std::optional<std::vector<terminal_history_handle_t>>
Terminal_search_controller::source_row_handles(
    std::int64_t first_public_row,
    int          row_count) const
{
    return !m_searching && m_source_available && !m_query.isEmpty()
        ? m_shared->source_row_handles(
            m_source_identity,
            first_public_row,
            row_count)
        : std::nullopt;
}

bool Terminal_search_controller::select_next()
{
    return !m_searching && m_shared->select_next();
}

bool Terminal_search_controller::select_previous()
{
    return !m_searching && m_shared->select_previous();
}

std::vector<Terminal_render_search_match_span>
Terminal_search_controller::spans_for_snapshot(
    const Terminal_render_snapshot& snapshot,
    std::uint64_t                   active_buffer_epoch) const
{
    return !m_searching && m_source_available && !m_query.isEmpty()
        ? m_shared->spans_for_snapshot(snapshot, active_buffer_epoch)
        : std::vector<Terminal_render_search_match_span>{};
}

void Terminal_search_controller::request_current_query()
{
    Shared_state::Desired_work desired;
    desired.query                   = m_query;
    desired.source_identity         = m_source_identity;
    desired.query_generation        = m_query_generation;
    desired.source_revision         = m_source_revision;
    desired.preferred_public_row    = m_preferred_public_row;
    desired.source_available        = m_source_available;
    m_shared->request(std::move(desired));
}

}
