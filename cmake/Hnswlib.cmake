# ============================================================================
# hnswlib Integration for DAM
# ============================================================================
#
# hnswlib is a header-only library for fast approximate nearest neighbor search
# using Hierarchical Navigable Small World (HNSW) graphs.
#
# Usage:
#   include(cmake/Hnswlib.cmake)
#   target_link_libraries(your_target PRIVATE hnswlib::hnswlib)
#
# Options:
#   DAM_ENABLE_VECTOR_SEARCH - Enable/disable vector search (default: ON)
#   DAM_HNSWLIB_VERSION - Version to fetch (default: v0.8.0)
#
# ============================================================================

option(DAM_ENABLE_VECTOR_SEARCH "Enable vector similarity search with hnswlib" ON)
set(DAM_HNSWLIB_VERSION "v0.8.0" CACHE STRING "hnswlib version to use")

if(NOT DAM_ENABLE_VECTOR_SEARCH)
    message(STATUS "Vector search disabled")
    return()
endif()

include(FetchContent)

message(STATUS "Configuring hnswlib ${DAM_HNSWLIB_VERSION}...")

FetchContent_Declare(
    hnswlib
    GIT_REPOSITORY https://github.com/nmslib/hnswlib.git
    GIT_TAG        ${DAM_HNSWLIB_VERSION}
    GIT_SHALLOW    TRUE
)

# Fetch and make available
FetchContent_MakeAvailable(hnswlib)

# hnswlib is header-only, create an interface library
if(NOT TARGET hnswlib::hnswlib)
    add_library(hnswlib_interface INTERFACE)
    target_include_directories(hnswlib_interface INTERFACE
        ${hnswlib_SOURCE_DIR}
    )

    # Enable OpenMP for parallel index building if available
    find_package(OpenMP QUIET)
    if(OpenMP_CXX_FOUND)
        target_link_libraries(hnswlib_interface INTERFACE OpenMP::OpenMP_CXX)
        target_compile_definitions(hnswlib_interface INTERFACE HNSWLIB_HAVE_OPENMP)
        message(STATUS "hnswlib: OpenMP enabled for parallel index building")
    endif()

    # Add alias for consistent naming
    add_library(hnswlib::hnswlib ALIAS hnswlib_interface)
endif()

# Note: DAM_HAS_VECTOR_SEARCH is defined via target_compile_definitions
# in src/dam/CMakeLists.txt to avoid global pollution

message(STATUS "hnswlib configured successfully")
message(STATUS "  Source: ${hnswlib_SOURCE_DIR}")
