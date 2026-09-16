# archivum_server: HTTP, TLS, authentication. Stage 1 is the Drogon spike:
# one TLS listener, OIDC validation against a configurable issuer, /healthz.
find_package(Drogon CONFIG REQUIRED)
find_package(jwt-cpp CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

add_library(archivum_server STATIC
  src/config.cpp
  src/http/json_bridge.cpp
  src/auth/oidc.cpp
  src/app.cpp
)
target_include_directories(archivum_server PUBLIC include)
target_link_libraries(archivum_server
  PUBLIC archivum_engine Drogon::Drogon jwt-cpp::jwt-cpp nlohmann_json::nlohmann_json OpenSSL::SSL OpenSSL::Crypto
  PRIVATE archivum_warnings)
if(ARCHIVUM_PRODUCTION_BUILD)
  target_compile_definitions(archivum_server PUBLIC ARCHIVUM_PRODUCTION_BUILD=1)
else()
  target_compile_definitions(archivum_server PUBLIC ARCHIVUM_PRODUCTION_BUILD=0)
endif()

add_executable(archivum src/main.cpp)
target_link_libraries(archivum PRIVATE archivum_server archivum_warnings)
