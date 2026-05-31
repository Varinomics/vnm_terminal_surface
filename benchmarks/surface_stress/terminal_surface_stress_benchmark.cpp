#include "vnm_terminal/internal/qsg_terminal_render_frame.h"
#include "vnm_terminal/internal/terminal_screen_model.h"

#include <QByteArray>
#include <QSizeF>
#include <QString>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

struct Benchmark_options
{
    int frames         = 180;
    int warmup_frames  = 10;
    int rows           = 235;
    int columns        = 873;
    int dirty_rows     = 235;
    int dirty_row_stride = 1;
    int graphics_every = 11;
    int style_period   = 8;
    bool packed_text_sidecars_enabled = false;
};

struct Benchmark_totals
{
    std::uint64_t snapshot_cells = 0U;
    std::uint64_t snapshot_dirty_rows_visible = 0U;
    std::uint64_t frame_cell_pass_input_cells = 0U;
    std::uint64_t frame_dirty_row_lookup_count = 0U;
    std::uint64_t frame_dirty_row_range_lookup_count = 0U;
    std::uint64_t frame_dirty_row_range_scan_steps = 0U;
    std::uint64_t frame_packed_pass_cells_scanned = 0U;
    std::uint64_t frame_text_runs_emitted = 0U;
    std::uint64_t frame_graphic_rects_emitted = 0U;
    std::uint64_t frame_packed_graphic_cells = 0U;
    std::uint64_t checksum = 0U;
};

using Clock = std::chrono::steady_clock;

template <typename Stats>
int dirty_row_range_lookup_count(const Stats& stats)
{
    if constexpr (requires { stats.dirty_row_range_lookup_count; }) {
        return stats.dirty_row_range_lookup_count;
    }
    return 0;
}

template <typename Stats>
int dirty_row_range_scan_steps(const Stats& stats)
{
    if constexpr (requires { stats.dirty_row_range_scan_steps; }) {
        return stats.dirty_row_range_scan_steps;
    }
    return 0;
}

