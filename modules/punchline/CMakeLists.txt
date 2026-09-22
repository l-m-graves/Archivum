# archivum_punchline: the Punchline module: schema, rules, data, local
# time and the embedded IANA time zone database, sync and pairing, the
# lifecycle, exceptions, the rollback export. Engine only: no Drogon, so
# the module tests run without the server.
add_custom_target(archivum_punchline_checked ALL)
add_library(archivum_punchline STATIC src/schema.cpp src/rules.cpp src/data.cpp src/localtime.cpp
  src/exceptions.cpp src/sync.cpp src/lifecycle.cpp src/rollback.cpp src/tzif.cpp
  tzdata/embedded_tzdata.cpp)
target_include_directories(archivum_punchline PUBLIC include)
target_link_libraries(archivum_punchline PUBLIC archivum_core archivum_engine PRIVATE archivum_warnings)
# The generated database is large and never warned about; the placeholder
# and the generated file are both compiled without the warning set.
set_source_files_properties(tzdata/embedded_tzdata.cpp PROPERTIES COMPILE_OPTIONS "")

# Build-time assertion (Stage 6 rulings, item 1): the embedded database
# must reproduce every transition zdump pinned at generation time, on every
# platform, or the build fails. ARCHIVUM_TZDATA_ALLOW_EMPTY lets the
# placeholder build until the IANA release is vendored; it is OFF in CI
# once the database is in.
option(ARCHIVUM_TZDATA_ALLOW_EMPTY "Allow building with no embedded time zone database" ON)
add_executable(tzcheck ${CMAKE_SOURCE_DIR}/tools/tzdata/tzcheck.cpp)
target_link_libraries(tzcheck PRIVATE archivum_punchline archivum_warnings)
set(ARCHIVUM_TZCHECK_ARGS --pinned ${CMAKE_CURRENT_SOURCE_DIR}/tzdata/pinned-transitions.txt)
if(ARCHIVUM_TZDATA_ALLOW_EMPTY)
  list(APPEND ARCHIVUM_TZCHECK_ARGS --allow-empty)
endif()
add_custom_command(TARGET tzcheck POST_BUILD COMMAND tzcheck ${ARCHIVUM_TZCHECK_ARGS}
  COMMENT "tzcheck: the embedded time zone database against the pinned zdump transitions")
add_dependencies(archivum_punchline_checked tzcheck)
