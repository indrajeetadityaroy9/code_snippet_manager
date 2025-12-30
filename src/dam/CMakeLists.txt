# DAM (Developer Asset Manager) library

set(DAM_SOURCES
    # Core DAM functionality
    language_detector.cpp
    snippet_index.cpp
    snippet_store.cpp

    # Storage layer
    storage/btree.cpp
    storage/buffer_pool.cpp
    storage/disk_manager.cpp
    storage/lru_replacer.cpp
    storage/page.cpp

    # Index layer
    index/tag_index.cpp

    # Search layer
    search/tokenizer.cpp
    search/inverted_index.cpp
    search/trigram_index.cpp
    search/embedder.cpp
    search/vector_index.cpp
    search/search_router.cpp

    # LLM layer
    llm/error_messages.cpp
    llm/model_discovery.cpp
    llm/ollama_provider.cpp
    llm/router.cpp

    # Utilities
    util/crc32.cpp
    util/logger.cpp
)

# Conditionally add llama.cpp provider
if(DAM_ENABLE_LLAMACPP)
    list(APPEND DAM_SOURCES llm/llamacpp_provider.cpp)
endif()

add_library(dam ${DAM_SOURCES})
add_library(dam::dam ALIAS dam)

target_include_directories(dam
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# Link dependencies for LLM and search features
if(DAM_ENABLE_LLM)
    target_link_libraries(dam
        PUBLIC
            nlohmann_json::nlohmann_json
        PRIVATE
            ${DAM_CURL_TARGET}
    )

    # Link llama.cpp if enabled
    if(DAM_ENABLE_LLAMACPP AND TARGET llama)
        target_link_libraries(dam PUBLIC llama)
        target_compile_definitions(dam PUBLIC DAM_HAS_LLAMACPP)
    endif()

    # Link hnswlib if enabled
    if(DAM_ENABLE_VECTOR_SEARCH AND TARGET hnswlib::hnswlib)
        target_link_libraries(dam PRIVATE hnswlib::hnswlib)
        target_compile_definitions(dam PUBLIC DAM_HAS_VECTOR_SEARCH)
    endif()
endif()
