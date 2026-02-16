find_package(GTest REQUIRED)

add_executable(flashkv_tests
    test_json_protocol.cpp
    test_storage_engine.cpp
    test_ttl.cpp
    test_lru.cpp
    test_concurrent.cpp
    test_integration.cpp
)

target_link_libraries(flashkv_tests PRIVATE
    flashkv_core
    GTest::gtest
    GTest::gtest_main
    Threads::Threads
)

set_target_properties(flashkv_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME flashkv_tests COMMAND flashkv_tests)
