#include <dam/storage/page.hpp>
#include <algorithm>

namespace dam {

// Simple CRC32 implementation for checksum
static uint32_t crc32_table[256];
static bool crc32_table_initialized = false;

static void init_crc32_table() {
    if (crc32_table_initialized) return;

    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (int j = 0; j < 8; ++j) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
        }
        crc32_table[i] = crc;
    }
    crc32_table_initialized = true;
}

static uint32_t compute_crc32(const uint8_t* data, size_t len) {
    init_crc32_table();
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

uint32_t Page::compute_checksum() const {
    // Compute checksum over data only (not header)
    return compute_crc32(data_.data(), DATA_SIZE);
}

// ============================================================================
// LeafPage Implementation
// ============================================================================

void LeafPage::init() {
    set_prev_leaf(INVALID_PAGE_ID);
    set_next_leaf(INVALID_PAGE_ID);
    set_free_space_offset(LEAF_HEADER_SIZE);
    set_data_offset(static_cast<uint16_t>(Page::DATA_SIZE));
    page_->set_num_keys(0);
}

PageId LeafPage::get_prev_leaf() const {
    const uint8_t* d = data();
    return *reinterpret_cast<const PageId*>(d);
}

void LeafPage::set_prev_leaf(PageId id) {
    uint8_t* d = data();
    *reinterpret_cast<PageId*>(d) = id;
}

PageId LeafPage::get_next_leaf() const {
    const uint8_t* d = data();
    return *reinterpret_cast<const PageId*>(d + 4);
}

void LeafPage::set_next_leaf(PageId id) {
    uint8_t* d = data();
    *reinterpret_cast<PageId*>(d + 4) = id;
}

uint16_t LeafPage::get_free_space_offset() const {
    const uint8_t* d = data();
    return *reinterpret_cast<const uint16_t*>(d + 8);
}

void LeafPage::set_free_space_offset(uint16_t offset) {
    uint8_t* d = data();
    *reinterpret_cast<uint16_t*>(d + 8) = offset;
}

uint16_t LeafPage::get_data_offset() const {
    const uint8_t* d = data();
    return *reinterpret_cast<const uint16_t*>(d + 10);
}

void LeafPage::set_data_offset(uint16_t offset) {
    uint8_t* d = data();
    *reinterpret_cast<uint16_t*>(d + 10) = offset;
}

LeafPage::Slot LeafPage::get_slot(size_t index) const {
    const uint8_t* d = data() + LEAF_HEADER_SIZE + index * SLOT_SIZE;
    Slot slot;
    slot.offset = *reinterpret_cast<const uint16_t*>(d);
    slot.key_len = *reinterpret_cast<const uint16_t*>(d + 2);
    slot.val_len = *reinterpret_cast<const uint16_t*>(d + 4);
    return slot;
}

void LeafPage::set_slot(size_t index, const Slot& slot) {
    uint8_t* d = data() + LEAF_HEADER_SIZE + index * SLOT_SIZE;
    *reinterpret_cast<uint16_t*>(d) = slot.offset;
    *reinterpret_cast<uint16_t*>(d + 2) = slot.key_len;
    *reinterpret_cast<uint16_t*>(d + 4) = slot.val_len;
}

bool LeafPage::has_space(size_t key_len, size_t val_len) const {
    uint16_t free_start = get_free_space_offset();
    uint16_t data_start = get_data_offset();

    // Prevent underflow on corrupted pages
    if (data_start < free_start) {
        return false;
    }

    size_t needed = SLOT_SIZE + key_len + val_len;
    return (data_start - free_start) >= needed;
}

bool LeafPage::insert(const std::string& key, const std::string& value) {
    if (!has_space(key.size(), value.size())) {
        return false;
    }

    uint16_t num_keys = page_->get_num_keys();

    // Find insertion position (maintain sorted order)
    size_t pos = 0;
    while (pos < num_keys && get_key_at(pos) < key) {
        ++pos;
    }

    // Check for duplicate
    if (pos < num_keys && get_key_at(pos) == key) {
        return false;  // Duplicate key
    }

    // Shift slots to make room
    for (size_t i = num_keys; i > pos; --i) {
        set_slot(i, get_slot(i - 1));
    }

    // Allocate space for key-value data at the end
    uint16_t data_offset = get_data_offset();
    size_t total_len = key.size() + value.size();

    // Defensive check: prevent underflow on corrupted pages
    if (data_offset < total_len) {
        return false;
    }
    data_offset -= static_cast<uint16_t>(total_len);

    // Write key and value
    uint8_t* d = data();
    std::memcpy(d + data_offset, key.data(), key.size());
    std::memcpy(d + data_offset + key.size(), value.data(), value.size());

    // Write slot
    Slot slot;
    slot.offset = data_offset;
    slot.key_len = static_cast<uint16_t>(key.size());
    slot.val_len = static_cast<uint16_t>(value.size());
    set_slot(pos, slot);

    // Update metadata
    set_data_offset(data_offset);
    set_free_space_offset(static_cast<uint16_t>(LEAF_HEADER_SIZE + (num_keys + 1) * SLOT_SIZE));
    page_->set_num_keys(num_keys + 1);

    return true;
}

bool LeafPage::remove(const std::string& key) {
    uint16_t num_keys = page_->get_num_keys();

    // Find the key
    size_t pos = 0;
    while (pos < num_keys && get_key_at(pos) < key) {
        ++pos;
    }

    if (pos >= num_keys || get_key_at(pos) != key) {
        return false;  // Key not found
    }

    // Shift slots to fill the gap
    for (size_t i = pos; i < num_keys - 1; ++i) {
        set_slot(i, get_slot(i + 1));
    }

    // Update metadata (we don't compact data region for simplicity)
    set_free_space_offset(static_cast<uint16_t>(LEAF_HEADER_SIZE + (num_keys - 1) * SLOT_SIZE));
    page_->set_num_keys(num_keys - 1);

    return true;
}

bool LeafPage::find(const std::string& key, std::string* value) const {
    uint16_t num_keys = page_->get_num_keys();

    // Binary search
    size_t left = 0, right = num_keys;
    while (left < right) {
        size_t mid = left + (right - left) / 2;
        std::string mid_key = get_key_at(mid);
        if (mid_key < key) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }

    if (left >= num_keys || get_key_at(left) != key) {
        return false;
    }

    if (value) {
        Slot slot = get_slot(left);

        // Validate slot data stays within page bounds
        size_t end_offset = static_cast<size_t>(slot.offset) + slot.key_len + slot.val_len;
        if (slot.offset < LEAF_HEADER_SIZE || end_offset > Page::DATA_SIZE) {
            return false;  // Corrupted slot - out of bounds
        }

        const uint8_t* d = data();
        *value = std::string(
            reinterpret_cast<const char*>(d + slot.offset + slot.key_len),
            slot.val_len);
    }

    return true;
}

bool LeafPage::update(const std::string& key, const std::string& new_value) {
    // Find the existing value
    std::string old_value;
    if (!find(key, &old_value)) {
        return false;
    }

    if (old_value.size() == new_value.size()) {
        // Can update in place
        uint16_t num_keys = page_->get_num_keys();
        for (size_t i = 0; i < num_keys; ++i) {
            if (get_key_at(i) == key) {
                Slot slot = get_slot(i);
                uint8_t* d = data();
                std::memcpy(d + slot.offset + slot.key_len,
                           new_value.data(), new_value.size());
                return true;
            }
        }
    }

    // For value size changes, check if we have space BEFORE removing.
    // Calculate space: after removing the old entry, we free SLOT_SIZE from slot array.
    // The data region doesn't get compacted, but we need contiguous space for new data.
    // Since we can't easily determine if there's enough contiguous space at data end
    // after accounting for fragmentation, just check if new value fits assuming no compaction.
    // If the new value is larger than old value and doesn't fit, return false to trigger
    // tree-level handling (split).
    if (new_value.size() > old_value.size()) {
        // Check if there's space for the size difference
        uint16_t free_start = get_free_space_offset();
        uint16_t data_start = get_data_offset();
        size_t available = data_start - free_start;
        size_t extra_needed = new_value.size() - old_value.size();
        if (available < extra_needed) {
            // Not enough space - return false to trigger tree-level split
            return false;
        }
    }

    // Remove and re-insert
    remove(key);
    return insert(key, new_value);
}

std::vector<std::pair<std::string, std::string>> LeafPage::get_all() const {
    std::vector<std::pair<std::string, std::string>> result;
    uint16_t num_keys = page_->get_num_keys();

    for (size_t i = 0; i < num_keys; ++i) {
        Slot slot = get_slot(i);

        // Validate slot data stays within page bounds
        size_t end_offset = static_cast<size_t>(slot.offset) + slot.key_len + slot.val_len;
        if (slot.offset < LEAF_HEADER_SIZE || end_offset > Page::DATA_SIZE) {
            continue;  // Skip corrupted slot
        }

        const uint8_t* d = data();

        std::string key(reinterpret_cast<const char*>(d + slot.offset), slot.key_len);
        std::string value(reinterpret_cast<const char*>(d + slot.offset + slot.key_len), slot.val_len);

        result.emplace_back(std::move(key), std::move(value));
    }

    return result;
}

std::string LeafPage::get_key_at(size_t index) const {
    Slot slot = get_slot(index);

    // Validate slot data stays within page bounds
    size_t end_offset = static_cast<size_t>(slot.offset) + slot.key_len;
    if (slot.offset < LEAF_HEADER_SIZE || end_offset > Page::DATA_SIZE) {
        return "";  // Return empty string for corrupted slot
    }

    const uint8_t* d = data();
    return std::string(reinterpret_cast<const char*>(d + slot.offset), slot.key_len);
}

std::string LeafPage::get_min_key() const {
    if (page_->get_num_keys() == 0) {
        return "";
    }
    return get_key_at(0);
}

std::string LeafPage::split(LeafPage* new_leaf) {
    uint16_t num_keys = page_->get_num_keys();
    uint16_t mid = num_keys / 2;

    // Move second half to new leaf
    for (size_t i = mid; i < num_keys; ++i) {
        Slot slot = get_slot(i);
        const uint8_t* d = data();

        std::string key(reinterpret_cast<const char*>(d + slot.offset), slot.key_len);
        std::string value(reinterpret_cast<const char*>(d + slot.offset + slot.key_len), slot.val_len);

        new_leaf->insert(key, value);
    }

    // Update our metadata
    page_->set_num_keys(mid);
    set_free_space_offset(static_cast<uint16_t>(LEAF_HEADER_SIZE + mid * SLOT_SIZE));

    // Update sibling pointers
    PageId old_next = get_next_leaf();
    set_next_leaf(new_leaf->page_->get_page_id());
    new_leaf->set_prev_leaf(page_->get_page_id());
    new_leaf->set_next_leaf(old_next);

    return new_leaf->get_min_key();
}

bool LeafPage::is_half_full() const {
    uint16_t num_keys = page_->get_num_keys();
    // At least half the slots should be used
    size_t max_slots = (Page::DATA_SIZE - LEAF_HEADER_SIZE) / SLOT_SIZE;
    return num_keys >= max_slots / 2;
}

// ============================================================================
// InternalPage Implementation
// ============================================================================

void InternalPage::init() {
    set_first_child(INVALID_PAGE_ID);
    set_free_space_offset(INTERNAL_HEADER_SIZE);
    set_data_offset(static_cast<uint16_t>(Page::DATA_SIZE));
    page_->set_num_keys(0);
}

PageId InternalPage::get_first_child() const {
    const uint8_t* d = data();
    return *reinterpret_cast<const PageId*>(d);
}

void InternalPage::set_first_child(PageId id) {
    uint8_t* d = data();
    *reinterpret_cast<PageId*>(d) = id;
}

uint16_t InternalPage::get_free_space_offset() const {
    const uint8_t* d = data();
    return *reinterpret_cast<const uint16_t*>(d + 4);
}

void InternalPage::set_free_space_offset(uint16_t offset) {
    uint8_t* d = data();
    *reinterpret_cast<uint16_t*>(d + 4) = offset;
}

uint16_t InternalPage::get_data_offset() const {
    const uint8_t* d = data();
    return *reinterpret_cast<const uint16_t*>(d + 6);
}

void InternalPage::set_data_offset(uint16_t offset) {
    uint8_t* d = data();
    *reinterpret_cast<uint16_t*>(d + 6) = offset;
}

InternalPage::Slot InternalPage::get_slot(size_t index) const {
    const uint8_t* d = data() + INTERNAL_HEADER_SIZE + index * SLOT_SIZE;
    Slot slot;
    slot.child_id = *reinterpret_cast<const PageId*>(d);
    slot.offset = *reinterpret_cast<const uint16_t*>(d + 4);
    slot.key_len = *reinterpret_cast<const uint16_t*>(d + 6);
    return slot;
}

void InternalPage::set_slot(size_t index, const Slot& slot) {
    uint8_t* d = data() + INTERNAL_HEADER_SIZE + index * SLOT_SIZE;
    *reinterpret_cast<PageId*>(d) = slot.child_id;
    *reinterpret_cast<uint16_t*>(d + 4) = slot.offset;
    *reinterpret_cast<uint16_t*>(d + 6) = slot.key_len;
}

std::string InternalPage::get_key_at(size_t index) const {
    Slot slot = get_slot(index);
    const uint8_t* d = data();
    return std::string(reinterpret_cast<const char*>(d + slot.offset), slot.key_len);
}

PageId InternalPage::get_child_at(size_t index) const {
    if (index == 0) {
        return get_first_child();
    }
    return get_slot(index - 1).child_id;
}

PageId InternalPage::find_child(const std::string& key) const {
    uint16_t num_keys = page_->get_num_keys();

    // Binary search for the correct child
    size_t left = 0, right = num_keys;
    while (left < right) {
        size_t mid = left + (right - left) / 2;
        if (get_key_at(mid) <= key) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }

    return get_child_at(left);
}

bool InternalPage::has_space(size_t key_len) const {
    uint16_t free_start = get_free_space_offset();
    uint16_t data_start = get_data_offset();

    // Prevent underflow on corrupted pages
    if (data_start < free_start) {
        return false;
    }

    size_t needed = SLOT_SIZE + key_len;
    return (data_start - free_start) >= needed;
}

bool InternalPage::insert(const std::string& key, PageId right_child) {
    if (!has_space(key.size())) {
        return false;
    }

    uint16_t num_keys = page_->get_num_keys();

    // Find insertion position
    size_t pos = 0;
    while (pos < num_keys && get_key_at(pos) < key) {
        ++pos;
    }

    // Shift slots to make room
    for (size_t i = num_keys; i > pos; --i) {
        set_slot(i, get_slot(i - 1));
    }

    // Allocate space for key at the end
    uint16_t data_offset = get_data_offset();

    // Defensive check: prevent underflow on corrupted pages
    if (data_offset < key.size()) {
        return false;
    }
    data_offset -= static_cast<uint16_t>(key.size());

    // Write key
    uint8_t* d = data();
    std::memcpy(d + data_offset, key.data(), key.size());

    // Write slot
    Slot slot;
    slot.child_id = right_child;
    slot.offset = data_offset;
    slot.key_len = static_cast<uint16_t>(key.size());
    set_slot(pos, slot);

    // Update metadata
    set_data_offset(data_offset);
    set_free_space_offset(static_cast<uint16_t>(INTERNAL_HEADER_SIZE + (num_keys + 1) * SLOT_SIZE));
    page_->set_num_keys(num_keys + 1);

    return true;
}

bool InternalPage::remove(const std::string& key) {
    uint16_t num_keys = page_->get_num_keys();

    // Find the key
    size_t pos = 0;
    while (pos < num_keys && get_key_at(pos) < key) {
        ++pos;
    }

    if (pos >= num_keys || get_key_at(pos) != key) {
        return false;
    }

    // Shift slots to fill the gap
    for (size_t i = pos; i < num_keys - 1; ++i) {
        set_slot(i, get_slot(i + 1));
    }

    // Update metadata
    set_free_space_offset(static_cast<uint16_t>(INTERNAL_HEADER_SIZE + (num_keys - 1) * SLOT_SIZE));
    page_->set_num_keys(num_keys - 1);

    return true;
}

std::string InternalPage::split(InternalPage* new_internal) {
    uint16_t num_keys = page_->get_num_keys();
    uint16_t mid = num_keys / 2;

    // The middle key will be promoted to the parent
    std::string promoted_key = get_key_at(mid);

    // Move keys after mid to new internal node
    // The first child of new_internal is the right child of promoted_key
    new_internal->set_first_child(get_slot(mid).child_id);

    for (size_t i = mid + 1; i < num_keys; ++i) {
        std::string key = get_key_at(i);
        PageId child = get_slot(i).child_id;
        new_internal->insert(key, child);
    }

    // Update our metadata
    page_->set_num_keys(mid);
    set_free_space_offset(static_cast<uint16_t>(INTERNAL_HEADER_SIZE + mid * SLOT_SIZE));

    return promoted_key;
}

}  // namespace dam
