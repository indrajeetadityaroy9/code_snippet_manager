#include <dam/storage/btree.hpp>
#include <stack>

namespace dam {

BPlusTree::BPlusTree(BufferPool* buffer_pool, PageId root_page_id)
    : buffer_pool_(buffer_pool)
    , root_page_id_(root_page_id)
    , size_(0)
{
    if (root_page_id_ == INVALID_PAGE_ID) {
        init_empty_tree();
    }
}

void BPlusTree::init_empty_tree() {
    // Create a new leaf page as the root
    Page* root_page = buffer_pool_->new_page();
    if (!root_page) {
        return;
    }

    root_page_id_ = root_page->get_page_id();

    // Initialize as leaf - LeafPage constructor will set type and call init()
    LeafPage leaf(root_page);

    buffer_pool_->unpin_page(root_page_id_, true);
}

PageId BPlusTree::find_leaf(const std::string& key) const {
    if (root_page_id_ == INVALID_PAGE_ID) {
        return INVALID_PAGE_ID;
    }

    PageId current_id = root_page_id_;

    while (true) {
        Page* page = buffer_pool_->fetch_page(current_id);
        if (!page) {
            return INVALID_PAGE_ID;
        }

        if (page->is_leaf()) {
            buffer_pool_->unpin_page(current_id, false);
            return current_id;
        }

        // Internal node - find child
        InternalPage internal(page);
        PageId child_id = internal.find_child(key);
        buffer_pool_->unpin_page(current_id, false);
        current_id = child_id;
    }
}

std::optional<std::string> BPlusTree::find(const std::string& key) const {
    PageId leaf_id = find_leaf(key);
    if (leaf_id == INVALID_PAGE_ID) {
        return std::nullopt;
    }

    Page* page = buffer_pool_->fetch_page(leaf_id);
    if (!page) {
        return std::nullopt;
    }

    LeafPage leaf(page);
    std::string value;
    bool found = leaf.find(key, &value);

    buffer_pool_->unpin_page(leaf_id, false);

    if (found) {
        return value;
    }
    return std::nullopt;
}

bool BPlusTree::contains(const std::string& key) const {
    return find(key).has_value();
}

bool BPlusTree::insert(const std::string& key, const std::string& value) {
    if (root_page_id_ == INVALID_PAGE_ID) {
        init_empty_tree();
    }

    PageId leaf_id = find_leaf(key);
    if (leaf_id == INVALID_PAGE_ID) {
        return false;
    }

    return insert_into_leaf(leaf_id, key, value);
}

bool BPlusTree::insert_into_leaf(PageId leaf_id, const std::string& key, const std::string& value) {
    Page* page = buffer_pool_->fetch_page(leaf_id);
    if (!page) {
        return false;
    }

    LeafPage leaf(page);

    // Check if key already exists
    if (leaf.find(key, nullptr)) {
        buffer_pool_->unpin_page(leaf_id, false);
        return false;  // Duplicate key
    }

    // Try to insert directly
    if (leaf.has_space(key.size(), value.size())) {
        bool success = leaf.insert(key, value);
        buffer_pool_->unpin_page(leaf_id, true);
        if (success) {
            ++size_;
        }
        return success;
    }

    buffer_pool_->unpin_page(leaf_id, false);

    // Need to split
    PageId new_leaf_id = split_leaf(leaf_id, key, value);
    if (new_leaf_id == INVALID_PAGE_ID) {
        return false;
    }

    ++size_;
    return true;
}

PageId BPlusTree::split_leaf(PageId leaf_id, const std::string& key, const std::string& value) {
    // Create new leaf page
    Page* new_page = buffer_pool_->new_page();
    if (!new_page) {
        return INVALID_PAGE_ID;
    }
    PageId new_leaf_id = new_page->get_page_id();

    Page* old_page = buffer_pool_->fetch_page(leaf_id);
    if (!old_page) {
        buffer_pool_->delete_page(new_leaf_id);
        return INVALID_PAGE_ID;
    }

    LeafPage old_leaf(old_page);
    // LeafPage constructor will set type and call init() for new page
    LeafPage new_leaf(new_page);

    // Get all existing entries
    auto entries = old_leaf.get_all();

    // Save page metadata BEFORE reset
    PageId old_prev = old_leaf.get_prev_leaf();
    PageId old_next = old_leaf.get_next_leaf();
    PageId old_parent = old_page->get_parent_page_id();

    // Add the new entry and sort
    entries.emplace_back(key, value);
    std::sort(entries.begin(), entries.end());

    // Clear old leaf and redistribute
    old_page->reset();
    old_page->set_page_id(leaf_id);
    old_page->set_parent_page_id(old_parent);  // Restore parent pointer
    // Don't set node_type - LeafPage constructor will set it and call init()
    LeafPage reset_old(old_page);

    size_t mid = entries.size() / 2;

    // First half stays in old leaf
    for (size_t i = 0; i < mid; ++i) {
        reset_old.insert(entries[i].first, entries[i].second);
    }

    // Second half goes to new leaf
    for (size_t i = mid; i < entries.size(); ++i) {
        new_leaf.insert(entries[i].first, entries[i].second);
    }

    // Update sibling pointers using saved values
    reset_old.set_prev_leaf(old_prev);
    reset_old.set_next_leaf(new_leaf_id);
    new_leaf.set_prev_leaf(leaf_id);
    new_leaf.set_next_leaf(old_next);

    // Update the old next leaf's prev pointer
    if (old_next != INVALID_PAGE_ID) {
        Page* next_page = buffer_pool_->fetch_page(old_next);
        if (next_page) {
            LeafPage next_leaf(next_page);
            next_leaf.set_prev_leaf(new_leaf_id);
            buffer_pool_->unpin_page(old_next, true);
        }
    }

    // Get the first key of new leaf for parent
    std::string split_key = new_leaf.get_min_key();

    buffer_pool_->unpin_page(leaf_id, true);
    buffer_pool_->unpin_page(new_leaf_id, true);

    // Insert into parent
    insert_into_parent(leaf_id, split_key, new_leaf_id);

    return new_leaf_id;
}

void BPlusTree::insert_into_parent(PageId left_id, const std::string& key, PageId right_id) {
    // Find the parent of left_id
    Page* left_page = buffer_pool_->fetch_page(left_id);
    if (!left_page) return;

    PageId parent_id = left_page->get_parent_page_id();
    buffer_pool_->unpin_page(left_id, false);

    if (parent_id == INVALID_PAGE_ID) {
        // Left was the root - create new root
        Page* new_root = buffer_pool_->new_page();
        if (!new_root) return;

        PageId new_root_id = new_root->get_page_id();

        // InternalPage constructor will set type and call init()
        InternalPage root_internal(new_root);
        root_internal.set_first_child(left_id);
        root_internal.insert(key, right_id);

        // Update children's parent pointers
        left_page = buffer_pool_->fetch_page(left_id);
        if (left_page) {
            left_page->set_parent_page_id(new_root_id);
            buffer_pool_->unpin_page(left_id, true);
        }

        Page* right_page = buffer_pool_->fetch_page(right_id);
        if (right_page) {
            right_page->set_parent_page_id(new_root_id);
            buffer_pool_->unpin_page(right_id, true);
        }

        buffer_pool_->unpin_page(new_root_id, true);
        root_page_id_ = new_root_id;
        return;
    }

    // Insert into existing parent
    Page* parent_page = buffer_pool_->fetch_page(parent_id);
    if (!parent_page) return;

    InternalPage parent(parent_page);

    if (parent.has_space(key.size())) {
        parent.insert(key, right_id);

        // Update right child's parent
        Page* right_page = buffer_pool_->fetch_page(right_id);
        if (right_page) {
            right_page->set_parent_page_id(parent_id);
            buffer_pool_->unpin_page(right_id, true);
        }

        buffer_pool_->unpin_page(parent_id, true);
        return;
    }

    // Need to split parent
    buffer_pool_->unpin_page(parent_id, false);
    split_internal(parent_id, key, right_id);
}

PageId BPlusTree::split_internal(PageId internal_id, const std::string& key, PageId right_child) {
    Page* new_page = buffer_pool_->new_page();
    if (!new_page) return INVALID_PAGE_ID;

    PageId new_internal_id = new_page->get_page_id();

    Page* old_page = buffer_pool_->fetch_page(internal_id);
    if (!old_page) {
        buffer_pool_->delete_page(new_internal_id);
        return INVALID_PAGE_ID;
    }

    InternalPage old_internal(old_page);
    // InternalPage constructor will set type and call init() for new page
    InternalPage new_internal(new_page);

    // Collect all keys and children
    std::vector<std::pair<std::string, PageId>> entries;
    uint16_t num_keys = old_page->get_num_keys();

    for (size_t i = 0; i < num_keys; ++i) {
        entries.emplace_back(old_internal.get_key_at(i), old_internal.get_child_at(i + 1));
    }

    // Add new entry and sort
    entries.emplace_back(key, right_child);
    std::sort(entries.begin(), entries.end());

    // Find split point
    size_t mid = entries.size() / 2;
    std::string promoted_key = entries[mid].first;

    // Save page metadata BEFORE reset
    PageId first_child = old_internal.get_first_child();
    PageId old_parent = old_page->get_parent_page_id();

    // Reset old internal and redistribute
    old_page->reset();
    old_page->set_page_id(internal_id);
    old_page->set_parent_page_id(old_parent);  // Restore parent pointer
    // Don't set node_type - InternalPage constructor will set it and call init()
    InternalPage reset_old(old_page);
    reset_old.set_first_child(first_child);

    // First half stays in old internal
    for (size_t i = 0; i < mid; ++i) {
        reset_old.insert(entries[i].first, entries[i].second);
    }

    // Set first child of new internal
    new_internal.set_first_child(entries[mid].second);

    // Second half (after mid) goes to new internal
    for (size_t i = mid + 1; i < entries.size(); ++i) {
        new_internal.insert(entries[i].first, entries[i].second);
    }

    // Update parent pointers of children that moved
    for (size_t i = mid; i < entries.size(); ++i) {
        Page* child_page = buffer_pool_->fetch_page(entries[i].second);
        if (child_page) {
            child_page->set_parent_page_id(new_internal_id);
            buffer_pool_->unpin_page(entries[i].second, true);
        }
    }

    buffer_pool_->unpin_page(internal_id, true);
    buffer_pool_->unpin_page(new_internal_id, true);

    // Insert promoted key into parent
    insert_into_parent(internal_id, promoted_key, new_internal_id);

    return new_internal_id;
}

bool BPlusTree::remove(const std::string& key) {
    PageId leaf_id = find_leaf(key);
    if (leaf_id == INVALID_PAGE_ID) {
        return false;
    }

    Page* page = buffer_pool_->fetch_page(leaf_id);
    if (!page) {
        return false;
    }

    LeafPage leaf(page);

    // Check if key exists
    if (!leaf.find(key, nullptr)) {
        buffer_pool_->unpin_page(leaf_id, false);
        return false;
    }

    bool success = leaf.remove(key);
    buffer_pool_->unpin_page(leaf_id, true);

    if (success) {
        --size_;
        // Note: For simplicity, we don't handle underflow/merging here
        // A production implementation would coalesce underflowed nodes
    }

    return success;
}

bool BPlusTree::update(const std::string& key, const std::string& value) {
    PageId leaf_id = find_leaf(key);
    if (leaf_id == INVALID_PAGE_ID) {
        return false;
    }

    Page* page = buffer_pool_->fetch_page(leaf_id);
    if (!page) {
        return false;
    }

    LeafPage leaf(page);
    bool success = leaf.update(key, value);
    buffer_pool_->unpin_page(leaf_id, success);

    if (!success) {
        // Update failed (likely due to value size increase and no space).
        // Fall back to tree-level remove + insert which can trigger a split.
        // Note: LeafPage::update now checks space before modifying,
        // so the key should still exist at this point.
        // remove() decrements size_, insert() increments it, so net effect is zero.
        if (!remove(key)) {
            return false;  // Key doesn't exist
        }
        return insert(key, value);
    }

    return true;
}

std::vector<std::pair<std::string, std::string>> BPlusTree::range(
    const std::string& start_key,
    const std::string& end_key) const
{
    std::vector<std::pair<std::string, std::string>> result;

    PageId leaf_id = find_leaf(start_key);
    if (leaf_id == INVALID_PAGE_ID) {
        return result;
    }

    while (leaf_id != INVALID_PAGE_ID) {
        Page* page = buffer_pool_->fetch_page(leaf_id);
        if (!page) break;

        LeafPage leaf(page);
        auto entries = leaf.get_all();

        for (const auto& entry : entries) {
            if (entry.first > end_key) {
                buffer_pool_->unpin_page(leaf_id, false);
                return result;
            }
            if (entry.first >= start_key) {
                result.push_back(entry);
            }
        }

        PageId next_leaf = leaf.get_next_leaf();
        buffer_pool_->unpin_page(leaf_id, false);
        leaf_id = next_leaf;
    }

    return result;
}

std::vector<std::pair<std::string, std::string>> BPlusTree::scan(
    const std::string& start_key,
    size_t limit) const
{
    std::vector<std::pair<std::string, std::string>> result;

    PageId leaf_id = find_leaf(start_key);
    if (leaf_id == INVALID_PAGE_ID) {
        return result;
    }

    while (leaf_id != INVALID_PAGE_ID && result.size() < limit) {
        Page* page = buffer_pool_->fetch_page(leaf_id);
        if (!page) break;

        LeafPage leaf(page);
        auto entries = leaf.get_all();

        for (const auto& entry : entries) {
            if (entry.first >= start_key) {
                result.push_back(entry);
                if (result.size() >= limit) {
                    break;
                }
            }
        }

        PageId next_leaf = leaf.get_next_leaf();
        buffer_pool_->unpin_page(leaf_id, false);
        leaf_id = next_leaf;
    }

    return result;
}

std::vector<std::pair<std::string, std::string>> BPlusTree::get_all() const {
    std::vector<std::pair<std::string, std::string>> result;

    PageId leaf_id = get_leftmost_leaf();

    while (leaf_id != INVALID_PAGE_ID) {
        Page* page = buffer_pool_->fetch_page(leaf_id);
        if (!page) break;

        LeafPage leaf(page);
        auto entries = leaf.get_all();
        result.insert(result.end(), entries.begin(), entries.end());

        PageId next_leaf = leaf.get_next_leaf();
        buffer_pool_->unpin_page(leaf_id, false);
        leaf_id = next_leaf;
    }

    return result;
}

void BPlusTree::for_each(
    const std::function<bool(const std::string&, const std::string&)>& callback) const
{
    PageId leaf_id = get_leftmost_leaf();

    while (leaf_id != INVALID_PAGE_ID) {
        Page* page = buffer_pool_->fetch_page(leaf_id);
        if (!page) break;

        LeafPage leaf(page);
        auto entries = leaf.get_all();

        bool should_continue = true;
        for (const auto& entry : entries) {
            if (!callback(entry.first, entry.second)) {
                should_continue = false;
                break;
            }
        }

        PageId next_leaf = leaf.get_next_leaf();
        buffer_pool_->unpin_page(leaf_id, false);

        if (!should_continue) break;
        leaf_id = next_leaf;
    }
}

PageId BPlusTree::get_leftmost_leaf() const {
    if (root_page_id_ == INVALID_PAGE_ID) {
        return INVALID_PAGE_ID;
    }

    PageId current_id = root_page_id_;

    while (true) {
        Page* page = buffer_pool_->fetch_page(current_id);
        if (!page) {
            return INVALID_PAGE_ID;
        }

        if (page->is_leaf()) {
            buffer_pool_->unpin_page(current_id, false);
            return current_id;
        }

        InternalPage internal(page);
        PageId child_id = internal.get_first_child();
        buffer_pool_->unpin_page(current_id, false);
        current_id = child_id;
    }
}

size_t BPlusTree::height() const {
    if (root_page_id_ == INVALID_PAGE_ID) {
        return 0;
    }

    size_t h = 1;
    PageId current_id = root_page_id_;

    while (true) {
        Page* page = buffer_pool_->fetch_page(current_id);
        if (!page) {
            return h;
        }

        if (page->is_leaf()) {
            buffer_pool_->unpin_page(current_id, false);
            return h;
        }

        InternalPage internal(page);
        PageId child_id = internal.get_first_child();
        buffer_pool_->unpin_page(current_id, false);
        current_id = child_id;
        ++h;
    }
}

bool BPlusTree::verify() const {
    if (root_page_id_ == INVALID_PAGE_ID) {
        return size_ == 0;
    }

    // Count all entries and verify sorted order
    size_t count = 0;
    std::string prev_key;
    bool first = true;
    bool valid = true;

    for_each([&](const std::string& key, const std::string&) {
        ++count;
        if (!first && key <= prev_key) {
            valid = false;
            return false;
        }
        prev_key = key;
        first = false;
        return true;
    });

    return valid && count == size_;
}

// ============================================================================
// BPlusTreeIterator Implementation
// ============================================================================

BPlusTreeIterator::BPlusTreeIterator(BufferPool* buffer_pool, PageId leaf_id, size_t index)
    : buffer_pool_(buffer_pool)
    , leaf_id_(leaf_id)
    , index_(index)
{}

std::pair<std::string, std::string> BPlusTreeIterator::operator*() const {
    if (!valid()) {
        return {"", ""};
    }

    Page* page = buffer_pool_->fetch_page(leaf_id_);
    if (!page) {
        return {"", ""};
    }

    LeafPage leaf(page);
    auto entries = leaf.get_all();

    std::pair<std::string, std::string> result;
    if (index_ < entries.size()) {
        result = entries[index_];
    }

    buffer_pool_->unpin_page(leaf_id_, false);
    return result;
}

BPlusTreeIterator& BPlusTreeIterator::operator++() {
    if (!valid()) {
        return *this;
    }

    Page* page = buffer_pool_->fetch_page(leaf_id_);
    if (!page) {
        leaf_id_ = INVALID_PAGE_ID;
        return *this;
    }

    LeafPage leaf(page);
    size_t num_keys = page->get_num_keys();

    if (index_ + 1 < num_keys) {
        ++index_;
    } else {
        // Move to next leaf
        leaf_id_ = leaf.get_next_leaf();
        index_ = 0;
    }

    buffer_pool_->unpin_page(page->get_page_id(), false);
    return *this;
}

bool BPlusTreeIterator::operator==(const BPlusTreeIterator& other) const {
    return leaf_id_ == other.leaf_id_ && index_ == other.index_;
}

}  // namespace dam
