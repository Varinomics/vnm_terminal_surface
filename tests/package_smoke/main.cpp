#include "vnm_terminal/vnm_terminal_surface.h"
#include "vnm_terminal/diagnostics/metrics_json.h"

int main()
{
    // Prove the installed public diagnostics header is includable and that its
    // builders link from the packaged library, without constructing a surface.
    void (*append_renderer)(const VNM_TerminalSurface&, QJsonObject&) =
        &vnm_terminal::diagnostics::append_renderer_metrics_json;
    void (*append_atlas)(const VNM_TerminalSurface&, QJsonObject&) =
        &vnm_terminal::diagnostics::append_atlas_metrics_json;
    return (append_renderer != nullptr && append_atlas != nullptr) ? 0 : 1;
}
