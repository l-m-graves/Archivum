# C++20 subset

Language standard: C++20 (`CMAKE_CXX_STANDARD 20`, extensions off), set
explicitly in the top-level `CMakeLists.txt` and inherited by every preset.
Stage 0 was written on this standard; see `docs/plan-v1.md` section 0 for
the evidence.
Compilers: MSVC 2022 on Windows, GCC 12 on Linux. Clang is used locally for a
second opinion but is not a CI target.

## Use freely

- `std::span` for every pointer-plus-length. The VFS, journal, and (later)
  pager, WAL frames, record serialisation, and b-tree node access take and
  return spans. Debug builds bounds-check through the sanitizers.
- `<bit>`: `countl_zero`, `has_single_bit`, `bit_width`, `bit_cast`.
- Three-way comparison `<=>` for key comparison.
- Designated initializers and expanded `constexpr` (the CRC32C table is
  built at compile time this way).

## Use where it clarifies

- Concepts on the typed API boundaries (`Db`, `ReadTxn`, `Table`, `Cursor`)
  once those exist. Constrain interfaces; do not build a concept hierarchy.
- Drogon coroutines in route handlers, subject to the Stage 1 spike.

## Do not use

- Modules.
- Ranges, until the engine is solid.
- `std::format`, pending the Stage 1 check of static-link and container
  builds.
- Exceptions across the engine API. The engine returns `Status` and
  `Result<T>` (`engine/include/archivum/status.h`). Tests use a `REQUIRE`
  exception internally; that never crosses into engine code.

## Conventions in force since Stage 0

- No `<windows.h>` or `<unistd.h>` outside `engine/src/vfs_*.cpp`.
- `-Werror` / `/WX` with the warning set in `cmake/warnings.cmake`.
  `-Wnull-dereference` is deliberately excluded: GCC 12/13 report a false
  positive inside `std::vector` copies at `-O2`.
- Nested structs with default member initialisers are not used as default
  arguments of the enclosing class (GCC rejects it); see `JournalOptions`.

Raise any change to this list before implementing it.
