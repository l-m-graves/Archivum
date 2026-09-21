# archivum_engine: the storage engine. Stage 2 adds the pager, the
# write-ahead log, recovery, and page-level transactions (engine/).
# Randomness comes from libsodium's randombytes_buf: thread-safe, no
# state of ours to manage, and it cannot fail once sodium_init() has
# succeeded (Stage 6 ruling: std::random_device can throw inside a
# checkpoint). The vcpkg port exports unofficial-sodium::sodium; a local
# build without vcpkg finds it through pkg-config.
find_package(unofficial-sodium CONFIG QUIET)
if(TARGET unofficial-sodium::sodium)
  set(ARCHIVUM_SODIUM unofficial-sodium::sodium)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
  set(ARCHIVUM_SODIUM PkgConfig::SODIUM)
endif()

add_library(archivum_engine STATIC
  src/crc32c.cpp
  src/status.cpp
  src/entropy.cpp
  src/journal.cpp
  src/engine/page_format.cpp
  src/engine/wal.cpp
  src/engine/db.cpp
  src/engine/btree.cpp
  src/engine/types.cpp
  src/engine/record.cpp
  src/engine/store.cpp
  src/engine/recovery.cpp
  src/engine/migrate.cpp
  $<IF:$<PLATFORM_ID:Windows>,src/vfs_win32.cpp,src/vfs_posix.cpp>
)
target_include_directories(archivum_engine PUBLIC include)
target_link_libraries(archivum_engine PRIVATE ${ARCHIVUM_SODIUM} archivum_warnings)

# archivum_engine_testing: test-only doubles for the VFS boundary. Never linked
# into archivum.exe. The crash-injection shim lives here.
add_library(archivum_engine_testing STATIC
  testing/src/mem_vfs.cpp
  testing/src/fault_vfs.cpp
)
target_include_directories(archivum_engine_testing PUBLIC testing/include)
target_link_libraries(archivum_engine_testing PUBLIC archivum_engine PRIVATE archivum_warnings)
