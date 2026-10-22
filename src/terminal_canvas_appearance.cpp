#include "vnm_terminal/terminal_canvas_appearance.h"

#include "vnm_terminal/internal/terminal_color_scheme.h"

#include "vnm_terminal/internal/qsg_terminal_render_frame.h"

#include <QColor>
#include <QVariantList>

#include <algorithm>

namespace vnm_terminal {

QStringList terminal_canvas_color_scheme_names()
{
    QStringList names;
    for (const auto& scheme : internal::builtin_color_schemes()) {
        names.append(scheme.name);
    }
    return names;
}

QString terminal_canvas_default_color_scheme_name()
{
    return internal::default_color_scheme().name;
}

QVariantMap terminal_color_scheme_preview(QStringView scheme_name)
{
    const auto* scheme = internal::find_color_scheme(scheme_name);
    if (scheme == nullptr) {
        return {};
    }

    QVariantList ansi;
    ansi.reserve(static_cast<int>(scheme->ansi_palette_rgba.size()));
    for (quint32 rgba : scheme->ansi_palette_rgba) {
        ansi.push_back(QColor::fromRgba(rgba));
    }

    QVariantMap preview;
    preview.insert(QStringLiteral("name"),       scheme->name);
    preview.insert(QStringLiteral("background"), QColor::fromRgba(scheme->background_rgba));
    preview.insert(QStringLiteral("foreground"), QColor::fromRgba(scheme->foreground_rgba));
    preview.insert(QStringLiteral("cursor"),     QColor::fromRgba(scheme->cursor_rgba));
    const QColor selection_background = QColor::fromRgba(scheme->selection_rgba);
    preview.insert(QStringLiteral("selection"), selection_background);
    preview.insert(
        QStringLiteral("selection_foreground"),
        internal::terminal_selection_foreground_for_background(selection_background));
    preview.insert(QStringLiteral("ansi"),       ansi);
    return preview;
}

namespace {

bool style_color_references_available(
    const std::vector<Terminal_canvas_style>& styles,
    const std::optional<Terminal_canvas_color_references>& references)
{
    if (!references ||
        references->record_version != k_terminal_canvas_color_references_version ||
        styles.empty() || references->styles.size() != styles.size() ||
        references->styles.front().foreground != k_terminal_canvas_color_default ||
        references->styles.front().background != k_terminal_canvas_color_default)
    {
        return false;
    }
    return std::all_of(references->styles.begin(), references->styles.end(), [](const auto& refs) {
        return refs.foreground <= k_terminal_canvas_color_rgba &&
            refs.background <= k_terminal_canvas_color_rgba;
    });
}

bool has_available_coverage(const Terminal_canvas_frame& frame)
{
    return frame.row_coverage &&
        frame.row_coverage->record_version == k_terminal_canvas_row_coverage_version &&
        frame.row_coverage->status == Terminal_canvas_row_coverage_status::AVAILABLE;
}

} // namespace

bool terminal_canvas_color_scheme_available(const Terminal_canvas_frame& frame)
{
    // Core frame admission also uses this native-table predicate. Optional
    // coverage selectors must not make an otherwise renderable frame invalid.
    return frame.styles.size() <= terminal_canvas_style_limit(frame) &&
        style_color_references_available(frame.styles, frame.color_references);
}

bool apply_terminal_canvas_color_scheme(
    Terminal_canvas_frame& frame,
    QStringView scheme_name)
{
    const auto* scheme = internal::find_color_scheme(scheme_name);
    if (!scheme || !terminal_canvas_color_scheme_available(frame) ||
        (has_available_coverage(frame) && !style_color_references_available(
            frame.row_coverage->styles, frame.row_coverage->color_references)))
    {
        return false;
    }
    const auto colors = internal::make_terminal_color_state(*scheme);
    const auto resolve = [&colors](std::uint16_t ref, quint32 rgba, quint32 fallback) {
        if (ref == k_terminal_canvas_color_rgba) {
            return rgba;
        }
        return ref == k_terminal_canvas_color_default ? fallback : colors.palette_rgba[ref];
    };
    const auto apply_styles = [&resolve, &colors](
        std::vector<Terminal_canvas_style>& styles,
        const Terminal_canvas_color_references& references)
    {
        for (std::size_t index = 0; index < styles.size(); ++index) {
            auto& style = styles[index];
            const auto& refs = references.styles[index];
            style.foreground_rgba = resolve(
                refs.foreground, style.foreground_rgba, colors.default_foreground_rgba);
            style.background_rgba = resolve(
                refs.background, style.background_rgba, colors.default_background_rgba);
        }
    };
    // Validate both tables before changing either: they share one displayed palette.
    apply_styles(frame.styles, *frame.color_references);
    if (has_available_coverage(frame)) {
        apply_styles(frame.row_coverage->styles, *frame.row_coverage->color_references);
    }
    frame.default_foreground_rgba = colors.default_foreground_rgba;
    frame.default_background_rgba = colors.default_background_rgba;
    frame.cursor_rgba             = colors.cursor_rgba;
    return true;
}

} // namespace vnm_terminal
