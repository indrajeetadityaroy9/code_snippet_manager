#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dam {

namespace fs = std::filesystem;

// Snippet ID type
using SnippetId = uint64_t;
constexpr SnippetId INVALID_SNIPPET_ID = 0;

/**
 * Metadata for a code snippet stored in the DAM.
 */
struct SnippetMetadata {
    SnippetId id = INVALID_SNIPPET_ID;
    std::string name;           // Snippet name/title
    std::string content;        // Actual code content (stored inline)
    std::string language;       // Detected language (bash, python, cpp, etc.)
    std::string description;    // Optional description
    std::vector<std::string> tags;
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point modified_at;
    uint32_t checksum = 0;      // CRC32 of content
};

/**
 * Configuration for opening a SnippetStore.
 */
struct Config {
    fs::path root_directory;
    size_t buffer_pool_size = 512;
    bool verbose = false;
};

}  // namespace dam
