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
#include <optional>
#include <string>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

constexpr const char* k_frame_descriptor_counter_semantics =
    "batch_7_frame_qsg_descriptor_reuse";

enum class Text_pattern
{
    LEGACY_ASCII_BLOCK,
    ASCII,
    BLOCK,
    BOX,
    CJK,
    MIXED_NON_ASCII,
    EMOJI,
};

struct Benchmark_options
{
    int frames           = 180;
    int warmup_frames    = 10;
    int rows             = 235;
    int columns          = 873;
    int dirty_rows       = 235;
    int dirty_row_stride = 1;
    int dirty_row_seed   = 37;
    int graphics_every   = 11;
    int style_period     = 8;
    Text_pattern text_pattern = Text_pattern::LEGACY_ASCII_BLOCK;
    bool model_profile_stats_enabled  = false;
};

struct Benchmark_totals
{
    std::uint64_t snapshot_cells = 0U;
    std::uint64_t snapshot_dirty_rows_visible = 0U;
    term::terminal_simple_content_cumulative_stats_t simple_content;
    std::uint64_t frame_visible_rows = 0U;
    std::uint64_t frame_dirty_rows = 0U;
    std::uint64_t frame_full_dirty_rows = 0U;
    std::uint64_t frame_cell_pass_input_cells = 0U;
    std::uint64_t frame_cells_considered = 0U;
    std::uint64_t frame_cell_pass_classification_calls = 0U;
    std::uint64_t frame_row_descriptors_built = 0U;
    std::uint64_t frame_layer_descriptors_built = 0U;
    std::uint64_t frame_dirty_row_lookup_count = 0U;
    std::uint64_t frame_dirty_row_range_lookup_count = 0U;
    std::uint64_t frame_dirty_row_range_scan_steps = 0U;
    std::uint64_t frame_compact_ascii_text_direct_appends = 0U;
    std::uint64_t frame_compact_ascii_qstring_materializations = 0U;
    std::uint64_t frame_text_runs_emitted = 0U;
    std::uint64_t frame_graphic_rects_emitted = 0U;
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

QString text_pattern_name(Text_pattern pattern)
{
    switch (pattern) {
        case Text_pattern::LEGACY_ASCII_BLOCK:
            return QStringLiteral("legacy_ascii_block");
        case Text_pattern::ASCII:
            return QStringLiteral("ascii");
        case Text_pattern::BLOCK:
            return QStringLiteral("block");
        case Text_pattern::BOX:
            return QStringLiteral("box");
        case Text_pattern::CJK:
            return QStringLiteral("cjk");
        case Text_pattern::MIXED_NON_ASCII:
            return QStringLiteral("mixed_non_ascii");
        case Text_pattern::EMOJI:
            return QStringLiteral("emoji");
    }

    return QStringLiteral("unknown");
}

std::optional<Text_pattern> parse_text_pattern(const std::string& value)
{
    if (value == "ascii") {
        return Text_pattern::ASCII;
    }
    if (value == "block") {
        return Text_pattern::BLOCK;
    }
    if (value == "box") {
        return Text_pattern::BOX;
    }
    if (value == "cjk") {
        return Text_pattern::CJK;
    }
    if (value == "mixed_non_ascii") {
        return Text_pattern::MIXED_NON_ASCII;
    }
    if (value == "emoji") {
        return Text_pattern::EMOJI;
    }

    return std::nullopt;
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
            parse_int_arg(argc, argv, index, "--dirty-row-seed", options.dirty_row_seed) ||
            parse_int_arg(argc, argv, index, "--graphics-every", options.graphics_every) ||
            parse_int_arg(argc, argv, index, "--style-period", options.style_period))
        {
            continue;
        }

