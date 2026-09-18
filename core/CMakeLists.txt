# archivum_core: what every module shares and what the server builds on:
# the core schema (audit log, change feed, roles, dataset grants, local
# accounts), the recorder that writes audit rows and change-feed images
# inside the business transaction, credentials (Argon2id via libsodium),
# local accounts, authorisation data, and the module interface. Engine and
# nlohmann/json only: no Drogon, so its tests run without the server.
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-sodium CONFIG QUIET)
if(TARGET unofficial-sodium::sodium)
  set(ARCHIVUM_SODIUM unofficial-sodium::sodium)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
  set(ARCHIVUM_SODIUM PkgConfig::SODIUM)
endif()

add_library(archivum_core STATIC
  src/schema.cpp
  src/recorder.cpp
  src/ids.cpp
  src/credentials.cpp
  src/accounts.cpp
  src/authz.cpp
  src/module.cpp
)
target_include_directories(archivum_core PUBLIC include)
target_link_libraries(archivum_core
  PUBLIC archivum_engine nlohmann_json::nlohmann_json
  PRIVATE ${ARCHIVUM_SODIUM} archivum_warnings)
