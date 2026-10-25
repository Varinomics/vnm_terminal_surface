# vnm_terminal_surface

[![CI Linux](https://github.com/Varinomics/vnm_terminal_surface/actions/workflows/ci-linux.yml/badge.svg?branch=master)](https://github.com/Varinomics/vnm_terminal_surface/actions/workflows/ci-linux.yml) [![CI Windows](https://github.com/Varinomics/vnm_terminal_surface/actions/workflows/ci-windows.yml/badge.svg?branch=master)](https://github.com/Varinomics/vnm_terminal_surface/actions/workflows/ci-windows.yml) [![CI macOS](https://github.com/Varinomics/vnm_terminal_surface/actions/workflows/ci-macos.yml/badge.svg?branch=master)](https://github.com/Varinomics/vnm_terminal_surface/actions/workflows/ci-macos.yml)

`vnm_terminal_surface` embeds an interactive terminal in a Qt Quick application.
Its C++ `QQuickItem`, `VNM_TerminalSurface`, runs a child process through ConPTY
on Windows or a PTY on Linux and macOS, handles terminal input and output, and
renders the terminal through the Qt Scene Graph. It ships in Varinomics products
and can be used by other Qt Quick hosts.

For a complete terminal application and screenshots, see
[`vnm_terminal`](https://github.com/Varinomics/vnm_terminal).

## Features

- Interactive shells and terminal applications, with alternate screen,
  scrollback, keyboard and mouse reporting, bracketed paste, selection, and
  clipboard support.
- Sixel images that scroll into history with their terminal rows.
- Unicode cell-width handling and Qt input-method support for text composition.
- GPU text rendering with glyph and MSDF paths, configurable fonts and colors,
  and LCD subpixel modes.

The host owns the surrounding application UI and its interaction policies.

## Requirements

- Qt 6.12 or newer, including its Gui and Quick private development modules
- A C++20 compiler
- CMake 3.25 or newer

Native process hosting supports Windows x64, Linux x86_64, and macOS. Windows
uses a ConPTY runtime that CMake downloads and verifies during configuration;
[Windows build instructions](docs/repository_guide.md#local-windows-build)
cover offline packages and distribution inputs.

## Use In A Qt Quick Host

Clone this repository beside your host project. After creating the host's Qt
Quick executable target, add and link the component:

```cmake
add_subdirectory("../vnm_terminal_surface" "${CMAKE_BINARY_DIR}/_deps/vnm_terminal_surface")
target_link_libraries(my_terminal PRIVATE
    vnm_terminal_surface::vnm_terminal_surface)

if(WIN32)
    vnm_terminal_deploy_conpty(my_terminal)
endif()
```

Once the host's `QGuiApplication` exists, call
`vnm_terminal::apply_terminal_renderer_minimum_surface_format()` before creating
its first window. Create a `VNM_TerminalSurface` under the window's content item
or expose it to QML, size it, and call `start_terminal()` with the executable,
arguments, absolute working directory, and explicit child environment. The
[public surface reference](docs/public_surface.md) describes the launch request
and host operations. POSIX hosts can optionally supply a process owner for
application-specific lifecycle requirements; ordinary terminal launch needs no
owner implementation from the host.

On Windows, call the deployment helper for each executable that hosts a
terminal. For installed packages, custom runtime destinations, and distribution
installation, see [CMake consumers](docs/repository_guide.md#cmake-consumers).

## Documentation

- [Build and packaging guide](docs/repository_guide.md) — build options,
  installation, Windows runtime packaging, and tests.
- [Public surface reference](docs/public_surface.md) — properties, process
  launch, input, selection, scrolling, and signals.
- [Terminal sequence matrix](docs/terminal_sequence_matrix.md) — supported
  terminal behavior.
- [Documentation index](docs/README.md) — architecture, rendering, Unicode,
  diagnostics, and other reference material.

## License

GNU General Public License version 3 only. See [LICENSE](LICENSE) and
[third-party notices](THIRD_PARTY_NOTICES.md).
