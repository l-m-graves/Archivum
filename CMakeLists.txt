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

include(cmake/warnings.cmake)

add_subdirectory(engine)

if(ARCHIVUM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
