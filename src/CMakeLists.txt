# Everything except main() lives in a static library so the test binary can
# link the exact same objects as the server.
add_library(flashkv_core STATIC
    utils/logger.cpp
    protocol/protocol_handler.cpp
    storage/key_value_store.cpp
    storage/lru_cache.cpp
    command/command_router.cpp
    network/server.cpp
    network/connection.cpp
)

target_include_directories(flashkv_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include   # nlohmann/json.hpp (vendored, header-only)
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(flashkv_core PUBLIC Threads::Threads)

add_executable(flashkv main.cpp)
target_link_libraries(flashkv PRIVATE flashkv_core)

# Land the binary at build/flashkv, which is what the README and Dockerfile use.
set_target_properties(flashkv PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