double elapsed_ms(Clock::time_point begin, Clock::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

bool parse_int_arg(
    int          argc,
    char**       argv,
    int&         index,
    const char*  name,
    int&         value)
{
    if (std::string(argv[index]) != name) {
        return false;
    }
    if (index + 1 >= argc) {
        std::cerr << name << " requires a value\n";
        std::exit(2);
    }

    value = std::atoi(argv[index + 1]);
    index += 2;
    return true;
}

Benchmark_options parse_options(int argc, char** argv)
{
    Benchmark_options options;

    int index = 1;
    while (index < argc) {
        if (parse_int_arg(argc, argv, index, "--frames", options.frames) ||
            parse_int_arg(argc, argv, index, "--warmup-frames", options.warmup_frames) ||
            parse_int_arg(argc, argv, index, "--rows", options.rows) ||
            parse_int_arg(argc, argv, index, "--cols", options.columns) ||
            parse_int_arg(argc, argv, index, "--dirty-rows", options.dirty_rows) ||
            parse_int_arg(argc, argv, index, "--dirty-row-stride", options.dirty_row_stride) ||
            parse_int_arg(argc, argv, index, "--graphics-every", options.graphics_every) ||
            parse_int_arg(argc, argv, index, "--style-period", options.style_period))
        {
            continue;
        }

        const std::string argument = argv[index];
        if (argument == "--packed-text-sidecars") {
            options.packed_text_sidecars_enabled = true;
            ++index;
            continue;
        }
        if (argument == "--help") {
            std::cout
                << "Usage: vnm_terminal_surface_stress_benchmark [options]\n"
                << "  --frames <n>                 measured frames, default 180\n"
                << "  --warmup-frames <n>          unmeasured warmup frames, default 10\n"
                << "  --rows <n>                   grid rows, default 235\n"
                << "  --cols <n>                   grid columns, default 873\n"
                << "  --dirty-rows <n>             rows rewritten per frame, default all rows\n"
                << "  --dirty-row-stride <n>       strided dirty row walk; fragmentation requires --dirty-rows < --rows, default 1\n"
                << "  --graphics-every <n>         emit U+2588 every N cells, 0 disables\n"
                << "  --style-period <n>           ANSI color variation period, 0 disables\n"
                << "  --packed-text-sidecars       enable packed text sidecars\n";
            std::exit(0);
        }

        std::cerr << "unknown argument: " << argument << '\n';
        std::exit(2);
    }

    options.frames        = std::max(1, options.frames);
    options.warmup_frames = std::max(0, options.warmup_frames);
    options.rows          = std::max(1, options.rows);
    options.columns       = std::max(1, options.columns);
    options.dirty_rows    = std::clamp(options.dirty_rows, 1, options.rows);
    options.dirty_row_stride = std::clamp(options.dirty_row_stride, 1, options.rows);
    options.graphics_every = std::max(0, options.graphics_every);
    options.style_period   = std::max(0, options.style_period);
    return options;
}

std::vector<int> dirty_rows_for_frame(const Benchmark_options& options, int frame_index)
{
    if (options.dirty_rows >= options.rows) {
        std::vector<int> rows(static_cast<std::size_t>(options.rows));
        for (int row = 0; row < options.rows; ++row) {
            rows[static_cast<std::size_t>(row)] = row;
        }
        return rows;
    }

    std::vector<bool> used(static_cast<std::size_t>(options.rows), false);
    std::vector<int> rows;
    rows.reserve(static_cast<std::size_t>(options.dirty_rows));
    std::int64_t probe = static_cast<std::int64_t>(frame_index) * 37;
    while (static_cast<int>(rows.size()) < options.dirty_rows) {
        const std::int64_t frame_offset = static_cast<std::int64_t>(frame_index) * 13;
        const int candidate = options.dirty_row_stride > 1
            ? static_cast<int>(
                ((probe * options.dirty_row_stride) + frame_offset) %
                    options.rows)
            : static_cast<int>(((probe * 17) + frame_offset) % options.rows);
        int row = candidate;
        if (options.dirty_row_stride > 1 && used[static_cast<std::size_t>(row)]) {
            for (int offset = 1; offset < options.rows; ++offset) {
                const int fallback_row = (candidate + offset) % options.rows;
                if (!used[static_cast<std::size_t>(fallback_row)]) {
                    row = fallback_row;
                    break;
                }
            }
        }
        if (!used[static_cast<std::size_t>(row)]) {
            used[static_cast<std::size_t>(row)] = true;
            rows.push_back(row);
        }
        ++probe;
    }
    std::sort(rows.begin(), rows.end());
    return rows;
}

QString row_text_for_frame(
    const Benchmark_options& options,
    int                      frame_index,
    int                      row)
{
    QString text;
    text.reserve(options.columns);
    for (int column = 0; column < options.columns; ++column) {
        const int pattern = frame_index * 17 + row * 31 + column * 7;
        if (options.graphics_every > 0 && pattern % options.graphics_every == 0) {
            text += QStringLiteral("\u2588");
            continue;
        }

        const char16_t character =
            static_cast<char16_t>(u'A' + (pattern % 26));
        text += QChar(character);
    }
    return text;
}

QByteArray payload_for_frame(
    const Benchmark_options& options,
    int                      frame_index,
    const std::vector<int>&  dirty_rows)
{
    QByteArray payload;
    const int estimated_bytes_per_row = options.columns * 2 + 24;
    payload.reserve(static_cast<qsizetype>(dirty_rows.size() * estimated_bytes_per_row));

    for (const int row : dirty_rows) {
        payload.append("\x1b[");
        payload.append(QByteArray::number(row + 1));
        payload.append(";1H");

        if (options.style_period > 0) {
            const int color = 30 + ((row + frame_index) % options.style_period) % 8;
            payload.append("\x1b[");
            payload.append(QByteArray::number(color));
            payload.append('m');
        }

        payload.append(row_text_for_frame(options, frame_index, row).toUtf8());
    }

    payload.append("\x1b[0m");
    return payload;
}

term::Terminal_viewport_state tail_viewport(const term::Terminal_screen_model& model)
{
    term::Terminal_viewport_state viewport;
    viewport.active_buffer = model.active_buffer_id();
    viewport.visible_rows  = model.grid_size().rows;
    viewport.scrollback_rows =
        model.active_buffer_id() == term::Terminal_buffer_id::PRIMARY
            ? model.scrollback_size()
            : 0;
    viewport.follow_tail      = true;
    viewport.offset_from_tail = 0;
    return viewport;
}

term::Terminal_render_snapshot_request snapshot_request(
    const term::Terminal_screen_model&          model,
    const term::Terminal_screen_model_result&   result,
    std::uint64_t                               sequence)
{
    term::Terminal_render_snapshot_request request;
    request.sequence      = sequence;
    request.viewport      = tail_viewport(model);
    request.dirty_rows    = result.dirty_rows;
    request.cursor_blink_enabled = false;
    return request;
}

term::terminal_cell_metrics_t benchmark_cell_metrics()
{
    term::terminal_cell_metrics_t metrics;
    metrics.width   = 8.0;
    metrics.height  = 16.0;
    metrics.ascent  = 12.0;
    metrics.descent = 4.0;
    return metrics;
}

void accumulate_frame_stats(
    Benchmark_totals&                         totals,
    const term::Terminal_render_snapshot&     snapshot,
    const term::Terminal_render_frame&        frame)
{
    totals.snapshot_cells += snapshot.cells.size();
    for (const term::Terminal_render_dirty_row_range& range : snapshot.dirty_row_ranges) {
        totals.snapshot_dirty_rows_visible += static_cast<std::uint64_t>(range.row_count);
    }

    totals.frame_cell_pass_input_cells +=
        static_cast<std::uint64_t>(frame.stats.cell_pass_input_cells);
    totals.frame_dirty_row_lookup_count +=
        static_cast<std::uint64_t>(frame.stats.dirty_row_lookup_count);
    totals.frame_dirty_row_range_lookup_count +=
        static_cast<std::uint64_t>(dirty_row_range_lookup_count(frame.stats));
    totals.frame_dirty_row_range_scan_steps +=
        static_cast<std::uint64_t>(dirty_row_range_scan_steps(frame.stats));
    totals.frame_packed_pass_cells_scanned +=
        static_cast<std::uint64_t>(frame.stats.packed_pass_cells_scanned);
    totals.frame_text_runs_emitted +=
        static_cast<std::uint64_t>(frame.stats.text_runs_emitted);
    totals.frame_graphic_rects_emitted +=
        static_cast<std::uint64_t>(frame.stats.graphic_rects_emitted);
    totals.frame_packed_graphic_cells +=
        static_cast<std::uint64_t>(frame.stats.packed_graphic_cells);
    totals.checksum +=
        static_cast<std::uint64_t>(frame.text_runs.size()) * 3U +
        static_cast<std::uint64_t>(frame.graphic_rects.size()) * 5U +
        static_cast<std::uint64_t>(frame.packed_graphic_codepoints.size()) * 7U +
        static_cast<std::uint64_t>(snapshot.metadata.sequence);
}

void print_metric(const char* name, std::uint64_t value)
{
    std::cout << name << '=' << value << '\n';
}

void print_metric(const char* name, double value)
{
    std::cout << name << '=' << value << '\n';
}

}

