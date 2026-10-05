#include "helpers/test_check.h"
#include "vnm_terminal/terminal_renderer_surface_format.h"

#include <QGuiApplication>
#include <QOpenGLContext>
#include <QSurfaceFormat>

#include <iostream>
#include <string>

namespace {

using vnm_terminal::test_helpers::check;

// Every option a host may have chosen besides the version; the minimum must
// leave all of them as requested.
QSurfaceFormat requested_format(
    QSurfaceFormat::RenderableType renderable_type,
    int                            major_version,
    int                            minor_version)
{
    QSurfaceFormat format;
    format.setRenderableType(renderable_type);
    format.setVersion(major_version, minor_version);
    format.setProfile(QSurfaceFormat::CompatibilityProfile);
    format.setOption(QSurfaceFormat::DebugContext);
    format.setRedBufferSize(8);
    format.setGreenBufferSize(8);
    format.setBlueBufferSize(8);
    format.setAlphaBufferSize(8);
    format.setDepthBufferSize(24);
    format.setStencilBufferSize(8);
    format.setSamples(4);
    format.setSwapInterval(0);
    format.setSwapBehavior(QSurfaceFormat::TripleBuffer);
    return format;
}

std::string describe(const QSurfaceFormat& format)
{
    return "renderable=" + std::to_string(int(format.renderableType())) +
        " version=" + std::to_string(format.majorVersion()) + "." +
        std::to_string(format.minorVersion()) +
        " profile=" + std::to_string(int(format.profile())) +
        " options=" + std::to_string(format.options().toInt()) +
        " depth=" + std::to_string(format.depthBufferSize()) +
        " stencil=" + std::to_string(format.stencilBufferSize()) +
        " samples=" + std::to_string(format.samples()) +
        " swap_interval=" + std::to_string(format.swapInterval());
}

bool check_applied(
    const char*           case_name,
    const QSurfaceFormat& requested,
    int                   expected_major_version,
    int                   expected_minor_version)
{
    QSurfaceFormat::setDefaultFormat(requested);
    vnm_terminal::apply_terminal_renderer_minimum_surface_format();

    QSurfaceFormat expected = requested;
    expected.setVersion(expected_major_version, expected_minor_version);
    const QSurfaceFormat actual = QSurfaceFormat::defaultFormat();
    if (actual == expected) {
        return true;
    }

    std::cerr << "  " << case_name << ": expected " << describe(expected)
              << ", got " << describe(actual) << '\n';
    return check(false, case_name);
}

} // namespace

int main(int argc, char** argv)
{
    // The module type that decides an unspecified renderable type is a
    // platform query, so the minimum is applied after the application exists,
    // as every host does.
    QGuiApplication application(argc, argv);
    bool            ok = true;

    ok &= check_applied(
        "desktop OpenGL 2.0 is raised to 3.3",
        requested_format(QSurfaceFormat::OpenGL, 2, 0), 3, 3);
    ok &= check_applied(
        "desktop OpenGL 3.0 is raised to 3.3",
        requested_format(QSurfaceFormat::OpenGL, 3, 0), 3, 3);
    ok &= check_applied(
        "desktop OpenGL 3.3 is kept",
        requested_format(QSurfaceFormat::OpenGL, 3, 3), 3, 3);
    ok &= check_applied(
        "desktop OpenGL 4.6 is kept",
        requested_format(QSurfaceFormat::OpenGL, 4, 6), 4, 6);

    ok &= check_applied(
        "OpenGL ES 2.0 is raised to 3.0",
        requested_format(QSurfaceFormat::OpenGLES, 2, 0), 3, 0);
    ok &= check_applied(
        "OpenGL ES 3.0 is kept",
        requested_format(QSurfaceFormat::OpenGLES, 3, 0), 3, 0);
    ok &= check_applied(
        "OpenGL ES 3.2 is kept",
        requested_format(QSurfaceFormat::OpenGLES, 3, 2), 3, 2);

    const bool platform_uses_gles =
        QOpenGLContext::openGLModuleType() == QOpenGLContext::LibGLES;
    ok &= check_applied(
        "an unspecified renderable type follows the platform's OpenGL module",
        requested_format(QSurfaceFormat::DefaultRenderableType, 2, 0),
        3, platform_uses_gles ? 0 : 3);

    ok &= check_applied(
        "Qt's default format is raised",
        QSurfaceFormat(), 3, platform_uses_gles ? 0 : 3);

    return ok ? 0 : 1;
}
