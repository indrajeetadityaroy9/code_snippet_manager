#pragma once

#include <dam/types.hpp>
#include <dam/storage/btree.hpp>
#include <dam/storage/buffer_pool.hpp>

#include <optional>
#include <vector>

namespace dam {

/**
 * SnippetIndex - Maps snippet IDs to metadata using B+ trees.
 *
 * Maintains two indexes:
 * - Primary: SnippetId -> SnippetMetadata (serialized)
 * - Secondary: name -> SnippetId
 */
class SnippetIndex {
public:
    /**
     * Create a snippet index backed by B+ trees.
     *
     * @param buffer_pool The buffer pool for page management
     * @param primary_root Primary index root page ID
     * @param name_root Name index root page ID
     */
    SnippetIndex(BufferPool* buffer_pool,
                 PageId primary_root = INVALID_PAGE_ID,
                 PageId name_root = INVALID_PAGE_ID);

    /**
     * Insert a new snippet.
     * If snippet.id is INVALID_SNIPPET_ID, assigns a new ID.
     *
     * @param snippet The snippet to insert
     * @return The assigned snippet ID
     */
    SnippetId insert(const SnippetMetadata& snippet);

    /**
     * Update an existing snippet.
     *
     * @param id The snippet ID
     * @param snippet The new metadata
     * @return true if updated successfully
     */
    bool update(SnippetId id, const SnippetMetadata& snippet);

    /**
     * Remove a snippet.
     *
     * @param id The snippet ID
     * @return true if removed successfully
     */
    bool remove(SnippetId id);

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
     * Get all snippets.
     *
     * @return Vector of all snippets
     */
    std::vector<SnippetMetadata> get_all() const;

    /**
     * Get the number of snippets.
     */
    size_t size() const { return count_; }

    /**
     * Get root page IDs for persistence.
     */
    PageId get_primary_root_id() const { return primary_tree_.get_root_page_id(); }
    PageId get_name_root_id() const { return name_tree_.get_root_page_id(); }

    /**
     * Get/set next ID for persistence.
     */
    SnippetId get_next_id() const { return next_id_; }
    void set_next_id(SnippetId id) { next_id_ = id; }

    /**
     * Set count for persistence (BPlusTree size isn't persisted).
     */
    void set_count(size_t count) { count_ = count; }

private:
    // Serialize SnippetMetadata to string
    static std::string serialize(const SnippetMetadata& snippet);

    // Deserialize string to SnippetMetadata
    static SnippetMetadata deserialize(const std::string& data);

    // Generate next snippet ID
    SnippetId generate_id();

    BPlusTree primary_tree_;  // id -> metadata
    BPlusTree name_tree_;     // name -> id
    SnippetId next_id_;
    size_t count_ = 0;
};

}  // namespace dam
