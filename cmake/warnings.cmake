# Compiler warning policy shared by every target. Applied through an interface
# library so that vendored code (none yet) can opt out by not linking it.
add_library(archivum_warnings INTERFACE)

if(MSVC)
  target_compile_options(archivum_warnings INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus)
  target_compile_definitions(archivum_warnings INTERFACE
    _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
  if(ARCHIVUM_WARNINGS_AS_ERRORS)
    target_compile_options(archivum_warnings INTERFACE /WX)
  endif()
else()
  target_compile_options(archivum_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wunused
    -Woverloaded-virtual -Wdouble-promotion)
  if(ARCHIVUM_WARNINGS_AS_ERRORS)
    target_compile_options(archivum_warnings INTERFACE -Werror)
  endif()
  if(ARCHIVUM_SANITIZERS AND ARCHIVUM_TSAN)
    message(FATAL_ERROR "ARCHIVUM_SANITIZERS (ASan+UBSan) and ARCHIVUM_TSAN cannot share a build")
  endif()
  if(ARCHIVUM_SANITIZERS)
    target_compile_options(archivum_warnings INTERFACE
      -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    target_link_options(archivum_warnings INTERFACE -fsanitize=address,undefined)
  endif()
  if(ARCHIVUM_TSAN)
    # ThreadSanitizer: data races between threads. ASan and UBSan cannot see
    # them (docs/testing/fault-model.md, "what each tool sees"). Separate
    # build; CI runs the concurrency-labelled tests under it.
    target_compile_options(archivum_warnings INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(archivum_warnings INTERFACE -fsanitize=thread)
  endif()
endif()
