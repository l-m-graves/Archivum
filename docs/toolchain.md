# Toolchain and pins

Everything a build depends on, and whether it is pinned to an exact version.

| Component | Where | Pin |
|---|---|---|
| CMake | `cmake_minimum_required(VERSION 3.22)` | minimum; CI uses Ubuntu 22.04's 3.22.x and the windows-2022 image's current CMake |
| C++ standard | `CMakeLists.txt` | C++20, extensions off |
| GCC | CI Linux job, `CXX=g++-12` in the `ubuntu:22.04` image | major version 12 from the distribution |
| MSVC | CI Windows job, `windows-2022` image, generator `Visual Studio 17 2022` | latest 17.x on the image; version printed in the "Toolchain versions" step of every run |
| Ninja | Linux only | distribution package |
| Linux image | `container: ubuntu:22.04` | tag, not digest (see below) |
| GitHub Actions | `actions/checkout@v4` | major tag, not commit SHA (see below) |
| Third-party libraries | none at Stage 0 | vcpkg manifest with a pinned baseline arrives in Stage 1 |

## Not yet pinned, and why

- **`actions/checkout` by SHA and `ubuntu:22.04` by digest.** This session
  cannot read repositories outside `l-m-graves/archivum`, so I could not
  look up the current commit SHA of `actions/checkout` or the image digest.
  Both should be pinned in the Stage 1 change once those values are
  obtained; the file to edit is `.github/workflows/ci.yml`, the `uses:` and
  `container:` lines.
- **MSVC minor version.** The `windows-2022` image updates Visual Studio in
  place. Every CI run prints the installed version so a regression can be
  correlated. A pin to a specific toolset would require installing the
  toolset in CI, which is Stage 1 work alongside vcpkg.

## Local development

Any GCC 12+ or Clang 16+ with libstdc++ 12+ builds the Linux presets. The
Windows preset needs Visual Studio 2022 with the C++ workload.
