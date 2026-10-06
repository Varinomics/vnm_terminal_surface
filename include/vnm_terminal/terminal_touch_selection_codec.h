#pragma once

#include "terminal_touch_selection.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <array>
#include <cmath>
#include <optional>

namespace vnm_terminal::touch_selection_codec {

inline constexpr qsizetype k_max_record_bytes = 40 * 1'024;
inline constexpr qsizetype k_max_copy_text_bytes = 24 * 1'024;

inline std::optional<quint64> counter(const QJsonValue& value)
{
    if (!value.isString()) {
        return std::nullopt;
    }
    const QString text = value.toString();
    bool ok = false;
    const quint64 number = text.toULongLong(&ok);
    return ok && QString::number(number) == text
        ? std::optional<quint64>(number) : std::nullopt;
}

inline std::optional<int> integer(const QJsonValue& value, int minimum, int maximum)
{
    if (!value.isDouble()) {
        return std::nullopt;
    }
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::trunc(number) != number ||
        number < minimum || number > maximum)
    {
        return std::nullopt;
    }
    return static_cast<int>(number);
}

inline QJsonObject encode_source(const vnm_terminal::terminal_selection_source_t& source)
{
    return {
        {QStringLiteral("session_epoch"), QString::number(source.session_epoch)},
        {QStringLiteral("buffer"), static_cast<int>(source.buffer)},
        {QStringLiteral("reflow_generation"), QString::number(source.grid_reflow_generation)},
        {QStringLiteral("rows"), source.rows},
        {QStringLiteral("columns"), source.columns},
    };
}

