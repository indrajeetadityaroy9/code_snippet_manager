#pragma once

#include <dam/core_types.hpp>
#include <dam/result.hpp>
#include <dam/storage/btree.hpp>

#include <set>
#include <string>
#include <vector>

namespace dam {

/**
 * TagIndex - Maps tags to sets of file IDs.
 *
 * Provides O(log n) lookup for files by tag, and supports
 * efficient set operations for multi-tag queries.
 */
class TagIndex {
public:
    /**
     * Create a tag index backed by a B+ tree.
     *
     * @param buffer_pool The buffer pool for page management
     * @param root_page_id Root page ID (INVALID_PAGE_ID to create new)
     */
    TagIndex(BufferPool* buffer_pool, PageId root_page_id = INVALID_PAGE_ID);

    /**
     * Add a file to a tag.
     *
     * @param tag The tag
     * @param file_id The file ID to add
     * @return true if added successfully
     */
    bool add_file_to_tag(const std::string& tag, FileId file_id);

    /**
     * Remove a file from a tag.
     *
     * @param tag The tag
     * @param file_id The file ID to remove
     * @return true if removed successfully
     */
    bool remove_file_from_tag(const std::string& tag, FileId file_id);

    /**
     * Remove a file from all tags.
     *
     * @param file_id The file ID to remove
     * @param tags List of tags to remove from
     */
    void remove_file_from_all_tags(FileId file_id, const std::vector<std::string>& tags);

    /**
     * Get all file IDs for a tag.
     *
     * @param tag The tag to query
     * @return Set of file IDs
     */
    std::set<FileId> get_files_for_tag(const std::string& tag) const;

    /**
     * Get files that have ALL the specified tags (AND query).
     *
     * @param tags The tags to match
     * @return Set of file IDs that have all tags
     */
    std::set<FileId> get_files_for_all_tags(const std::vector<std::string>& tags) const;

    /**
     * Get files that have ANY of the specified tags (OR query).
     *
     * @param tags The tags to match
     * @return Set of file IDs that have at least one tag
     */
    std::set<FileId> get_files_for_any_tag(const std::vector<std::string>& tags) const;

    /**
     * Get all tags in a range [start, end].
     *
     * @param start_tag Start of range (inclusive)
     * @param end_tag End of range (inclusive)
     * @return Vector of tags in the range
     */
    std::vector<std::string> get_tags_in_range(
        const std::string& start_tag,
        const std::string& end_tag) const;

    /**
     * Get all tags in the index.
     *
     * @return Vector of all tags
     */
    std::vector<std::string> get_all_tags() const;

    /**
     * Get the number of files for a tag.
     *
     * @param tag The tag to query
     * @return Number of files with this tag
     */
    size_t get_tag_count(const std::string& tag) const;

    /**
     * Check if a tag exists.
     */
    bool tag_exists(const std::string& tag) const;

    /**
     * Get root page ID for persistence.
     */
    PageId get_root_page_id() const { return tree_.get_root_page_id(); }

private:
    // Serialize a set of file IDs to a string
    static std::string serialize_file_ids(const std::set<FileId>& ids);

    // Deserialize a string to a set of file IDs
    static std::set<FileId> deserialize_file_ids(const std::string& data);

    BPlusTree tree_;
};

}  // namespace dam
