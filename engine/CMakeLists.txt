# archivum_engine: the storage engine. Stage 0 contains only the VFS boundary,
# CRC32C, and the append-only journal. No pager, WAL, or b-tree code yet.
add_library(archivum_engine STATIC
  src/crc32c.cpp
  src/status.cpp
  src/journal.cpp
  $<IF:$<PLATFORM_ID:Windows>,src/vfs_win32.cpp,src/vfs_posix.cpp>
)
target_include_directories(archivum_engine PUBLIC include)
target_link_libraries(archivum_engine PRIVATE archivum_warnings)

# archivum_engine_testing: test-only doubles for the VFS boundary. Never linked
# into archivum.exe. The crash-injection shim lives here.
add_library(archivum_engine_testing STATIC
  testing/src/mem_vfs.cpp
  testing/src/fault_vfs.cpp
)
target_include_directories(archivum_engine_testing PUBLIC testing/include)
target_link_libraries(archivum_engine_testing PUBLIC archivum_engine PRIVATE archivum_warnings)
