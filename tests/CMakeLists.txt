add_library(archivum_testsupport STATIC support/test_main.cpp)
target_include_directories(archivum_testsupport PUBLIC support)
target_link_libraries(archivum_testsupport PRIVATE archivum_warnings)

function(archivum_add_test name source)
  add_executable(${name} ${source})
  target_link_libraries(${name} PRIVATE archivum_engine archivum_engine_testing archivum_testsupport archivum_warnings)
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
archivum_add_test(btree_model_test crash/btree_model_test.cpp)
set_tests_properties(btree_model_test PROPERTIES TIMEOUT 900)
set_tests_properties(db_crash_test PROPERTIES TIMEOUT 900)

# Server tests (Stage 1). Test-only issuer and PKI live here, never in the
# shipped binary.
add_library(archivum_server_testing STATIC server/test_pki.cpp server/test_issuer.cpp)
target_include_directories(archivum_server_testing PUBLIC server)
target_link_libraries(archivum_server_testing PUBLIC archivum_server PRIVATE archivum_warnings)

function(archivum_add_server_test name source)
  add_executable(${name} ${source})
  target_link_libraries(${name} PRIVATE archivum_server archivum_server_testing archivum_testsupport archivum_warnings)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 300)
endfunction()

archivum_add_server_test(server_integration_test server/server_integration_test.cpp)
archivum_add_server_test(server_saturation_test server/server_saturation_test.cpp)
