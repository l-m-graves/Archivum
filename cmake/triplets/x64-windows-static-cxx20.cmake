# x64-windows-static (static CRT, static libraries) with every C++ port
# built as C++20. See x64-linux-cxx20.cmake.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CXX_FLAGS "/std:c++20")
set(VCPKG_C_FLAGS "")
