#include "vnm_terminal/vnm_terminal_surface.h"
#include "vnm_terminal/diagnostics/metrics_json.h"
#include "vnm_terminal/font_metrics.h"

int main()
{
    // Prove the installed public diagnostics header is includable and that its
    // builders link from the packaged library, without constructing a surface.
    void (*append_renderer)(const VNM_TerminalSurface&, QJsonObject&) =
        &vnm_terminal::diagnostics::append_renderer_metrics_json;
    void (*append_atlas)(const VNM_TerminalSurface&, QJsonObject&) =
        &vnm_terminal::diagnostics::append_atlas_metrics_json;

    // Prove the installed public font/metrics header is includable and that its
    // functions link from the packaged library.
    QString (*default_family)() = &vnm_terminal::default_monospace_font_family;
    vnm_terminal::Cell_metrics (*metrics_for_font)(const QString&, qreal, qreal) =
        &vnm_terminal::cell_metrics_for_font;
    bool (*metrics_valid)(const vnm_terminal::Cell_metrics&) =
        &vnm_terminal::cell_metrics_valid;

    return (append_renderer != nullptr && append_atlas != nullptr &&
            default_family != nullptr && metrics_for_font != nullptr &&
            metrics_valid != nullptr)
        ? 0
        : 1;
}
