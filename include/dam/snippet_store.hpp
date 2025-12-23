#pragma once

#include <dam/types.hpp>
#include <dam/snippet_index.hpp>
#include <dam/language_detector.hpp>
#include <dam/result.hpp>
#include <dam/storage/buffer_pool.hpp>
#include <dam/storage/disk_manager.hpp>
#include <dam/index/tag_index.hpp>

#include <map>
#include <memory>
#include <set>
#include <vector>

namespace dam {

/**
 * SnippetStore - Main API for the Developer Asset Manager.
 *
 * Stores code snippets with inline content, tags, and language detection.
 * Uses B+ trees for efficient indexing and queries.
 */
class SnippetStore {
public:
    /**
     * Open or create a snippet store.
     *
     * @param config Store configuration
     * @return The opened store, or error
     */
    static Result<std::unique_ptr<SnippetStore>> open(const Config& config);

    /**
     * Close the store and flush pending changes.
     */
    void close();

    /**
     * Destructor - ensures close is called.
     */
    ~SnippetStore();

    // ========================================================================
    // CRUD Operations
    // ========================================================================

    /**
     * Add a new snippet.
     *
     * @param content The snippet content
     * @param name The snippet name
     * @param tags Optional tags
     * @param language Optional language (auto-detected if empty)
     * @param description Optional description
     * @return The assigned snippet ID
     */
    Result<SnippetId> add(const std::string& content,
                                    const std::string& name,
                                    const std::vector<std::string>& tags = {},
                                    const std::string& language = "",
                                    const std::string& description = "");

    /**
     * Get a snippet by ID.
     *
     * @param id The snippet ID
     * @return The snippet if found
     */
    std::optional<SnippetMetadata> get(SnippetId id) const;

    /**
     * Find a snippet by name.
     *
     * @param name The snippet name
     * @return The snippet ID if found
     */
    std::optional<SnippetId> find_by_name(const std::string& name) const;

    /**
     * Remove a snippet.
     *
     * @param id The snippet ID
     * @return Success or error
     */
    Result<void> remove(SnippetId id);

    // ========================================================================
    // Tag Operations
    // ========================================================================

    /**
     * Add a tag to a snippet.
     *
     * @param id The snippet ID
     * @param tag The tag to add
     * @return Success or error
     */
    Result<void> add_tag(SnippetId id, const std::string& tag);

    /**
     * Remove a tag from a snippet.
     *
     * @param id The snippet ID
     * @param tag The tag to remove
     * @return Success or error
     */
    Result<void> remove_tag(SnippetId id, const std::string& tag);

    // ========================================================================
    // Query Operations
    // ========================================================================

    /**
     * List all snippets.
     *
     * @return Vector of all snippets
     */
    std::vector<SnippetMetadata> list_all() const;

    /**
     * Find snippets by tag.
     *
     * @param tag The tag to search for
     * @return Matching snippets
     */
    std::vector<SnippetMetadata> find_by_tag(const std::string& tag) const;

    /**
     * Find snippets by language.
     *
     * @param language The language to search for
     * @return Matching snippets
     */
    std::vector<SnippetMetadata> find_by_language(const std::string& language) const;

    /**
     * Get all unique tags.
     *
     * @return Vector of tag names
     */
    std::vector<std::string> get_all_tags() const;

    /**
     * Get tag counts.
     *
     * @return Map of tag name to snippet count
     */
    std::map<std::string, size_t> get_tag_counts() const;

    // ========================================================================
    // Statistics
    // ========================================================================

    /**
     * Get the number of snippets.
     */
    size_t count() const;

    /**
     * Get the root directory.
     */
    const fs::path& get_root_directory() const { return root_dir_; }

    /**
     * Check if store is open.
     */
    bool is_open() const { return is_open_; }

private:
    SnippetStore() = default;

    std::unique_ptr<DiskManager> disk_manager_;
    std::unique_ptr<BufferPool> buffer_pool_;
    std::unique_ptr<SnippetIndex> snippet_index_;
    std::unique_ptr<TagIndex> tag_index_;
    fs::path root_dir_;
    bool is_open_ = false;
};

}  // namespace dam
