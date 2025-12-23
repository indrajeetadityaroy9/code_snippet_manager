#pragma once

#include <dam/core_types.hpp>
#include <dam/result.hpp>
#include <dam/storage/page.hpp>
#include <dam/storage/buffer_pool.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dam {

/**
 * BPlusTree - A disk-based B+ tree implementation.
 *
 * Provides O(log n) insert, delete, and search operations.
 * Supports range queries via leaf node sibling pointers.
 *
 * Key features:
 * - String keys and values
 * - Automatic page splitting and merging
 * - Buffer pool integration for efficient I/O
 * - Iterator support for range scans
 */
class BPlusTree {
public:
    /**
     * Create a B+ tree backed by the given buffer pool.
     *
     * @param buffer_pool The buffer pool for page management
     * @param root_page_id The root page ID (INVALID_PAGE_ID to create new tree)
     */
    BPlusTree(BufferPool* buffer_pool, PageId root_page_id = INVALID_PAGE_ID);

    ~BPlusTree() = default;

    // Prevent copying
    BPlusTree(const BPlusTree&) = delete;
    BPlusTree& operator=(const BPlusTree&) = delete;

    /**
     * Insert a key-value pair into the tree.
     *
     * @param key The key to insert
     * @param value The value associated with the key
     * @return true if insertion succeeded, false if key already exists
     */
    bool insert(const std::string& key, const std::string& value);

    /**
     * Remove a key from the tree.
     *
     * @param key The key to remove
     * @return true if the key was found and removed
     */
    bool remove(const std::string& key);

    /**
     * Search for a key in the tree.
     *
     * @param key The key to search for
     * @return The value if found, std::nullopt otherwise
     */
    std::optional<std::string> find(const std::string& key) const;

    /**
     * Update the value for an existing key.
     *
     * @param key The key to update
     * @param value The new value
     * @return true if the key was found and updated
     */
    bool update(const std::string& key, const std::string& value);

    /**
     * Check if a key exists in the tree.
     */
    bool contains(const std::string& key) const;

    /**
     * Get all key-value pairs in a range [start_key, end_key].
     *
     * @param start_key The start of the range (inclusive)
     * @param end_key The end of the range (inclusive)
     * @return Vector of key-value pairs in the range
     */
    std::vector<std::pair<std::string, std::string>> range(
        const std::string& start_key,
        const std::string& end_key) const;

    /**
     * Get all key-value pairs starting from a key (inclusive) up to a limit.
     *
     * @param start_key The start key (inclusive)
     * @param limit Maximum number of pairs to return
     * @return Vector of key-value pairs
     */
    std::vector<std::pair<std::string, std::string>> scan(
        const std::string& start_key,
        size_t limit) const;

    /**
     * Get all key-value pairs in the tree.
     *
     * @return Vector of all key-value pairs
     */
    std::vector<std::pair<std::string, std::string>> get_all() const;

    /**
     * Iterate over all key-value pairs with a callback.
     *
     * @param callback Function called for each pair; return false to stop
     */
    void for_each(const std::function<bool(const std::string&, const std::string&)>& callback) const;

    /**
     * Get the number of keys in the tree.
     */
    size_t size() const { return size_; }

    /**
     * Check if the tree is empty.
     */
    bool empty() const { return size_ == 0; }

    /**
     * Get the root page ID.
     */
    PageId get_root_page_id() const { return root_page_id_; }

    /**
     * Get the height of the tree.
     */
    size_t height() const;

    /**
     * Verify B+ tree invariants (for testing/debugging).
     *
     * @return true if all invariants hold
     */
    bool verify() const;

private:
    // Find the leaf page that should contain the given key
    PageId find_leaf(const std::string& key) const;

    // Insert into a leaf, handling splits if necessary
    bool insert_into_leaf(PageId leaf_id, const std::string& key, const std::string& value);

    // Insert into a parent after a child split
    void insert_into_parent(PageId left_id, const std::string& key, PageId right_id);

    // Split a leaf node
    PageId split_leaf(PageId leaf_id, const std::string& key, const std::string& value);

    // Split an internal node
    PageId split_internal(PageId internal_id, const std::string& key, PageId right_child);

    // Get a new tree with a single leaf as root
    void init_empty_tree();

    // Helper to get the leftmost leaf
    PageId get_leftmost_leaf() const;

    BufferPool* buffer_pool_;
    PageId root_page_id_;
    size_t size_;
};

/**
 * BPlusTreeIterator - Iterator for range scans over a B+ tree.
 */
class BPlusTreeIterator {
public:
    BPlusTreeIterator(BufferPool* buffer_pool, PageId leaf_id, size_t index);

    bool valid() const { return leaf_id_ != INVALID_PAGE_ID; }

    std::pair<std::string, std::string> operator*() const;

    BPlusTreeIterator& operator++();

    bool operator==(const BPlusTreeIterator& other) const;
    bool operator!=(const BPlusTreeIterator& other) const { return !(*this == other); }

private:
    BufferPool* buffer_pool_;
    PageId leaf_id_;
    size_t index_;
};

}  // namespace dam
