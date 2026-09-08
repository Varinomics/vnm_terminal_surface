#include "vnm_terminal/terminal_canvas_appearance.h"

#include "vnm_terminal/internal/terminal_color_scheme.h"

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

bool terminal_canvas_color_scheme_available(const Terminal_canvas_frame& frame)
{
    if (!frame.color_references ||
        frame.color_references->record_version != k_terminal_canvas_color_references_version ||
        frame.styles.empty() ||
        frame.styles.size() > k_terminal_canvas_max_styles ||
        frame.color_references->styles.size() != frame.styles.size() ||
        frame.color_references->styles.front().foreground != k_terminal_canvas_color_default ||
        frame.color_references->styles.front().background != k_terminal_canvas_color_default)
    {
        return false;
    }
    return std::all_of(
        frame.color_references->styles.begin(),
        frame.color_references->styles.end(),
        [](const auto& refs) {
            return refs.foreground <= k_terminal_canvas_color_rgba &&
                refs.background <= k_terminal_canvas_color_rgba;
        });
}

bool apply_terminal_canvas_color_scheme(
    Terminal_canvas_frame& frame,
    QStringView scheme_name)
{
    const auto* scheme = internal::find_color_scheme(scheme_name);
    if (!scheme || !terminal_canvas_color_scheme_available(frame)) {
        return false;
    }
    const auto colors = internal::make_terminal_color_state(*scheme);
    const auto resolve = [&colors](std::uint16_t ref, quint32 rgba, quint32 fallback) {
        if (ref == k_terminal_canvas_color_rgba) {
            return rgba;
        }
        return ref == k_terminal_canvas_color_default ? fallback : colors.palette_rgba[ref];
    };
    for (std::size_t index = 0; index < frame.styles.size(); ++index) {
        auto& style = frame.styles[index];
        const auto& refs = frame.color_references->styles[index];
        style.foreground_rgba = resolve(
            refs.foreground, style.foreground_rgba, colors.default_foreground_rgba);
        style.background_rgba = resolve(
            refs.background, style.background_rgba, colors.default_background_rgba);
    }
    frame.default_foreground_rgba = colors.default_foreground_rgba;
    frame.default_background_rgba = colors.default_background_rgba;
    frame.cursor_rgba             = colors.cursor_rgba;
    return true;
}

} // namespace vnm_terminal
