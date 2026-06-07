# vnm_terminal_surface

[![CI Linux](https://github.com/Varinomics/vnm_terminal_surface/actions/workflows/ci-linux.yml/badge.svg?branch=master)](https://github.com/Varinomics/vnm_terminal_surface/actions/workflows/ci-linux.yml) [![CI Windows](https://github.com/Varinomics/vnm_terminal_surface/actions/workflows/ci-windows.yml/badge.svg?branch=master)](https://github.com/Varinomics/vnm_terminal_surface/actions/workflows/ci-windows.yml) [![CI macOS](https://github.com/Varinomics/vnm_terminal_surface/actions/workflows/ci-macos.yml/badge.svg?branch=master)](https://github.com/Varinomics/vnm_terminal_surface/actions/workflows/ci-macos.yml)

`vnm_terminal_surface` is the home of the Qt Quick terminal surface used to embed
interactive PTY and ConPTY-backed command-line applications in Varinomics
products.

The primary visual component is `VNM_TerminalSurface`, a C++ `QQuickItem`.

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
