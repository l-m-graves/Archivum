# archivum_punchline: the Punchline module. Stage 4 provides its schema as
# the first migration; endpoints and rules arrive with Stage 6. Engine
# only: no Drogon, so the schema tests run without the server.
add_library(archivum_punchline STATIC src/schema.cpp src/rules.cpp src/data.cpp src/localtime.cpp
  src/exceptions.cpp src/sync.cpp src/lifecycle.cpp)
target_include_directories(archivum_punchline PUBLIC include)
target_link_libraries(archivum_punchline PUBLIC archivum_core archivum_engine PRIVATE archivum_warnings)
