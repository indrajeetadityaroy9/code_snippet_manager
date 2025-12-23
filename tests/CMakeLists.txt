# Test configuration

# DAM tests
add_executable(test_snippet_store dam/test_snippet_store.cpp)
target_link_libraries(test_snippet_store
    PRIVATE
        dam
        GTest::gtest_main
        GTest::gmock
)
gtest_discover_tests(test_snippet_store)

# Storage layer tests
add_executable(test_btree dam/test_btree.cpp)
target_link_libraries(test_btree
    PRIVATE
        dam
        GTest::gtest_main
        GTest::gmock
)
gtest_discover_tests(test_btree)

add_executable(test_buffer_pool dam/test_buffer_pool.cpp)
target_link_libraries(test_buffer_pool
    PRIVATE
        dam
        GTest::gtest_main
        GTest::gmock
)
gtest_discover_tests(test_buffer_pool)
