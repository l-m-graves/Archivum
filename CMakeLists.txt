cmake_minimum_required(VERSION 3.22)

project(archivum
  VERSION 0.0.1
  DESCRIPTION "Archivum storage engine, application server, and low-code SQL interface"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ARCHIVUM_BUILD_TESTS "Build the test binaries" ON)
option(ARCHIVUM_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(ARCHIVUM_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer (GCC/Clang only)" OFF)
option(ARCHIVUM_TSAN "Enable ThreadSanitizer (GCC/Clang only; exclusive with ARCHIVUM_SANITIZERS)" OFF)
option(ARCHIVUM_PRODUCTION_BUILD "Production build: refuses test issuers and non-OS trust anchors" OFF)

# Record C++20 library feature availability for docs/cpp-subset.md. GCC 12's
# libstdc++ has no <format>; MSVC 2022 does. The result is printed at
# configure time and read from CI logs.
include(CheckCXXSourceCompiles)
check_cxx_source_compiles("#include <version>
#if !defined(__cpp_lib_format)
#error no std::format
#endif
int main() { return 0; }" ARCHIVUM_HAS_STD_FORMAT)
message(STATUS "std::format available: ${ARCHIVUM_HAS_STD_FORMAT}")

include(cmake/warnings.cmake)

add_subdirectory(engine)
add_subdirectory(core)
add_subdirectory(modules/punchline)
add_subdirectory(server)

if(ARCHIVUM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
