# ============================================================================
# DAM Feature Configuration
# ============================================================================
# Consolidates all feature flags and provides a summary at configure time.

include(CMakeDependentOption)

# ============================================================================
# Core Build Options
# ============================================================================

option(DAM_BUILD_TESTS "Build unit and integration tests" ON)
option(DAM_BUILD_CLI "Build command-line interface" ON)

# ============================================================================
# LLM Integration (master switch)
# ============================================================================

option(DAM_ENABLE_LLM "Enable LLM-assisted features (Ollama, llama.cpp)" ON)

# ============================================================================
# LLM Sub-features (only active if DAM_ENABLE_LLM is ON)
# ============================================================================

cmake_dependent_option(DAM_ENABLE_LLAMACPP
    "Enable native llama.cpp integration for local inference" OFF
    "DAM_ENABLE_LLM" OFF)

cmake_dependent_option(DAM_ENABLE_VECTOR_SEARCH
    "Enable vector similarity search with hnswlib" ON
    "DAM_ENABLE_LLM" OFF)

# ============================================================================
# GPU Acceleration Options (only active if DAM_ENABLE_LLAMACPP is ON)
# ============================================================================

cmake_dependent_option(DAM_LLAMACPP_METAL
    "Enable Metal GPU acceleration (macOS only)" OFF
    "DAM_ENABLE_LLAMACPP" OFF)

cmake_dependent_option(DAM_LLAMACPP_CUDA
    "Enable CUDA GPU acceleration (NVIDIA)" OFF
    "DAM_ENABLE_LLAMACPP" OFF)

cmake_dependent_option(DAM_LLAMACPP_VULKAN
    "Enable Vulkan GPU acceleration" OFF
    "DAM_ENABLE_LLAMACPP" OFF)

# ============================================================================
# Validation
# ============================================================================

# Only one GPU backend can be active
set(_gpu_count 0)
if(DAM_LLAMACPP_METAL)
    math(EXPR _gpu_count "${_gpu_count} + 1")
endif()
if(DAM_LLAMACPP_CUDA)
    math(EXPR _gpu_count "${_gpu_count} + 1")
endif()
if(DAM_LLAMACPP_VULKAN)
    math(EXPR _gpu_count "${_gpu_count} + 1")
endif()
if(_gpu_count GREATER 1)
    message(FATAL_ERROR "Only one GPU backend can be enabled at a time. "
        "Please enable only one of: DAM_LLAMACPP_METAL, DAM_LLAMACPP_CUDA, DAM_LLAMACPP_VULKAN")
endif()
unset(_gpu_count)

# ============================================================================
# Feature Summary Function
# ============================================================================

function(dam_print_feature_summary)
    message(STATUS "")
    message(STATUS "========================================")
    message(STATUS "DAM Build Configuration")
    message(STATUS "========================================")
    message(STATUS "")
    message(STATUS "Build Type:        ${CMAKE_BUILD_TYPE}")
    message(STATUS "C++ Standard:      ${CMAKE_CXX_STANDARD}")
    message(STATUS "Compiler:          ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
    message(STATUS "")
    message(STATUS "Core Features:")

    if(DAM_BUILD_TESTS)
        message(STATUS "  Tests:           ON")
    else()
        message(STATUS "  Tests:           OFF")
    endif()

    if(DAM_BUILD_CLI)
        message(STATUS "  CLI:             ON")
    else()
        message(STATUS "  CLI:             OFF")
    endif()

    message(STATUS "")

    if(DAM_ENABLE_LLM)
        message(STATUS "LLM Features:      ENABLED")
        message(STATUS "  Ollama:          Always available")

        if(DAM_ENABLE_LLAMACPP)
            message(STATUS "  llama.cpp:       ON")
            if(DAM_LLAMACPP_METAL)
                message(STATUS "    GPU Backend:   Metal (macOS)")
            elseif(DAM_LLAMACPP_CUDA)
                message(STATUS "    GPU Backend:   CUDA (NVIDIA)")
            elseif(DAM_LLAMACPP_VULKAN)
                message(STATUS "    GPU Backend:   Vulkan")
            else()
                message(STATUS "    GPU Backend:   CPU only")
            endif()
        else()
            message(STATUS "  llama.cpp:       OFF")
        endif()

        if(DAM_ENABLE_VECTOR_SEARCH)
            message(STATUS "  Vector Search:   ON (hnswlib)")
        else()
            message(STATUS "  Vector Search:   OFF")
        endif()
    else()
        message(STATUS "LLM Features:      DISABLED")
    endif()

    message(STATUS "")
    message(STATUS "========================================")
    message(STATUS "")
endfunction()
