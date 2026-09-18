# x64-linux-cxx20 with ThreadSanitizer in every port, so that the TSan CI
# job sees the synchronisation inside Drogon, trantor, OpenSSL and jsoncpp
# rather than reporting their atomics and reference counts as races. Only
# the linux-tsan preset uses it.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_CXX_FLAGS "-std=c++20 -fsanitize=thread -fno-omit-frame-pointer")
set(VCPKG_C_FLAGS "-fsanitize=thread -fno-omit-frame-pointer")
set(VCPKG_LINKER_FLAGS "-fsanitize=thread")
