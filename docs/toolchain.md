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
| vcpkg | `vcpkg.json` builtin-baseline and `vcpkg-configuration.json`; CI checks out `microsoft/vcpkg` at the same commit | `1577f17ee57f42a0ef6d75bbb82cb37d0b76d7e8` |
| Drogon | vcpkg port at that baseline | 1.9.13 (port-version 2), trantor 1.5.28 |
| jwt-cpp | vcpkg port, no default features (nlohmann traits) | 0.7.2 |
| nlohmann-json | vcpkg port | 3.12.0 |
| OpenSSL | vcpkg port, static | 3.6.4 |
| libsodium | vcpkg port (Argon2id for credentials; rulings v3); an autotools build, so the Linux CI image installs autoconf, autoconf-archive, automake and libtool | 1.0.22 (port-version 1), as printed by the CI vcpkg install step of run 21 |
| Python (test toolchain only) | the contract runner against the FastAPI prototype, `tests/contract/test_contract.py`; installs the prototype's `requirements-dev.txt` (fastapi, sqlalchemy, pydantic, httpx, pytest) in the `contract-fastapi` CI job when a token for the Punchline repository is configured | the runner image's python3; the prototype's requirements pin minimums, not exact versions, and nothing from them ships |
| Triplets | `cmake/triplets/x64-linux-cxx20.cmake`, `x64-windows-static-cxx20.cmake`, `x64-linux-cxx20-tsan.cmake` | every port built as C++20; static CRT on Windows; the tsan triplet builds every port with `-fsanitize=thread` for the ThreadSanitizer job |
| Overlay port: drogon | `cmake/vcpkg-overlay-ports/drogon/`, the baseline port plus `0006-archivum-per-listener-ssl-conf-cmds.patch` | port-version 3; upstream v1.9.13, commit 4c5430757ea5451a7c38fbbef4b4bef7dbb47f2f, tarball SHA512 unchanged |
| Overlay port: trantor | `cmake/vcpkg-overlay-ports/trantor/`, a copy of the baseline port plus `002-archivum-ssl-conf-commands-win.patch` (TLS defaults applied before configured commands) | port-version 1; upstream v1.5.28, commit 63a4e5e164e219dc3bf30cdbfa1462ae5602fa97, tarball SHA512 unchanged from the baseline port |

## Observed in CI run 9 (2026-09-16, first all-green run with dependencies)

Same runner images as run 1. vcpkg built OpenSSL 3.6.4, Drogon 1.9.13
(overlay port-version 3), trantor 1.5.28 (overlay port-version 1),
jwt-cpp 0.7.2, nlohmann-json 3.12.0, jsoncpp, zlib, brotli, c-ares, and
libuuid from the pinned baseline; a cold Windows dependency build took 13
to 15 minutes, a warm one under 2. The binary cache is saved after
configure even when the build fails.

## Observed in CI run 1 (2026-09-15)

| Job | Versions printed |
|---|---|
| windows-2022 | Visual Studio 17.14.37614, MSVC 19.44.35228 (toolset 14.44), CMake 3.31.6, Windows SDK 10.0.26100 targeting 10.0.20348 |
| ubuntu:22.04 container | GCC 12 (distribution package), CMake 3.22, git 2.34.1 |

`actions/checkout@v4` targets Node 20, which the runners now force onto
Node 24 with a warning. Move to the v5 tag (or a pinned SHA) in the Stage 1
change.

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

## Local development in this session (not CI)

The session's egress proxy denies GitHub archive downloads
(codeload.github.com returns 403), which vcpkg needs for zlib, brotli,
jsoncpp, trantor, drogon, and jwt-cpp. Local builds therefore use
`CMakeUserPresets.json` (git-ignored) against Drogon 1.9.13 and jwt-cpp
0.7.2 built from git clones at their release tags (commits
4c5430757ea5451a7c38fbbef4b4bef7dbb47f2f and
b0ea29a58fc852a67d4e896d266880c2c63b0c4c) with the distribution's OpenSSL
3.0, jsoncpp, zlib, brotli, c-ares, and nlohmann-json. CI is the
authoritative build.

## Local development

Any GCC 12+ or Clang 16+ with libstdc++ 12+ builds the Linux presets. The
Windows preset needs Visual Studio 2022 with the C++ workload.
