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
    storage/wal.cpp

    # Index layer
    index/tag_index.cpp

    # Utilities
    util/crc32.cpp
    util/logger.cpp
)

add_library(dam ${DAM_SOURCES})
add_library(dam::dam ALIAS dam)

target_include_directories(dam
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)