        const std::string argument = argv[index];
        if (argument == "--model-profile-stats") {
            options.model_profile_stats_enabled = true;
            ++index;
            continue;
        }
        if (argument == "--text-pattern") {
            if (index + 1 >= argc) {
                std::cerr << "--text-pattern requires a value\n";
                std::exit(2);
            }

            const std::optional<Text_pattern> pattern =
                parse_text_pattern(argv[index + 1]);
            if (!pattern.has_value()) {
                std::cerr
                    << "--text-pattern requires one of "
                    << "ascii|block|box|cjk|mixed_non_ascii|emoji\n";
                std::exit(2);
            }

            options.text_pattern = *pattern;
            index += 2;
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
                << "  --dirty-row-seed <n>         deterministic dirty row walk seed, default 37\n"
                << "  --graphics-every <n>         emit U+2588 every N cells, 0 disables\n"
                << "  --style-period <n>           ANSI color variation period, 0 disables\n"
                << "  --text-pattern <name>        ascii|block|box|cjk|mixed_non_ascii|emoji; default preserves old mixed ASCII+U+2588 via --graphics-every\n"
                << "  --model-profile-stats        reset/enable model profile counters after warmup in profiling builds\n";
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
    options.dirty_row_seed = std::max(1, options.dirty_row_seed);
    options.graphics_every = std::max(0, options.graphics_every);
    options.style_period   = std::max(0, options.style_period);
    return options;
}

std::vector<int> all_rows(int row_count)
{
    std::vector<int> rows(static_cast<std::size_t>(row_count));
    for (int row = 0; row < row_count; ++row) {
        rows[static_cast<std::size_t>(row)] = row;
    }
    return rows;
}

std::vector<int> dirty_rows_for_frame(const Benchmark_options& options, int frame_index)
{
    if (options.dirty_rows >= options.rows) {
        return all_rows(options.rows);
    }

    std::vector<bool> used(static_cast<std::size_t>(options.rows), false);
    std::vector<int> rows;
    rows.reserve(static_cast<std::size_t>(options.dirty_rows));
    std::int64_t probe =
        static_cast<std::int64_t>(frame_index) * options.dirty_row_seed;
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

QChar ascii_pattern_char(int pattern)
{
    return QChar(static_cast<ushort>(u'A' + (pattern % 26)));
}

QChar box_pattern_char(int row, int column, int frame_index)
{
    const int selector = (row * 3 + column * 5 + frame_index) % 7;
    if (selector == 0) {
        return QChar(static_cast<ushort>(0x253cU));
    }
    if (selector % 2 == 0) {
        return QChar(static_cast<ushort>(0x2502U));
    }
    return QChar(static_cast<ushort>(0x2500U));
}

void append_wide_or_space(QString& text, int& column, int columns, const QString& wide_text)
{
    if (column + 1 < columns) {
        text += wide_text;
        column += 2;
        return;
    }

    text += QChar(u' ');
    ++column;
}

QString emoji_marker()
{
    return QString::fromUtf8("\xF0\x9F\x99\x82");
}

QString row_text_for_frame(
    const Benchmark_options& options,
    int                      frame_index,
    int                      row)
{
    QString text;
    text.reserve(options.columns);
    int column = 0;
    while (column < options.columns) {
        const int pattern = frame_index * 17 + row * 31 + column * 7;
        switch (options.text_pattern) {
            case Text_pattern::LEGACY_ASCII_BLOCK:
                if (options.graphics_every > 0 &&
                    pattern % options.graphics_every == 0)
                {
                    text += QChar(static_cast<ushort>(0x2588U));
                }
                else {
                    text += ascii_pattern_char(pattern);
                }
                ++column;
                break;

            case Text_pattern::ASCII:
                text += ascii_pattern_char(pattern);
                ++column;
                break;

            case Text_pattern::BLOCK:
                text += QChar(static_cast<ushort>(0x2588U));
                ++column;
                break;

            case Text_pattern::BOX:
                text += box_pattern_char(row, column, frame_index);
                ++column;
                break;

            case Text_pattern::CJK:
                append_wide_or_space(text, column, options.columns, QStringLiteral("\u754c"));
                break;

            case Text_pattern::EMOJI:
                append_wide_or_space(text, column, options.columns, emoji_marker());
                break;

            case Text_pattern::MIXED_NON_ASCII: {
                const int selector = pattern % 19;
                if (selector == 0 && column + 1 < options.columns) {
                    append_wide_or_space(
                        text,
                        column,
                        options.columns,
                        QStringLiteral("\u754c"));
                    break;
                }
                if (selector == 1 && column + 1 < options.columns) {
                    append_wide_or_space(text, column, options.columns, emoji_marker());
                    break;
                }
                if (selector == 2) {
                    text += QStringLiteral("e\u0301");
                    ++column;
                    break;
                }
                if (selector == 3) {
                    text += QChar(static_cast<ushort>(0x2588U));
                    ++column;
                    break;
                }
                if (selector == 4) {
                    text += box_pattern_char(row, column, frame_index);
                    ++column;
                    break;
                }

                text += ascii_pattern_char(pattern);
                ++column;
                break;
            }
        }
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

void accumulate_simple_content_stats(
    term::terminal_simple_content_cumulative_stats_t&    totals,
    const term::terminal_simple_content_stats_t&         stats)
{
    totals.cells_considered               += stats.cells_considered;
    totals.eligible_cells                 += stats.eligible_cells;
    totals.eligible_after_all_gates_cells += stats.eligible_after_all_gates_cells;
    totals.rows_with_eligible_cells       += stats.rows_with_eligible_cells;
    totals.styles_with_eligible_cells     += stats.styles_with_eligible_cells;
    totals.dirty_eligible_cells           += stats.dirty_eligible_cells;
    totals.clean_eligible_cells           += stats.clean_eligible_cells;
    totals.text_category_empty_cells      += stats.text_category_empty_cells;
    totals.text_category_printable_ascii_cells +=
        stats.text_category_printable_ascii_cells;
    totals.text_category_other_ascii_cells += stats.text_category_other_ascii_cells;
    totals.text_category_non_ascii_cells   += stats.text_category_non_ascii_cells;
    totals.route_none_cells                += stats.route_none_cells;
    totals.route_fast_text_cells           += stats.route_fast_text_cells;
    totals.route_qt_text_layout_cells      += stats.route_qt_text_layout_cells;
    totals.route_fallback_cells            += stats.route_fallback_cells;
    totals.rejection_none_cells            += stats.rejection_none_cells;
    totals.rejection_empty_text_cells      += stats.rejection_empty_text_cells;
    totals.rejection_invalid_grid_cells    += stats.rejection_invalid_grid_cells;
    totals.rejection_invalid_position_cells += stats.rejection_invalid_position_cells;
    totals.rejection_invalid_style_id_cells += stats.rejection_invalid_style_id_cells;
    totals.rejection_wide_continuation_cells +=
        stats.rejection_wide_continuation_cells;
    totals.rejection_invalid_display_width_cells +=
        stats.rejection_invalid_display_width_cells;
    totals.rejection_invalid_text_encoding_cells +=
        stats.rejection_invalid_text_encoding_cells;
    totals.rejection_invalid_text_width_cells +=
        stats.rejection_invalid_text_width_cells;
    totals.rejection_multi_cell_text_cells +=
        stats.rejection_multi_cell_text_cells;
    totals.rejection_non_printable_ascii_cells +=
        stats.rejection_non_printable_ascii_cells;
    totals.rejection_non_ascii_text_cells +=
        stats.rejection_non_ascii_text_cells;
    totals.rejection_decoration_cells       += stats.rejection_decoration_cells;
    totals.rejection_hyperlink_cells        += stats.rejection_hyperlink_cells;
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

    accumulate_simple_content_stats(totals.simple_content, frame.stats.simple_content);
    totals.frame_visible_rows +=
        static_cast<std::uint64_t>(frame.stats.visible_rows);
    totals.frame_dirty_rows +=
        static_cast<std::uint64_t>(frame.stats.dirty_rows);
    totals.frame_full_dirty_rows +=
        static_cast<std::uint64_t>(frame.stats.full_dirty_rows);
    totals.frame_cell_pass_input_cells +=
        static_cast<std::uint64_t>(frame.stats.cell_pass_input_cells);
    totals.frame_cells_considered +=
        static_cast<std::uint64_t>(frame.stats.cells_considered);
    totals.frame_cell_pass_classification_calls +=
        static_cast<std::uint64_t>(frame.stats.cell_pass_classification_calls);
    totals.frame_row_descriptors_built +=
        static_cast<std::uint64_t>(frame.stats.row_descriptors_built);
    totals.frame_layer_descriptors_built +=
        static_cast<std::uint64_t>(frame.stats.layer_descriptors_built);
    totals.frame_dirty_row_lookup_count +=
        static_cast<std::uint64_t>(frame.stats.dirty_row_lookup_count);
    totals.frame_dirty_row_range_lookup_count +=
        static_cast<std::uint64_t>(dirty_row_range_lookup_count(frame.stats));
    totals.frame_dirty_row_range_scan_steps +=
        static_cast<std::uint64_t>(dirty_row_range_scan_steps(frame.stats));
    totals.frame_compact_ascii_text_direct_appends +=
        static_cast<std::uint64_t>(frame.stats.compact_ascii_text_direct_appends);
    totals.frame_compact_ascii_qstring_materializations +=
        static_cast<std::uint64_t>(frame.stats.compact_ascii_qstring_materializations);
    totals.frame_text_runs_emitted +=
        static_cast<std::uint64_t>(frame.stats.text_runs_emitted);
    totals.frame_graphic_rects_emitted +=
        static_cast<std::uint64_t>(frame.stats.graphic_rects_emitted);
    totals.checksum +=
        static_cast<std::uint64_t>(frame.text_runs.size()) * 3U +
        static_cast<std::uint64_t>(frame.graphic_rects.size()) * 5U +
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

void print_simple_content_stats(
    const term::terminal_simple_content_cumulative_stats_t& stats)
{
    print_metric("simple_content.cells_considered", stats.cells_considered);
    print_metric("simple_content.eligible_cells", stats.eligible_cells);
    print_metric(
        "simple_content.eligible_after_all_gates_cells",
        stats.eligible_after_all_gates_cells);
    print_metric("simple_content.rows_with_eligible_cells", stats.rows_with_eligible_cells);
    print_metric(
        "simple_content.styles_with_eligible_cells",
        stats.styles_with_eligible_cells);
    print_metric("simple_content.dirty_eligible_cells", stats.dirty_eligible_cells);
    print_metric("simple_content.clean_eligible_cells", stats.clean_eligible_cells);
    print_metric(
        "simple_content.text_category_empty_cells",
        stats.text_category_empty_cells);
    print_metric(
        "simple_content.text_category_printable_ascii_cells",
        stats.text_category_printable_ascii_cells);
    print_metric(
        "simple_content.text_category_other_ascii_cells",
        stats.text_category_other_ascii_cells);
    print_metric(
        "simple_content.text_category_non_ascii_cells",
        stats.text_category_non_ascii_cells);
    print_metric("simple_content.route_none_cells", stats.route_none_cells);
    print_metric("simple_content.route_fast_text_cells", stats.route_fast_text_cells);
    print_metric(
        "simple_content.route_qt_text_layout_cells",
        stats.route_qt_text_layout_cells);
    print_metric("simple_content.route_fallback_cells", stats.route_fallback_cells);
    print_metric("simple_content.rejection_none_cells", stats.rejection_none_cells);
    print_metric(
        "simple_content.rejection_empty_text_cells",
        stats.rejection_empty_text_cells);
    print_metric(
        "simple_content.rejection_invalid_grid_cells",
        stats.rejection_invalid_grid_cells);
    print_metric(
        "simple_content.rejection_invalid_position_cells",
        stats.rejection_invalid_position_cells);
    print_metric(
        "simple_content.rejection_invalid_style_id_cells",
        stats.rejection_invalid_style_id_cells);
    print_metric(
        "simple_content.rejection_wide_continuation_cells",
        stats.rejection_wide_continuation_cells);
    print_metric(
        "simple_content.rejection_invalid_display_width_cells",
        stats.rejection_invalid_display_width_cells);
    print_metric(
        "simple_content.rejection_invalid_text_encoding_cells",
        stats.rejection_invalid_text_encoding_cells);
    print_metric(
        "simple_content.rejection_invalid_text_width_cells",
        stats.rejection_invalid_text_width_cells);
    print_metric(
        "simple_content.rejection_multi_cell_text_cells",
        stats.rejection_multi_cell_text_cells);
    print_metric(
        "simple_content.rejection_non_printable_ascii_cells",
        stats.rejection_non_printable_ascii_cells);
    print_metric(
        "simple_content.rejection_non_ascii_text_cells",
        stats.rejection_non_ascii_text_cells);
    print_metric(
        "simple_content.rejection_decoration_cells",
        stats.rejection_decoration_cells);
    print_metric(
        "simple_content.rejection_hyperlink_cells",
        stats.rejection_hyperlink_cells);
}

void print_model_profile_stats(
    const term::Terminal_screen_model_profile_stats& stats)
{
    print_metric("model_profile.print_text_calls", stats.print_text_calls);
    print_metric(
        "model_profile.printable_ascii_span_calls",
        stats.printable_ascii_span_calls);
    print_metric(
        "model_profile.printable_ascii_span_characters",
        stats.printable_ascii_span_characters);
    print_metric(
        "model_profile.printable_ascii_cells_written",
        stats.printable_ascii_cells_written);
    print_metric(
        "model_profile.render_snapshot_cells_scanned",
        stats.render_snapshot_cells_scanned);
    print_metric(
        "model_profile.render_snapshot_cells_emitted",
        stats.render_snapshot_cells_emitted);
    print_metric(
        "model_profile.render_snapshot_rows_built_from_model_storage",
        stats.render_snapshot_rows_built_from_model_storage);
    print_metric(
        "model_profile.render_snapshot_model_row_accessor_borrows",
        stats.render_snapshot_model_row_accessor_borrows);
    print_metric(
        "model_profile.render_snapshot_compact_empty_text_cells",
        stats.render_snapshot_compact_empty_text_cells);
    print_metric(
        "model_profile.render_snapshot_compact_ascii_text_cells",
        stats.render_snapshot_compact_ascii_text_cells);
    print_metric(
        "model_profile.render_snapshot_inline_single_bmp_text_cells",
        stats.render_snapshot_inline_single_bmp_text_cells);
    print_metric(
        "model_profile.render_snapshot_fallback_qstring_copies",
        stats.render_snapshot_fallback_qstring_copies);
    print_metric(
        "model_profile.render_snapshot_fallback_text_code_units_copied",
        stats.render_snapshot_fallback_text_code_units_copied);
    print_metric(
        "model_profile.render_snapshot_fallback_printable_ascii_copies",
        stats.render_snapshot_fallback_printable_ascii_copies);
    print_metric(
        "model_profile.render_snapshot_fallback_other_ascii_copies",
        stats.render_snapshot_fallback_other_ascii_copies);
    print_metric(
        "model_profile.render_snapshot_fallback_single_non_ascii_copies",
        stats.render_snapshot_fallback_single_non_ascii_copies);
    print_metric(
        "model_profile.render_snapshot_fallback_multi_text_copies",
        stats.render_snapshot_fallback_multi_text_copies);
    print_metric(
        "model_profile.max_render_snapshot_fallback_text_units_per_cell",
        stats.max_render_snapshot_fallback_text_units_per_cell);
}

bool validate_observed_dirty_rows(
    const Benchmark_options&                              options,
    const Benchmark_totals&                               totals,
    const term::Terminal_screen_model_profile_stats*      model_profile_stats)
{
    const std::uint64_t requested_dirty_rows =
        static_cast<std::uint64_t>(options.frames) *
        static_cast<std::uint64_t>(options.dirty_rows);
    if (options.dirty_rows >= options.rows) {
        if (totals.snapshot_dirty_rows_visible != requested_dirty_rows ||
            totals.frame_dirty_rows            != requested_dirty_rows ||
            totals.frame_full_dirty_rows       != requested_dirty_rows)
        {
            std::cerr << "full dirty-row validation failed: expected "
                << requested_dirty_rows
                << " rows, observed snapshot="
                << totals.snapshot_dirty_rows_visible
                << " frame=" << totals.frame_dirty_rows
                << " full=" << totals.frame_full_dirty_rows << '\n';
            return false;
        }

        if (model_profile_stats != nullptr) {
            const std::uint64_t profile_dirty_rows =
                model_profile_stats->render_snapshot_dirty_rows_visible;
            const std::uint64_t profile_full_repaint_fallbacks =
                model_profile_stats->render_snapshot_full_repaint_fallbacks;
            if (profile_dirty_rows != requested_dirty_rows ||
                profile_full_repaint_fallbacks != 0U)
            {
                std::cerr << "full model-profile dirty-row validation failed: expected "
                    << requested_dirty_rows
                    << " visible rows and no full repaint fallbacks, observed "
                    << "model_profile.render_snapshot_dirty_rows_visible="
                    << profile_dirty_rows
                    << " model_profile.render_snapshot_full_repaint_fallbacks="
                    << profile_full_repaint_fallbacks
                    << '\n';
                return false;
            }
        }

        return true;
    }

    const std::uint64_t accounted_cursor_rows =
        static_cast<std::uint64_t>(options.frames);
    const std::uint64_t maximum_accounted_dirty_rows =
        requested_dirty_rows + accounted_cursor_rows;
    if (totals.snapshot_dirty_rows_visible < requested_dirty_rows ||
        totals.snapshot_dirty_rows_visible > maximum_accounted_dirty_rows ||
        totals.frame_dirty_rows            < requested_dirty_rows ||
        totals.frame_dirty_rows            > maximum_accounted_dirty_rows ||
        totals.frame_full_dirty_rows       != 0U)
    {
        std::cerr << "sparse dirty-row validation failed: expected "
            << requested_dirty_rows << ".." << maximum_accounted_dirty_rows
            << " rows, observed snapshot="
            << totals.snapshot_dirty_rows_visible
            << " frame=" << totals.frame_dirty_rows
            << " full=" << totals.frame_full_dirty_rows << '\n';
        return false;
    }

    if (model_profile_stats != nullptr) {
        const std::uint64_t profile_dirty_rows =
            model_profile_stats->render_snapshot_dirty_rows_visible;
        const std::uint64_t profile_full_repaint_fallbacks =
            model_profile_stats->render_snapshot_full_repaint_fallbacks;
        if (profile_dirty_rows < requested_dirty_rows ||
            profile_dirty_rows > maximum_accounted_dirty_rows ||
            profile_full_repaint_fallbacks != 0U)
        {
            std::cerr << "sparse model-profile dirty-row validation failed: expected "
                << requested_dirty_rows << ".." << maximum_accounted_dirty_rows
                << " visible rows and no full repaint fallbacks, observed "
                << "model_profile.render_snapshot_dirty_rows_visible="
                << profile_dirty_rows
                << " model_profile.render_snapshot_full_repaint_fallbacks="
                << profile_full_repaint_fallbacks
                << '\n';
            return false;
        }
    }

    return true;
}

}

int main(int argc, char** argv)
{
    const Benchmark_options options = parse_options(argc, argv);
    const term::terminal_grid_size_t grid_size{options.rows, options.columns};

    term::Terminal_screen_model model({grid_size, 0, 8});
    const term::Terminal_render_options render_options;

    const term::terminal_cell_metrics_t metrics = benchmark_cell_metrics();
    const QSizeF logical_size(
        metrics.width * static_cast<qreal>(options.columns),
        metrics.height * static_cast<qreal>(options.rows));

    double ingest_ms = 0.0;
    double snapshot_ms = 0.0;
    double frame_ms = 0.0;
    double total_ms = 0.0;
    Benchmark_totals totals;

#if VNM_TERMINAL_PROFILING_ENABLED
    if (options.model_profile_stats_enabled) {
        model.set_profile_stats_enabled(false);
    }
#endif

    if (options.dirty_rows < options.rows) {
        const std::vector<int> baseline_rows = all_rows(options.rows);
        (void)model.ingest(payload_for_frame(options, 0, baseline_rows));
    }

    const int total_frames = options.warmup_frames + options.frames;
    for (int frame_index = 0; frame_index < total_frames; ++frame_index) {
        if (options.model_profile_stats_enabled &&
            frame_index == options.warmup_frames)
        {
#if VNM_TERMINAL_PROFILING_ENABLED
            model.set_profile_stats_enabled(true);
#endif
        }

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

#if VNM_TERMINAL_PROFILING_ENABLED
    const term::Terminal_screen_model_profile_stats model_profile_stats =
        model.profile_stats();
    const bool model_profile_stats_available =
        options.model_profile_stats_enabled && model_profile_stats.enabled;
#else
    const term::Terminal_screen_model_profile_stats model_profile_stats;
    const bool model_profile_stats_available = false;
#endif

    if (!validate_observed_dirty_rows(
            options,
            totals,
            model_profile_stats_available ? &model_profile_stats : nullptr))
    {
        return 3;
    }

    const double frames = static_cast<double>(options.frames);
    std::cout << "scenario=nelostie_like_model_snapshot_frame\n";
    print_metric("frames", static_cast<std::uint64_t>(options.frames));
    print_metric("warmup_frames", static_cast<std::uint64_t>(options.warmup_frames));
    print_metric("rows", static_cast<std::uint64_t>(options.rows));
    print_metric("columns", static_cast<std::uint64_t>(options.columns));
    print_metric("dirty_rows_requested", static_cast<std::uint64_t>(options.dirty_rows));
    print_metric("dirty_row_stride", static_cast<std::uint64_t>(options.dirty_row_stride));
    print_metric("dirty_row_seed", static_cast<std::uint64_t>(options.dirty_row_seed));
    print_metric("graphics_every", static_cast<std::uint64_t>(options.graphics_every));
    print_metric("style_period", static_cast<std::uint64_t>(options.style_period));
    std::cout << "text_pattern=" << text_pattern_name(options.text_pattern).toUtf8().constData()
        << '\n';
    std::cout << "model_profile_stats_requested="
        << (options.model_profile_stats_enabled ? "true" : "false") << '\n';
    std::cout << "model_profile_stats_available="
        << (model_profile_stats_available ? "true" : "false") << '\n';
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
    print_simple_content_stats(totals.simple_content);
    print_metric(
        "route_qt_text_layout_cells",
        totals.simple_content.route_qt_text_layout_cells);
    print_metric("frame_visible_rows", totals.frame_visible_rows);
    print_metric("frame_dirty_rows", totals.frame_dirty_rows);
    print_metric("frame_full_dirty_rows", totals.frame_full_dirty_rows);
    print_metric("frame_cell_pass_input_cells", totals.frame_cell_pass_input_cells);
    print_metric("frame_cells_considered", totals.frame_cells_considered);
    print_metric("frame_input_cells_considered", totals.frame_cell_pass_input_cells);
    print_metric(
        "frame_cell_pass_classification_calls",
        totals.frame_cell_pass_classification_calls);
    std::cout << "frame_row_descriptor_counters_available=true\n";
    std::cout << "frame_row_descriptor_counter_semantics="
        << k_frame_descriptor_counter_semantics << '\n';
    print_metric("frame_row_descriptors_built", totals.frame_row_descriptors_built);
    print_metric("frame_layer_descriptors_built", totals.frame_layer_descriptors_built);
    print_metric("frame_dirty_row_lookup_count", totals.frame_dirty_row_lookup_count);
    print_metric(
        "frame_dirty_row_range_lookup_count",
        totals.frame_dirty_row_range_lookup_count);
    print_metric(
        "frame_dirty_row_range_scan_steps",
        totals.frame_dirty_row_range_scan_steps);
    print_metric(
        "frame_compact_ascii_text_direct_appends",
        totals.frame_compact_ascii_text_direct_appends);
    print_metric(
        "frame_compact_ascii_qstring_materializations",
        totals.frame_compact_ascii_qstring_materializations);
    print_metric("frame_text_runs_emitted", totals.frame_text_runs_emitted);
    print_metric("frame_graphic_rects_emitted", totals.frame_graphic_rects_emitted);
    if (model_profile_stats_available) {
        print_model_profile_stats(model_profile_stats);
    }
    print_metric("checksum", totals.checksum);
    return 0;
}