inline std::optional<vnm_terminal::terminal_selection_source_t> decode_source(const QJsonValue& value)
{
    if (!value.isObject()) {
        return std::nullopt;
    }
    const auto object = value.toObject();
    const auto epoch = counter(object.value(QStringLiteral("session_epoch")));
    const auto reflow = counter(object.value(QStringLiteral("reflow_generation")));
    const auto buffer = integer(object.value(QStringLiteral("buffer")), 0, 1);
    const auto rows = integer(object.value(QStringLiteral("rows")), 1, 32'768);
    const auto columns = integer(object.value(QStringLiteral("columns")), 1, 32'768);
    if (object.size() != 5 || !epoch || !*epoch || !reflow || !buffer || !rows || !columns) {
        return std::nullopt;
    }
    return vnm_terminal::terminal_selection_source_t{
        *epoch, static_cast<vnm_terminal::Terminal_selection_buffer>(*buffer), *reflow, *rows, *columns};
}

inline QJsonArray encode_row(const vnm_terminal::terminal_selection_row_t& row)
{
    return {QString::number(row.retained_line_id), QString::number(row.content_generation),
        row.visual_fragment_index};
}

inline std::optional<vnm_terminal::terminal_selection_row_t> decode_row(const QJsonValue& value)
{
    if (!value.isArray()) {
        return std::nullopt;
    }
    const auto array = value.toArray();
    if (array.size() != 3) {
        return std::nullopt;
    }
    const auto id = counter(array[0]);
    const auto generation = counter(array[1]);
    const auto fragment = integer(array[2], 0, 1'048'576);
    if (!id || !generation || !fragment) {
        return std::nullopt;
    }
    return vnm_terminal::terminal_selection_row_t{*id, *generation, *fragment};
}

inline constexpr auto k_word_query_presence = "terminal.selection.word_query";

inline QJsonObject word_query_marker(std::uint16_t version = k_terminal_word_query_version)
{
    return {{QStringLiteral("presence"), QLatin1String(k_word_query_presence)},
        {QStringLiteral("version"), version}};
}

inline std::optional<std::uint16_t> decode_word_query_marker(const QJsonValue& value)
{
    const auto object = value.toObject();
    const auto version = integer(object.value(QStringLiteral("version")), 1, 65'535);
    if (object.size() != 2 || object.value(QStringLiteral("presence")).toString() !=
            QLatin1String(k_word_query_presence) || !version)
    {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(*version);
}

inline constexpr std::array<const char*, 9> k_actions{
    "select_word", "begin_handle", "move_handle", "scroll_extend",
    "end_gesture", "cancel_gesture", "clear", "copy", "inspect_word"};
inline constexpr std::array<const char*, 7> k_statuses{
    "ok", "stale_source", "stale_selection", "unavailable", "over_limit", "invalid_request", "indeterminate"};

template<std::size_t N>
inline std::optional<int> enum_index(const QJsonValue& value, const std::array<const char*, N>& names)
{
    if (!value.isString()) {
        return std::nullopt;
    }
    for (std::size_t index = 0; index < N; ++index) {
        if (value.toString() == QLatin1String(names[index])) {
            return static_cast<int>(index);
        }
    }
    return std::nullopt;
}

inline QJsonObject encode_request(const vnm_terminal::Terminal_touch_selection_request& request)
{
    const auto action = static_cast<std::size_t>(request.action);
    if (action >= k_actions.size()) {
        return {};
    }
    QJsonObject object{
        {QStringLiteral("request_id"), QString::number(request.request_id)},
        {QStringLiteral("gesture_id"), request.gesture_id},
        {QStringLiteral("action"), QLatin1String(k_actions[action])},
        {QStringLiteral("source"), encode_source(request.source)},
        {QStringLiteral("row"), encode_row(request.target.row)},
        {QStringLiteral("column"), request.target.column},
        {QStringLiteral("handle"), static_cast<int>(request.handle)},
        {QStringLiteral("selection_generation"), QString::number(request.expected_selection_generation)},
        {QStringLiteral("scroll_lines"), request.scroll_lines},
    };
    if (request.action == Terminal_touch_selection_action::INSPECT_WORD) {
        object.insert(QStringLiteral("word_query"), word_query_marker());
    }
    return object;
}

inline std::optional<vnm_terminal::Terminal_touch_selection_request> decode_request(const QJsonValue& value)
{
    if (!value.isObject()) {
        return std::nullopt;
    }
    const auto object = value.toObject();
    const auto id = counter(object.value(QStringLiteral("request_id")));
    const auto generation = counter(object.value(QStringLiteral("selection_generation")));
    const auto source = decode_source(object.value(QStringLiteral("source")));
    const auto row = decode_row(object.value(QStringLiteral("row")));
    const auto action = enum_index(object.value(QStringLiteral("action")), k_actions);
    const auto column = integer(object.value(QStringLiteral("column")), 0, source ? source->columns : 0);
    const auto handle = integer(object.value(QStringLiteral("handle")), 0, 1);
    const auto scroll = integer(object.value(QStringLiteral("scroll_lines")), -64, 64);
    const auto gesture = object.value(QStringLiteral("gesture_id"));
    const bool query = action && *action == static_cast<int>(Terminal_touch_selection_action::INSPECT_WORD);
    if (object.size() != (query ? 10 : 9) ||
        (query && decode_word_query_marker(object.value(QStringLiteral("word_query"))) != k_terminal_word_query_version) ||
        !id || !*id || !generation || !source || !row || !action ||
        !column || !handle || !scroll || !gesture.isString() || gesture.toString().isEmpty() ||
        gesture.toString().toUtf8().size() > 256)
    {
        return std::nullopt;
    }
    return vnm_terminal::Terminal_touch_selection_request{
        *id, gesture.toString(), static_cast<vnm_terminal::Terminal_touch_selection_action>(*action),
        *source, {*row, *column}, static_cast<vnm_terminal::Terminal_selection_handle>(*handle), *generation, *scroll};
}

inline QJsonObject encode_result(vnm_terminal::Terminal_touch_selection_result result)
{
    const auto action = static_cast<std::size_t>(result.action);
    auto status = static_cast<std::size_t>(result.status);
    if (action >= k_actions.size() || status >= k_statuses.size()) {
        return {};
    }
    if (result.action != vnm_terminal::Terminal_touch_selection_action::COPY ||
        result.status != vnm_terminal::Terminal_touch_selection_status::OK)
    {
        result.text.clear();
    }
    const QByteArray text = result.text.toUtf8();
    if (text.size() > k_max_copy_text_bytes || QString::fromUtf8(text) != result.text) {
        result.status = vnm_terminal::Terminal_touch_selection_status::OVER_LIMIT;
        result.text.clear();
        result.reason = QStringLiteral("Selected text exceeds the companion copy limit.");
    }
    status = static_cast<std::size_t>(result.status);
    QJsonObject object{
        {QStringLiteral("request_id"), QString::number(result.request_id)},
        {QStringLiteral("action"), QLatin1String(k_actions[action])},
        {QStringLiteral("status"), QLatin1String(k_statuses[status])},
        {QStringLiteral("selection_generation"), QString::number(result.selection_generation)},
        {QStringLiteral("text"), result.text},
        {QStringLiteral("reason"), result.reason.left(512)},
    };
    if (result.action == Terminal_touch_selection_action::INSPECT_WORD) {
        object.insert(QStringLiteral("word_query"), word_query_marker());
        object.insert(QStringLiteral("word_available"), result.word_available
            ? QJsonValue(*result.word_available) : QJsonValue(QJsonValue::Null));
    }
    if (QJsonDocument(object).toJson(QJsonDocument::Compact).size() > k_max_record_bytes) {
        object.insert(QStringLiteral("status"), QStringLiteral("over_limit"));
        object.insert(QStringLiteral("text"), QString{});
        object.insert(QStringLiteral("reason"), QStringLiteral("Selected text exceeds the companion copy limit."));
    }
    return object;
}

inline std::optional<vnm_terminal::Terminal_touch_selection_result> decode_result(const QJsonValue& value)
{
    if (!value.isObject()) {
        return std::nullopt;
    }
    const auto object = value.toObject();
    const auto id = counter(object.value(QStringLiteral("request_id")));
    const auto generation = counter(object.value(QStringLiteral("selection_generation")));
    const auto action = enum_index(object.value(QStringLiteral("action")), k_actions);
    const auto status = enum_index(object.value(QStringLiteral("status")), k_statuses);
    const auto text = object.value(QStringLiteral("text"));
    const auto reason = object.value(QStringLiteral("reason"));
    const bool query = action && *action == static_cast<int>(Terminal_touch_selection_action::INSPECT_WORD);
    const auto available = object.value(QStringLiteral("word_available"));
    if (object.size() != (query ? 8 : 6) ||
        (query && (decode_word_query_marker(object.value(QStringLiteral("word_query"))) != k_terminal_word_query_version ||
            (!available.isBool() && !available.isNull()) ||
            (status && *status == 0 && !available.isBool()))) ||
        !id || !*id || !generation || !action || !status ||
        !text.isString() || !reason.isString() || text.toString().toUtf8().size() > k_max_copy_text_bytes ||
        reason.toString().size() > 512 || QJsonDocument(object).toJson(QJsonDocument::Compact).size() > k_max_record_bytes)
    {
        return std::nullopt;
    }
    if (!text.toString().isEmpty() && (*action != 7 || *status != 0)) {
        return std::nullopt;
    }
    return vnm_terminal::Terminal_touch_selection_result{
        *id, static_cast<vnm_terminal::Terminal_touch_selection_action>(*action),
        static_cast<vnm_terminal::Terminal_touch_selection_status>(*status), *generation,
        text.toString(), reason.toString(),
        query && available.isBool() ? std::optional(available.toBool()) : std::nullopt};
}

inline QJsonObject encode_frame(const vnm_terminal::Terminal_canvas_selection& selection)
{
    QJsonArray rows;
    QJsonArray spans;
    for (const auto& row : selection.visible_rows) {
        rows.append(encode_row(row));
    }
    for (const auto& span : selection.spans) {
        spans.append(QJsonArray{span.row, span.first_column, span.column_count});
    }
    const auto endpoint = [](const auto& point) {
        return QJsonArray{point.row, point.column, point.visible};
    };
    QJsonObject object{
        {QStringLiteral("version"), selection.record_version},
        {QStringLiteral("source"), encode_source(selection.source)},
        {QStringLiteral("rows"), rows},
        {QStringLiteral("selection_generation"), QString::number(selection.selection_generation)},
        {QStringLiteral("has_selection"), selection.has_selection},
        {QStringLiteral("touch_handles_visible"), selection.touch_handles_visible},
        {QStringLiteral("start"), endpoint(selection.start)},
        {QStringLiteral("end"), endpoint(selection.end)},
        {QStringLiteral("spans"), spans},
        {QStringLiteral("background_rgba"), static_cast<qint64>(selection.background_rgba)},
        {QStringLiteral("foreground_rgba"), static_cast<qint64>(selection.foreground_rgba)},
    };
    return QJsonDocument(object).toJson(QJsonDocument::Compact).size() <= k_max_record_bytes ? object : QJsonObject{};
}

inline std::optional<vnm_terminal::Terminal_canvas_selection> decode_frame(const QJsonValue& value)
{
    if (!value.isObject()) {
        return std::nullopt;
    }
    const auto object = value.toObject();
    const auto source = decode_source(object.value(QStringLiteral("source")));
    const auto generation = counter(object.value(QStringLiteral("selection_generation")));
    if (object.size() != 11 || object.value(QStringLiteral("version")).toInt(-1) != 1 ||
        !source || !generation || !object.value(QStringLiteral("rows")).isArray() ||
        !object.value(QStringLiteral("spans")).isArray() ||
        !object.value(QStringLiteral("has_selection")).isBool() ||
        !object.value(QStringLiteral("touch_handles_visible")).isBool() ||
        QJsonDocument(object).toJson(QJsonDocument::Compact).size() > k_max_record_bytes)
    {
        return std::nullopt;
    }
    vnm_terminal::Terminal_canvas_selection result;
    result.source = *source;
    result.selection_generation = *generation;
    result.has_selection = object.value(QStringLiteral("has_selection")).toBool();
    result.touch_handles_visible = object.value(QStringLiteral("touch_handles_visible")).toBool();
    const auto rows = object.value(QStringLiteral("rows")).toArray();
    if (rows.size() != source->rows) {
        return std::nullopt;
    }
    for (const auto& encoded : rows) {
        const auto row = decode_row(encoded);
        if (!row) {
            return std::nullopt;
        }
        result.visible_rows.push_back(*row);
    }
    const auto endpoint = [&](const QJsonValue& encoded)
        -> std::optional<vnm_terminal::terminal_canvas_selection_endpoint_t>
    {
        const auto array = encoded.toArray();
        if (array.size() != 3 || !array[2].isBool()) {
            return std::nullopt;
        }
        const bool visible = array[2].toBool();
        const auto row = integer(array[0], visible ? 0 : -32'768, visible ? source->rows - 1 : 32'768);
        const auto column = integer(array[1], visible ? 0 : -32'768, visible ? source->columns : 32'768);
        if (!row || !column) {
            return std::nullopt;
        }
        return vnm_terminal::terminal_canvas_selection_endpoint_t{*row, *column, array[2].toBool()};
    };
    const auto start = endpoint(object.value(QStringLiteral("start")));
    const auto end = endpoint(object.value(QStringLiteral("end")));
    if (!start || !end) {
        return std::nullopt;
    }
    result.start = *start;
    result.end = *end;
    const auto spans = object.value(QStringLiteral("spans")).toArray();
    if (spans.size() > source->rows) {
        return std::nullopt;
    }
    int previous_row = -1;
    for (const auto& encoded : spans) {
        const auto array = encoded.toArray();
        if (array.size() != 3) {
            return std::nullopt;
        }
        const auto row = integer(array[0], previous_row + 1, source->rows - 1);
        const auto first = integer(array[1], 0, source->columns - 1);
        const auto count = integer(array[2], 1, first ? source->columns - *first : 0);
        if (!row || !first || !count) {
            return std::nullopt;
        }
        previous_row = *row;
        result.spans.push_back({*row, *first, *count});
    }
    for (const auto* name : {"background_rgba", "foreground_rgba"}) {
        const auto encoded = object.value(QLatin1String(name));
        const double number = encoded.toDouble(-1);
        if (!encoded.isDouble() || !std::isfinite(number) || std::trunc(number) != number ||
            number < 0 || number > 4'294'967'295.0)
        {
            return std::nullopt;
        }
        (QLatin1String(name) == QLatin1String("background_rgba")
            ? result.background_rgba : result.foreground_rgba) = static_cast<quint32>(number);
    }
    return vnm_terminal::terminal_canvas_selection_is_valid(result, source->rows, source->columns)
        ? std::optional(result) : std::nullopt;
}

} // namespace vnm_terminal::touch_selection_codec
