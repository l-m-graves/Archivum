# archivum_server: HTTP, TLS, authentication. Stage 1 is the Drogon spike:
# one TLS listener, OIDC validation against a configurable issuer, /healthz.
find_package(Drogon CONFIG REQUIRED)
# Drogon's own FindJsoncpp module resolves a single library path, which under
# a multi-config generator (MSVC) is the debug jsoncpp for every
# configuration and breaks Release links. Re-point its imported target at
# jsoncpp's per-configuration CMake target when one is available.
find_package(jsoncpp CONFIG QUIET)
if(TARGET Jsoncpp_lib AND TARGET JsonCpp::JsonCpp)
  set_target_properties(Jsoncpp_lib PROPERTIES INTERFACE_LINK_LIBRARIES JsonCpp::JsonCpp)
  message(STATUS "Jsoncpp_lib re-pointed at JsonCpp::JsonCpp (per-configuration)")
endif()
find_package(jwt-cpp CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

add_library(archivum_server STATIC
  src/config.cpp
  src/log.cpp
  src/archive.cpp
  src/http/json_bridge.cpp
  src/http/request.cpp
  src/auth/oidc.cpp
  src/auth/identity.cpp
  src/modules/punchline_module.cpp
  src/modules/punchline_routes.cpp
  src/app.cpp
)
target_include_directories(archivum_server PUBLIC include)
target_link_libraries(archivum_server
  PUBLIC archivum_engine archivum_core archivum_punchline Drogon::Drogon jwt-cpp::jwt-cpp nlohmann_json::nlohmann_json OpenSSL::SSL OpenSSL::Crypto
  PRIVATE archivum_warnings)
if(ARCHIVUM_PRODUCTION_BUILD)
  target_compile_definitions(archivum_server PUBLIC ARCHIVUM_PRODUCTION_BUILD=1)
else()
  target_compile_definitions(archivum_server PUBLIC ARCHIVUM_PRODUCTION_BUILD=0)
endif()

add_executable(archivum src/main.cpp)
target_link_libraries(archivum PRIVATE archivum_server archivum_core archivum_punchline archivum_warnings)
