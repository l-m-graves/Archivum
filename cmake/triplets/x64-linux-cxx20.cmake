# x64-linux with every C++ port built as C++20, so that Drogon's
# coroutine support is compiled into the library the same way our code
# uses it. Otherwise identical to vcpkg's builtin x64-linux triplet.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_CXX_FLAGS "-std=c++20")
set(VCPKG_C_FLAGS "")
