#pragma once

#include <dam/core_types.hpp>
#include <dam/result.hpp>
#include <dam/storage/page.hpp>
#include <dam/storage/disk_manager.hpp>
#include <dam/storage/lru_replacer.hpp>

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace dam {

/**
 * BufferPool - Manages a pool of in-memory page frames.
 *
 * Provides:
 * - Page fetching with automatic disk I/O
 * - Pin/unpin semantics for concurrency control
 * - LRU eviction of unpinned pages
 * - Dirty page tracking and flushing
 */
class BufferPool {
public:
    /**
     * Create a buffer pool.
     *
     * @param pool_size Number of page frames in the pool
     * @param disk_manager The disk manager for I/O
     */
    BufferPool(size_t pool_size, DiskManager* disk_manager);

    ~BufferPool();

    // Prevent copying
    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    /**
     * Fetch a page from the buffer pool.
     * If the page is not in memory, it will be loaded from disk.
     * The page's pin count is incremented.
     *
     * @param page_id The page to fetch
     * @return Pointer to the page, or nullptr if failed
     */
    Page* fetch_page(PageId page_id);

    /**
     * Allocate a new page in the buffer pool.
     * The new page is pinned with pin_count = 1.
     *
     * @return Pointer to the new page, or nullptr if failed
     */
    Page* new_page();

    /**
     * Unpin a page, decrementing its pin count.
     * When pin_count reaches 0, the page becomes evictable.
     *
     * @param page_id The page to unpin
     * @param is_dirty Mark the page as dirty if true
     * @return true if the page was successfully unpinned
     */
    bool unpin_page(PageId page_id, bool is_dirty = false);

    /**
     * Mark a page as dirty.
     *
     * @param page_id The page to mark
     */
    void mark_dirty(PageId page_id);

    /**
     * Flush a specific page to disk.
     *
     * @param page_id The page to flush
     * @return Result indicating success or failure
     */
    Result<void> flush_page(PageId page_id);

    /**
     * Flush all dirty pages to disk.
     *
     * @return Result indicating success or failure
     */
    Result<void> flush_all_pages();

    /**
     * Delete a page from the buffer pool and disk.
     *
     * @param page_id The page to delete
     * @return true if the page was successfully deleted
     */
    bool delete_page(PageId page_id);

    /**
     * Get the number of pages currently in the buffer pool.
     */
    size_t get_pool_size() const { return pool_size_; }

    /**
     * Get the number of free frames in the pool.
     */
    size_t get_free_frame_count() const;

    /**
     * Check if a page is in the buffer pool.
     */
    bool contains_page(PageId page_id) const;

    /**
     * Get the pin count of a page.
     * Returns 0 if the page is not in the buffer pool.
     */
    uint32_t get_pin_count(PageId page_id) const;

private:
    // Find a victim frame for eviction
    // Returns the frame index or -1 if all frames are pinned
    int find_victim_frame();

    // Evict a page from a frame
    Result<void> evict_page(size_t frame_id);

    struct FrameInfo {
        PageId page_id = INVALID_PAGE_ID;
        bool is_dirty = false;
        uint32_t pin_count = 0;
    };

    size_t pool_size_;
    DiskManager* disk_manager_;

    // Page frames
    std::vector<std::unique_ptr<Page>> pages_;
    std::vector<FrameInfo> frame_info_;

    // Page table: page_id -> frame_id
    std::unordered_map<PageId, size_t> page_table_;

    // Free frames list
    std::vector<size_t> free_frames_;

    // LRU replacer for eviction
    LRUReplacer replacer_;

    mutable std::mutex mutex_;
};

}  // namespace dam
