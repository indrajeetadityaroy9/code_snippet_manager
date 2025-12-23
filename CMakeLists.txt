cmake_minimum_required(VERSION 3.16)
project(dam VERSION 1.0.0 LANGUAGES CXX)

# C++17 required for std::filesystem and structured bindings
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for IDE support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build options
option(DAM_BUILD_TESTS "Build unit and integration tests" ON)
option(DAM_BUILD_CLI "Build command-line interface" ON)
option(DAM_ENABLE_LLM "Enable LLM-assisted features (requires libcurl)" ON)

# Include CMake modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(CompilerSettings)

# LLM dependencies (optional)
if(DAM_ENABLE_LLM)
    include(Dependencies)
endif()

# Main library
add_subdirectory(src/dam)

# Optional components
if(DAM_BUILD_TESTS)
    enable_testing()
    include(Testing)
    add_subdirectory(tests)
endif()

if(DAM_BUILD_CLI)
    add_subdirectory(tools/dam-cli)
endif()

# Installation
include(GNUInstallDirs)
install(DIRECTORY include/dam
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS dam
        EXPORT damTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