int main(int argc, char** argv)
{
    const Benchmark_options options = parse_options(argc, argv);
    const term::terminal_grid_size_t grid_size{options.rows, options.columns};

    term::Terminal_screen_model model({grid_size, 0, 8});
    term::Terminal_render_options render_options;
    render_options.packed_text_sidecars_enabled =
        options.packed_text_sidecars_enabled;

    const term::terminal_cell_metrics_t metrics = benchmark_cell_metrics();
    const QSizeF logical_size(
        metrics.width * static_cast<qreal>(options.columns),
        metrics.height * static_cast<qreal>(options.rows));

    double ingest_ms = 0.0;
    double snapshot_ms = 0.0;
    double frame_ms = 0.0;
    double total_ms = 0.0;
    Benchmark_totals totals;

    const int total_frames = options.warmup_frames + options.frames;
    for (int frame_index = 0; frame_index < total_frames; ++frame_index) {
        const bool measured = frame_index >= options.warmup_frames;
        const std::vector<int> dirty_rows =
            dirty_rows_for_frame(options, frame_index);
        const QByteArray payload =
            payload_for_frame(options, frame_index, dirty_rows);

        const Clock::time_point frame_begin = Clock::now();
        const Clock::time_point ingest_begin = Clock::now();
        const term::Terminal_screen_model_result result = model.ingest(payload);
        const Clock::time_point ingest_end = Clock::now();

        const Clock::time_point snapshot_begin = Clock::now();
        const term::Terminal_render_snapshot snapshot =
            model.render_snapshot(
                snapshot_request(
                    model,
                    result,
                    static_cast<std::uint64_t>(frame_index + 1)));
        const Clock::time_point snapshot_end = Clock::now();

        const Clock::time_point frame_build_begin = Clock::now();
        const term::Terminal_render_frame frame =
            term::build_terminal_render_frame(
                &snapshot,
                logical_size,
                metrics,
                render_options,
                false);
        const Clock::time_point frame_build_end = Clock::now();
        const Clock::time_point frame_end = Clock::now();

        if (measured) {
            ingest_ms   += elapsed_ms(ingest_begin, ingest_end);
            snapshot_ms += elapsed_ms(snapshot_begin, snapshot_end);
            frame_ms    += elapsed_ms(frame_build_begin, frame_build_end);
            total_ms    += elapsed_ms(frame_begin, frame_end);
            accumulate_frame_stats(totals, snapshot, frame);
        }
    }

    const double frames = static_cast<double>(options.frames);
    std::cout << "scenario=nelostie_like_model_snapshot_frame\n";
    print_metric("frames", static_cast<std::uint64_t>(options.frames));
    print_metric("warmup_frames", static_cast<std::uint64_t>(options.warmup_frames));
    print_metric("rows", static_cast<std::uint64_t>(options.rows));
    print_metric("columns", static_cast<std::uint64_t>(options.columns));
    print_metric("dirty_rows_requested", static_cast<std::uint64_t>(options.dirty_rows));
    print_metric("dirty_row_stride", static_cast<std::uint64_t>(options.dirty_row_stride));
    print_metric("graphics_every", static_cast<std::uint64_t>(options.graphics_every));
    print_metric("style_period", static_cast<std::uint64_t>(options.style_period));
    std::cout << "packed_text_sidecars_enabled="
        << (options.packed_text_sidecars_enabled ? "true" : "false") << '\n';
    print_metric("total_ms", total_ms);
    print_metric("frames_per_second", frames * 1000.0 / total_ms);
    print_metric("ingest_ms_total", ingest_ms);
    print_metric("snapshot_ms_total", snapshot_ms);
    print_metric("render_frame_ms_total", frame_ms);
    print_metric("ingest_ms_per_frame", ingest_ms / frames);
    print_metric("snapshot_ms_per_frame", snapshot_ms / frames);
    print_metric("render_frame_ms_per_frame", frame_ms / frames);
    print_metric("snapshot_cells_per_frame", static_cast<double>(totals.snapshot_cells) / frames);
    print_metric(
        "snapshot_dirty_rows_visible_per_frame",
        static_cast<double>(totals.snapshot_dirty_rows_visible) / frames);
    print_metric("frame_cell_pass_input_cells", totals.frame_cell_pass_input_cells);
    print_metric("frame_dirty_row_lookup_count", totals.frame_dirty_row_lookup_count);
    print_metric(
        "frame_dirty_row_range_lookup_count",
        totals.frame_dirty_row_range_lookup_count);
    print_metric(
        "frame_dirty_row_range_scan_steps",
        totals.frame_dirty_row_range_scan_steps);
    print_metric("frame_packed_pass_cells_scanned", totals.frame_packed_pass_cells_scanned);
    print_metric("frame_text_runs_emitted", totals.frame_text_runs_emitted);
    print_metric("frame_graphic_rects_emitted", totals.frame_graphic_rects_emitted);
    print_metric("frame_packed_graphic_cells", totals.frame_packed_graphic_cells);
    print_metric("checksum", totals.checksum);
    return 0;
}
