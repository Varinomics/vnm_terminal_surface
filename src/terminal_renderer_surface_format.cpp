#include "vnm_terminal/terminal_renderer_surface_format.h"

#include <QOpenGLContext>
#include <QPair>
#include <QSurfaceFormat>

namespace vnm_terminal {

void apply_terminal_renderer_minimum_surface_format()
{
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    const bool uses_gles =
        format.renderableType() == QSurfaceFormat::OpenGLES ||
        (format.renderableType() == QSurfaceFormat::DefaultRenderableType &&
            QOpenGLContext::openGLModuleType() == QOpenGLContext::LibGLES);
    const QPair<int, int> minimum_version(3, uses_gles ? 0 : 3);
    if (format.version() >= minimum_version) {
        return;
    }

    format.setVersion(minimum_version.first, minimum_version.second);
    QSurfaceFormat::setDefaultFormat(format);
}

} // namespace vnm_terminal
