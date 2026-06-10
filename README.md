# vnm_terminal_surface

[![CI Linux](https://github.com/Varinomics/vnm_terminal_surface/actions/workflows/ci-linux.yml/badge.svg?branch=master)](https://github.com/Varinomics/vnm_terminal_surface/actions/workflows/ci-linux.yml) [![CI Windows](https://github.com/Varinomics/vnm_terminal_surface/actions/workflows/ci-windows.yml/badge.svg?branch=master)](https://github.com/Varinomics/vnm_terminal_surface/actions/workflows/ci-windows.yml) [![CI macOS](https://github.com/Varinomics/vnm_terminal_surface/actions/workflows/ci-macos.yml/badge.svg?branch=master)](https://github.com/Varinomics/vnm_terminal_surface/actions/workflows/ci-macos.yml)

`vnm_terminal_surface` is a BSD-licensed Qt Quick terminal component. It embeds
a real interactive terminal, backed by a ConPTY (Windows) or PTY (Linux, macOS)
child process, in a Qt Quick application as a single C++ `QQuickItem`:
`VNM_TerminalSurface`. It was built for and ships in Varinomics products, and
the same component is usable by any Qt Quick host.

## Highlights

- Real terminal behavior, not a log viewer: alternate screen, cursor
  addressing, scrollback, keyboard and mouse reporting modes, bracketed paste,
  selection, clipboard policy, and terminal replies.
- Native process hosting per platform: ConPTY on Windows and PTY on Linux and
  macOS, with resize propagation and process lifecycle signals.
- GPU text rendering through the Qt Scene Graph: a glyph-atlas render node
  with MSDF and glyph raster paths and LCD subpixel modes.
- A documented public API (`docs/public_surface.md`), typed scroll
  diagnostics, and a public diagnostics serialization API.
- Continuously tested on Windows, Linux, and macOS: contract, model, backend,
  renderer, conformance, randomized-parser, and lifecycle test families.

## Requirements

- Qt 6.7 or newer
- A C++20 compiler
- CMake 3.21 or newer

## Start Here

For a first pass through the repository, read these in order:

1. [Developer orientation](docs/developer_orientation.md) - the shortest stable explanation
   of what the project is, where the important code lives, and how the pieces
   fit together.
2. [Architecture](docs/architecture.md) - the runtime model, ownership
   boundaries, and data flow.
3. [Public surface](docs/public_surface.md) - the `VNM_TerminalSurface` API.
4. [Repository guide](docs/repository_guide.md) - build targets, test families,
   generated/provenance material, and common entry points.

The [documentation index](docs/README.md) has a time-budgeted reading path and
links to the reference material.

## Build On Windows

Configure once:

```powershell
cmake -S . -B build -DBUILD_TESTING=ON
```

Project tests are configured only when both CTest's `BUILD_TESTING` switch and
`VNM_TERMINAL_SURFACE_BUILD_TESTING` are `ON`. The project-specific gate defaults
to `ON` for a top-level checkout and `OFF` when embedded as a subproject.

Diagnostic transcript capture/replay is compiled out by default. Enable it only
for local diagnostic builds with
`-DVNM_TERMINAL_ENABLE_TRANSCRIPT_CAPTURE_REPLAY=ON`; distribution builds must
use `-DVNM_TERMINAL_DISTRIBUTION_BUILD=ON` and leave transcript capture/replay
disabled.

Build from an x64 MSVC Developer Command Prompt or another shell where the
Visual Studio C++ environment has already been initialized:

```bat
cmake --build build --target vnm_terminal_surface --config Release
```

Run the configured test suite:

```powershell
ctest --test-dir build -C Release --output-on-failure
```

On Linux and macOS, use the same configure step and the normal generated build
command for the selected generator.

Use the [repository guide](docs/repository_guide.md) for test families,
conformance controls, benchmarks, and focused validation commands.

## License

BSD 2-Clause. See [LICENSE](LICENSE).
