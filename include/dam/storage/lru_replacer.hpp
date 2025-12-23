#pragma once

#include <cstddef>
#include <list>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace dam {

/**
 * LRUReplacer - Tracks unpinned pages and selects victims using LRU policy.
 *
 * When a page is unpinned (pin_count reaches 0), it becomes a candidate for
 * eviction. When a new page needs to be loaded and no free frames exist,
 * the replacer selects the least recently used unpinned page as the victim.
 */
class LRUReplacer {
public:
    /**
     * Create an LRU replacer with the specified capacity.
     *
     * @param capacity Maximum number of frames that can be tracked
     */
    explicit LRUReplacer(size_t capacity);

    /**
     * Add a frame to the replacer (called when page is unpinned).
     * If the frame is already in the replacer, moves it to MRU position.
     *
     * @param frame_id The frame to add/update
     */
    void unpin(size_t frame_id);

    /**
     * Remove a frame from the replacer (called when page is pinned).
     * The frame is no longer a candidate for eviction.
     *
     * @param frame_id The frame to remove
     */
    void pin(size_t frame_id);

    /**
     * Select a victim frame for eviction.
     * Returns the least recently used frame and removes it from the replacer.
     *
     * @return The victim frame ID, or std::nullopt if no victims available
     */
    std::optional<size_t> victim();

    /**
     * Get the current number of frames in the replacer.
     */
    size_t size() const;

    /**
     * Check if the replacer is empty.
     */
    bool empty() const { return size() == 0; }

    /**
     * Check if a frame is in the replacer (unpinned).
     */
    bool contains(size_t frame_id) const;

private:
    size_t capacity_;

    // Doubly-linked list: front = MRU, back = LRU
    std::list<size_t> lru_list_;

    // Map from frame_id to iterator in lru_list_
    std::unordered_map<size_t, std::list<size_t>::iterator> frame_map_;

    mutable std::mutex mutex_;
};

}  // namespace dam
