#pragma once

#include <cstdint>
#include <chrono>
#include <string>
#include <vector>

namespace dam {

// Page identifier - unique within a database file
using PageId = uint32_t;

// File identifier - unique within the document store
using FileId = uint64_t;

// Invalid/null values
constexpr PageId INVALID_PAGE_ID = 0;
constexpr FileId INVALID_FILE_ID = 0;

// Page configuration
constexpr size_t PAGE_SIZE = 4096;  // 4KB pages, aligned to disk blocks
constexpr size_t MAX_KEY_SIZE = 256;

// File metadata stored in the file index
struct FileMetadata {
    FileId id = INVALID_FILE_ID;
    std::string original_name;   // Original filename (without tags)
    std::string stored_path;     // Path within document store
    std::string extension;       // File extension (e.g., ".jpg")
    uint64_t size = 0;           // File size in bytes
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point modified_at;
    std::vector<std::string> tags;
    uint32_t checksum = 0;       // CRC32 of file content

    bool operator==(const FileMetadata& other) const {
        return id == other.id;
    }
};

// Node type in B+ tree
enum class NodeType : uint8_t {
    UNINITIALIZED = 0x00,  // Default after page reset
    INTERNAL = 0x01,
    LEAF = 0x02
};

}  // namespace dam
