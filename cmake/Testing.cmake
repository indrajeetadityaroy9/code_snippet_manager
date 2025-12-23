# Testing configuration with GoogleTest

include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)

# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# Helper function for adding test executables
function(dam_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name}
        PRIVATE
            dam
            GTest::gtest_main
            GTest::gmock
    )
    gtest_discover_tests(${name})
endfunction()
