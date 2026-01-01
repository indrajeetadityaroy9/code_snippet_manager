#pragma once

#include <dam/snippet_store.hpp>
#include <CLI/CLI.hpp>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

namespace dam::cli {

/**
 * Context passed to command execution.
 * Contains shared resources like the snippet store.
 */
struct CommandContext {
    SnippetStore* store = nullptr;
    bool verbose = false;
    std::filesystem::path store_path;
};

/**
 * Base class for CLI commands.
 *
 * Each command implements:
 * - setup(): Configure CLI11 options and flags
 * - execute(): Perform the command action
 */
class Command {
public:
    virtual ~Command() = default;

    /**
     * Configure command options with CLI11.
     * Called during CLI initialization.
     *
     * @param app The CLI11 subcommand to configure
     */
    virtual void setup(CLI::App& app) = 0;

    /**
     * Execute the command.
     * Called after argument parsing succeeds.
     *
     * @param ctx Execution context with store and settings
     * @return Exit code (0 = success)
     */
    virtual int execute(CommandContext& ctx) = 0;

    /**
     * Get the command name (e.g., "add", "get").
     */
    virtual std::string name() const = 0;

    /**
     * Get a brief description for help text.
     */
    virtual std::string description() const = 0;
};

// Helper functions used by multiple commands

/**
 * Get the default store path (~/.dam).
 */
inline std::filesystem::path get_default_store_path() {
    const char* home = std::getenv("HOME");
    if (home) {
        return std::filesystem::path(home) / ".dam";
    }
    return ".dam";
}

/**
 * Open the snippet store.
 * Prints error to stderr on failure.
 *
 * @param path Store directory path
 * @param verbose Enable verbose mode
 * @return Opened store, or nullptr on error
 */
inline std::unique_ptr<SnippetStore> open_store(
    const std::filesystem::path& path,
    bool verbose = false
) {
    Config config;
    config.root_directory = path;
    config.verbose = verbose;

    auto result = SnippetStore::open(config);
    if (!result.ok()) {
        std::cerr << "Error: " << result.error().to_string() << "\n";
        return nullptr;
    }
    return std::move(result.value());
}

/**
 * Parse a snippet ID from string.
 * Returns nullopt if not a valid positive integer.
 */
inline std::optional<SnippetId> parse_snippet_id(const std::string& str) {
    if (str.empty()) return std::nullopt;

    char* end = nullptr;
    errno = 0;
    unsigned long long val = std::strtoull(str.c_str(), &end, 10);

    if (end == str.c_str() || *end != '\0') {
        return std::nullopt;
    }

    if (errno == ERANGE || val == 0) {
        return std::nullopt;
    }

    return static_cast<SnippetId>(val);
}

/**
 * Resolve snippet by ID or name.
 * Tries parsing as ID first, then looks up by name.
 *
 * @param store The snippet store
 * @param id_or_name ID number or snippet name
 * @return Snippet metadata, or error
 */
inline Result<SnippetMetadata> resolve_snippet(
    SnippetStore* store,
    const std::string& id_or_name
) {
    // Try as ID first
    auto parsed_id = parse_snippet_id(id_or_name);
    if (parsed_id.has_value()) {
        auto result = store->get(*parsed_id);
        if (result.ok()) {
            return result;
        }
    }

    // Try as name
    auto found_id = store->find_by_name(id_or_name);
    if (found_id.ok()) {
        return store->get(found_id.value());
    }

    return Error(ErrorCode::NOT_FOUND, "Snippet not found: " + id_or_name);
}

/**
 * Read entire file content.
 * @return File content if successful, std::nullopt on error
 */
inline std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    if (file.fail() && !file.eof()) {
        return std::nullopt;  // Read error occurred
    }
    return ss.str();
}

/**
 * Read from stdin until EOF.
 */
inline std::string read_stdin() {
    std::ostringstream ss;
    ss << std::cin.rdbuf();
    return ss.str();
}

/**
 * Truncate a string for display, adding "..." if needed.
 *
 * @param s The string to truncate
 * @param max_len Maximum length (including "..." if truncated)
 * @return Truncated string
 */
inline std::string truncate(const std::string& s, size_t max_len) {
    if (max_len <= 3) return s.substr(0, max_len);
    if (s.length() <= max_len) return s;
    return s.substr(0, max_len - 3) + "...";
}

}  // namespace dam::cli
