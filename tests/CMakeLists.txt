add_library(archivum_testsupport STATIC support/test_main.cpp)
target_include_directories(archivum_testsupport PUBLIC support)
target_link_libraries(archivum_testsupport PRIVATE archivum_warnings)

function(archivum_add_test name source)
  add_executable(${name} ${source})
  target_link_libraries(${name} PRIVATE archivum_engine archivum_punchline archivum_engine_testing archivum_testsupport archivum_warnings)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

archivum_add_test(crc32c_test unit/crc32c_test.cpp)
archivum_add_test(vfs_os_test unit/vfs_os_test.cpp)
archivum_add_test(mem_vfs_test unit/mem_vfs_test.cpp)
archivum_add_test(fault_vfs_test unit/fault_vfs_test.cpp)
archivum_add_test(journal_test unit/journal_test.cpp)
archivum_add_test(journal_crash_test crash/journal_crash_test.cpp)
archivum_add_test(db_test unit/db_test.cpp)
archivum_add_test(db_crash_test crash/db_crash_test.cpp)
archivum_add_test(btree_test unit/btree_test.cpp)
archivum_add_test(record_test unit/record_test.cpp)
archivum_add_test(store_test unit/store_test.cpp)
archivum_add_test(btree_model_test crash/btree_model_test.cpp)
archivum_add_test(store_model_test crash/store_model_test.cpp)
archivum_add_test(migrate_test unit/migrate_test.cpp)
archivum_add_test(backup_test unit/backup_test.cpp)
archivum_add_test(punchline_migration_test crash/punchline_migration_test.cpp)
set_tests_properties(punchline_migration_test PROPERTIES TIMEOUT 900)
set_tests_properties(btree_model_test PROPERTIES TIMEOUT 900)
set_tests_properties(store_model_test PROPERTIES TIMEOUT 900)
set_tests_properties(db_crash_test PROPERTIES TIMEOUT 900)

# The binary's operational subcommands on real files (Stage 4).
archivum_add_test(cli_test cli/cli_test.cpp)

# Tests that run more than one thread. The ThreadSanitizer job runs these
# and only these (TSan cannot share a build with ASan).
archivum_add_test(concurrency_test unit/concurrency_test.cpp)
archivum_add_test(core_test unit/core_test.cpp)
archivum_add_test(punchline_rules_test unit/punchline_rules_test.cpp)
archivum_add_test(punchline_sync_test unit/punchline_sync_test.cpp)
archivum_add_test(tzif_test unit/tzif_test.cpp)
# The time zone tooling against a synthetic release (tests/tzdata/README.md):
# the build-time check on zic output with zdump-pinned transitions, on
# every platform; regeneration and diff where zic and zdump exist.
add_executable(tzcheck_synthetic ${CMAKE_SOURCE_DIR}/tools/tzdata/tzcheck.cpp ${CMAKE_SOURCE_DIR}/modules/punchline/src/tzif.cpp
  tzdata/generated/embedded_tzdata.cpp)
target_include_directories(tzcheck_synthetic PRIVATE ${CMAKE_SOURCE_DIR}/modules/punchline/include)
target_link_libraries(tzcheck_synthetic PRIVATE archivum_engine archivum_warnings)
add_test(NAME tzcheck_synthetic_test COMMAND tzcheck_synthetic --pinned ${CMAKE_CURRENT_SOURCE_DIR}/tzdata/generated/pinned-transitions.txt)
if(NOT WIN32)
  add_test(NAME tzdata_generate_test COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tzdata/generate_test.sh ${CMAKE_SOURCE_DIR})
endif()
set_tests_properties(db_test store_test backup_test concurrency_test PROPERTIES LABELS concurrency)
set_tests_properties(cli_test PROPERTIES
  ENVIRONMENT "ARCHIVUM_BIN=$<TARGET_FILE:archivum>;ARCHIVUM_TMP=${CMAKE_CURRENT_BINARY_DIR}/cli-tmp"
  DEPENDS archivum)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/cli-tmp ${CMAKE_CURRENT_BINARY_DIR}/cli-tmp/archive)
add_dependencies(cli_test archivum)

# Server tests (Stage 1). Test-only issuer and PKI live here, never in the
# shipped binary.
add_library(archivum_server_testing STATIC server/test_pki.cpp server/test_issuer.cpp)
target_include_directories(archivum_server_testing PUBLIC server)
target_link_libraries(archivum_server_testing PUBLIC archivum_server archivum_core archivum_punchline PRIVATE archivum_warnings)

function(archivum_add_server_test name source)
  add_executable(${name} ${source})
  target_link_libraries(${name} PRIVATE archivum_server archivum_server_testing archivum_testsupport archivum_warnings)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 300 LABELS concurrency)
endfunction()

archivum_add_server_test(server_integration_test server/server_integration_test.cpp)
archivum_add_server_test(server_core_test server/server_core_test.cpp)
archivum_add_server_test(server_punchline_test server/server_punchline_test.cpp)
archivum_add_server_test(server_contract_test server/server_contract_test.cpp)
archivum_add_server_test(server_archive_test server/server_archive_test.cpp)
target_link_libraries(server_archive_test PRIVATE archivum_engine_testing)
set_tests_properties(server_contract_test PROPERTIES ENVIRONMENT "ARCHIVUM_SOURCE_DIR=${CMAKE_SOURCE_DIR}")
archivum_add_server_test(server_saturation_test server/server_saturation_test.cpp)
