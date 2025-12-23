#pragma once

#include <dam/core_types.hpp>
#include <array>
#include <cstring>
#include <cstdint>

namespace dam {

/**
 * Page Header - Common header for all pages (32 bytes).
 *
 * Layout:
 *   [0-3]   page_id (4 bytes)
 *   [4]     node_type (1 byte)
 *   [5-6]   num_keys (2 bytes)
 *   [7-10]  parent_page_id (4 bytes)
 *   [11-14] page_lsn (4 bytes) - for WAL
 *   [15-18] checksum (4 bytes)
 *   [19-31] reserved (13 bytes)
 */
struct PageHeader {
    PageId page_id;          // 4 bytes
    PageId parent_page_id;   // 4 bytes
    uint32_t page_lsn;       // 4 bytes
    uint32_t checksum;       // 4 bytes
    uint16_t num_keys;       // 2 bytes
    NodeType node_type;      // 1 byte
    uint8_t reserved[13];    // 13 bytes = 32 total

    PageHeader()
        : page_id(INVALID_PAGE_ID)
        , parent_page_id(INVALID_PAGE_ID)
        , page_lsn(0)
        , checksum(0)
        , num_keys(0)
        , node_type(NodeType::UNINITIALIZED) {
        std::memset(reserved, 0, sizeof(reserved));
    }
};

static_assert(sizeof(PageHeader) == 32, "PageHeader must be 32 bytes");

/**
 * Page - A 4KB page that can be stored on disk.
 *
 * The page contains:
 * - Header (32 bytes)
 * - Data region (4064 bytes)
 *
 * For B+ tree nodes:
 * - Internal nodes: keys + child pointers
 * - Leaf nodes: key-value pairs + sibling pointers
 */
class Page {
public:
    static constexpr size_t HEADER_SIZE = sizeof(PageHeader);
    static constexpr size_t DATA_SIZE = PAGE_SIZE - HEADER_SIZE;

    Page() : header_(), data_() {
        std::memset(data_.data(), 0, DATA_SIZE);
    }

    // Header accessors
    PageId get_page_id() const { return header_.page_id; }
    void set_page_id(PageId id) { header_.page_id = id; }

    NodeType get_node_type() const { return header_.node_type; }
    void set_node_type(NodeType type) { header_.node_type = type; }

    uint16_t get_num_keys() const { return header_.num_keys; }
    void set_num_keys(uint16_t n) { header_.num_keys = n; }

    PageId get_parent_page_id() const { return header_.parent_page_id; }
    void set_parent_page_id(PageId id) { header_.parent_page_id = id; }

    uint32_t get_page_lsn() const { return header_.page_lsn; }
    void set_page_lsn(uint32_t lsn) { header_.page_lsn = lsn; }

    uint32_t get_checksum() const { return header_.checksum; }
    void set_checksum(uint32_t cs) { header_.checksum = cs; }

    // Data accessors
    uint8_t* get_data() { return data_.data(); }
    const uint8_t* get_data() const { return data_.data(); }

    // Raw page accessors (for disk I/O)
    char* get_raw_data() { return reinterpret_cast<char*>(this); }
    const char* get_raw_data() const { return reinterpret_cast<const char*>(this); }

    // Type checks
    bool is_leaf() const { return header_.node_type == NodeType::LEAF; }
    bool is_internal() const { return header_.node_type == NodeType::INTERNAL; }

    // Reset page to initial state
    void reset() {
        header_ = PageHeader();
        std::memset(data_.data(), 0, DATA_SIZE);
    }

    // Compute checksum of page data
    uint32_t compute_checksum() const;

    // Verify checksum
    bool verify_checksum() const {
        return header_.checksum == compute_checksum();
    }

    // Update checksum before writing
    void update_checksum() {
        header_.checksum = compute_checksum();
    }

private:
    PageHeader header_;
    std::array<uint8_t, DATA_SIZE> data_;
};

static_assert(sizeof(Page) == PAGE_SIZE, "Page must be exactly PAGE_SIZE bytes");

/**
 * Leaf node specific layout within Page::data_:
 *
 * [0-3]   prev_leaf_id (4 bytes)
 * [4-7]   next_leaf_id (4 bytes)
 * [8-9]   free_space_offset (2 bytes) - where free space starts
 * [10-11] data_offset (2 bytes) - where key-value data starts (grows from end)
 * [12+]   slot array: [offset:2, key_len:2, val_len:2] per entry
 * [...-end] key-value data (grows backward from end)
 */
class LeafPage {
public:
    static constexpr size_t LEAF_HEADER_SIZE = 12;
    static constexpr size_t SLOT_SIZE = 6;  // offset(2) + key_len(2) + val_len(2)

