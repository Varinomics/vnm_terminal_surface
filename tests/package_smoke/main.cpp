#include "vnm_terminal/vnm_terminal_surface.h"
#include "vnm_terminal/diagnostics/metrics_json.h"
#include "vnm_terminal/font_metrics.h"
#include "vnm_terminal/terminal_canvas_export.h"
#include "vnm_terminal/vnm_terminal_canvas.h"

#include <optional>
#include <utility>
#include <vector>

namespace {

vnm_terminal::Terminal_process_start_result invoke_standalone_boundary(
    VNM_TerminalSurface& surface,
    QString executable,
    QString working_directory,
    std::vector<vnm_terminal::Terminal_environment_entry> base_environment)
{
    return surface.start_terminal({
        {std::move(executable)},
        std::move(working_directory),
        std::move(base_environment),
        std::nullopt,
    });
}

vnm_terminal::Terminal_process_start_result invoke_worker_boundary(
    VNM_TerminalSurface& surface,
    QString executable,
    QString working_directory,
    std::vector<vnm_terminal::Terminal_environment_entry> base_environment,
    std::vector<vnm_terminal::Terminal_environment_entry> contribution)
{
    return surface.start_terminal({
        {std::move(executable)},
        std::move(working_directory),
        std::move(base_environment),
        std::move(contribution),
    });
}

} // namespace

int main()
{
    // Prove the installed public diagnostics header is includable and that its
    // builders link from the packaged library, without constructing a surface.
    void (*append_atlas)(const VNM_TerminalSurface&, QJsonObject&) =
        &vnm_terminal::diagnostics::append_atlas_metrics_json;
    void (*append_render_invalidation)(const VNM_TerminalSurface&, QJsonObject&) =
        &vnm_terminal::diagnostics::append_render_invalidation_metrics_json;
    void (*append_backend_drain)(const VNM_TerminalSurface&, QJsonObject&) =
        &vnm_terminal::diagnostics::append_backend_drain_metrics_json;

    // Prove the installed public font/metrics header is includable and that its
    // functions link from the packaged library.
    QString (*default_family)() = &vnm_terminal::default_monospace_font_family;
    vnm_terminal::Cell_metrics (*metrics_for_font)(const QString&, qreal, qreal) =
        &vnm_terminal::cell_metrics_for_font;
    bool (*metrics_valid)(const vnm_terminal::Cell_metrics&) =
        &vnm_terminal::cell_metrics_valid;
    const auto standalone_boundary = &invoke_standalone_boundary;
    const auto worker_boundary = &invoke_worker_boundary;
    vnm_terminal::Terminal_canvas_export_result (*export_canvas)(
        const VNM_TerminalSurface&) =
            &vnm_terminal::export_terminal_canvas_frame;
    bool (VNM_TerminalCanvas::*set_canvas_frame)(
        std::shared_ptr<const vnm_terminal::Terminal_canvas_frame>) =
            &VNM_TerminalCanvas::set_canvas_frame;

    return (append_atlas != nullptr &&
            append_render_invalidation != nullptr &&
            append_backend_drain != nullptr &&
            default_family != nullptr && metrics_for_font != nullptr &&
            metrics_valid != nullptr && standalone_boundary != nullptr &&
            worker_boundary != nullptr && export_canvas != nullptr &&
            set_canvas_frame != nullptr)
        ? 0
        : 1;
}
