# cmake/LlamaCpp.cmake
# Native llama.cpp integration for local LLM inference

option(DAM_ENABLE_LLAMACPP "Enable native llama.cpp integration" ON)
option(DAM_LLAMACPP_METAL "Enable Metal GPU acceleration (macOS)" OFF)
option(DAM_LLAMACPP_CUDA "Enable CUDA GPU acceleration" OFF)
option(DAM_LLAMACPP_VULKAN "Enable Vulkan GPU acceleration" OFF)

if(DAM_ENABLE_LLAMACPP)
    message(STATUS "Building with native llama.cpp support")

    include(FetchContent)

    # Fetch llama.cpp from GitHub
    FetchContent_Declare(
        llama_cpp
        GIT_REPOSITORY https://github.com/ggerganov/llama.cpp.git
        GIT_TAG b4568
        GIT_SHALLOW TRUE
    )

    # Configure llama.cpp build options - disable unnecessary components
    set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

    # GPU backend selection
    if(DAM_LLAMACPP_METAL)
        set(GGML_METAL ON CACHE BOOL "" FORCE)
        message(STATUS "  GPU Backend: Metal (macOS)")
    elseif(DAM_LLAMACPP_CUDA)
        set(GGML_CUDA ON CACHE BOOL "" FORCE)
        message(STATUS "  GPU Backend: CUDA")
    elseif(DAM_LLAMACPP_VULKAN)
        set(GGML_VULKAN ON CACHE BOOL "" FORCE)
        message(STATUS "  GPU Backend: Vulkan")
    else()
        message(STATUS "  GPU Backend: None (CPU only)")
    endif()

    # Fetch and make available
    FetchContent_MakeAvailable(llama_cpp)

    # Create interface library for easy linking
    add_library(dam_llamacpp_deps INTERFACE)
    target_link_libraries(dam_llamacpp_deps INTERFACE llama ggml)
    target_include_directories(dam_llamacpp_deps INTERFACE
        ${llama_cpp_SOURCE_DIR}/include
        ${llama_cpp_SOURCE_DIR}/ggml/include
    )
    target_compile_definitions(dam_llamacpp_deps INTERFACE DAM_HAS_LLAMACPP=1)

    # Export for use in other CMake files
    set(DAM_HAS_LLAMACPP TRUE PARENT_SCOPE)
else()
    message(STATUS "llama.cpp support disabled")
    set(DAM_HAS_LLAMACPP FALSE PARENT_SCOPE)
endif()