    explicit LeafPage(Page* page) : page_(page) {
        if (page_->get_node_type() != NodeType::LEAF) {
            page_->set_node_type(NodeType::LEAF);
            init();
        }
    }

    // Sibling pointers
    PageId get_prev_leaf() const;
    void set_prev_leaf(PageId id);
    PageId get_next_leaf() const;
    void set_next_leaf(PageId id);

    // Key-value operations
    bool insert(const std::string& key, const std::string& value);
    bool remove(const std::string& key);
    bool find(const std::string& key, std::string* value) const;
    bool update(const std::string& key, const std::string& new_value);

    // Check if there's space for a new key-value pair
    bool has_space(size_t key_len, size_t val_len) const;

    // Get all key-value pairs (for iteration)
    std::vector<std::pair<std::string, std::string>> get_all() const;

    // Get key at index
    std::string get_key_at(size_t index) const;

    // Get the minimum key in this leaf
    std::string get_min_key() const;

    // Split this leaf, moving half the entries to the new leaf
    // Returns the first key of the new leaf (to be inserted in parent)
    std::string split(LeafPage* new_leaf);

    // Check if leaf is at least half full
    bool is_half_full() const;

private:
    void init();
    uint8_t* data() { return page_->get_data(); }
    const uint8_t* data() const { return page_->get_data(); }

    // Slot array access
    struct Slot {
        uint16_t offset;
        uint16_t key_len;
        uint16_t val_len;
    };

    Slot get_slot(size_t index) const;
    void set_slot(size_t index, const Slot& slot);

    uint16_t get_free_space_offset() const;
    void set_free_space_offset(uint16_t offset);
    uint16_t get_data_offset() const;
    void set_data_offset(uint16_t offset);

    Page* page_;
};

/**
 * Internal node specific layout within Page::data_:
 *
 * [0-3]   first_child_id (4 bytes) - pointer before first key
 * [4-5]   free_space_offset (2 bytes)
 * [6-7]   data_offset (2 bytes)
 * [8+]    slot array: [child_id:4, offset:2, key_len:2] per entry
 * [...-end] key data (grows backward from end)
 */
class InternalPage {
public:
    static constexpr size_t INTERNAL_HEADER_SIZE = 8;
    static constexpr size_t SLOT_SIZE = 8;  // child_id(4) + offset(2) + key_len(2)

    explicit InternalPage(Page* page) : page_(page) {
        if (page_->get_node_type() != NodeType::INTERNAL) {
            page_->set_node_type(NodeType::INTERNAL);
            init();
        }
    }

    // Get the child page for a given key
    PageId find_child(const std::string& key) const;

    // Get child at specific index (0 = first_child, 1+ = after key[i-1])
    PageId get_child_at(size_t index) const;

    // Insert a key and right child pointer
    bool insert(const std::string& key, PageId right_child);

    // Remove a key (and its associated right child)
    bool remove(const std::string& key);

    // Get key at index
    std::string get_key_at(size_t index) const;

    // Check if there's space for a new key
    bool has_space(size_t key_len) const;

    // Set the first child (leftmost pointer)
    void set_first_child(PageId id);
    PageId get_first_child() const;

    // Split this internal node
    std::string split(InternalPage* new_internal);

private:
    void init();
    uint8_t* data() { return page_->get_data(); }
    const uint8_t* data() const { return page_->get_data(); }

    struct Slot {
        PageId child_id;
        uint16_t offset;
        uint16_t key_len;
    };

    Slot get_slot(size_t index) const;
    void set_slot(size_t index, const Slot& slot);

    uint16_t get_free_space_offset() const;
    void set_free_space_offset(uint16_t offset);
    uint16_t get_data_offset() const;
    void set_data_offset(uint16_t offset);

    Page* page_;
};

}  // namespace dam
