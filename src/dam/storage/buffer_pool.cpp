#include <dam/storage/buffer_pool.hpp>

namespace dam {

BufferPool::BufferPool(size_t pool_size, DiskManager* disk_manager)
    : pool_size_(pool_size)
    , disk_manager_(disk_manager)
    , replacer_(pool_size)
{
    // Allocate page frames
    pages_.reserve(pool_size);
    frame_info_.resize(pool_size);

    for (size_t i = 0; i < pool_size; ++i) {
        pages_.push_back(std::make_unique<Page>());
        free_frames_.push_back(i);
    }
}

BufferPool::~BufferPool() {
    // Flush all dirty pages before destruction
    flush_all_pages();
}

Page* BufferPool::fetch_page(PageId page_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (page_id == INVALID_PAGE_ID) {
        return nullptr;
    }

    // Check if page is already in buffer pool
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        size_t frame_id = it->second;
        frame_info_[frame_id].pin_count++;
        replacer_.pin(frame_id);  // Remove from eviction candidates
        return pages_[frame_id].get();
    }

    // Need to load from disk - find a frame
    int frame_id = find_victim_frame();
    if (frame_id < 0) {
        return nullptr;  // All frames pinned
    }

    // Evict current page in frame if any
    if (frame_info_[frame_id].page_id != INVALID_PAGE_ID) {
        auto result = evict_page(static_cast<size_t>(frame_id));
        if (!result.ok()) {
            return nullptr;
        }
    }

    // Load page from disk
    Page* page = pages_[frame_id].get();
    auto result = disk_manager_->read_page(page_id, page->get_raw_data());
    if (!result.ok()) {
        // Page might not exist yet on disk, initialize empty
        page->reset();
        page->set_page_id(page_id);
    }

    // Update metadata
    frame_info_[frame_id].page_id = page_id;
    frame_info_[frame_id].is_dirty = false;
    frame_info_[frame_id].pin_count = 1;
    page_table_[page_id] = static_cast<size_t>(frame_id);
    replacer_.pin(static_cast<size_t>(frame_id));

    return page;
}

Page* BufferPool::new_page() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Allocate a new page on disk
    PageId page_id = disk_manager_->allocate_page();
    if (page_id == INVALID_PAGE_ID) {
        return nullptr;
    }

    // Find a frame for the new page
    int frame_id = find_victim_frame();
    if (frame_id < 0) {
        disk_manager_->deallocate_page(page_id);
        return nullptr;
    }

    // Evict current page in frame if any
    if (frame_info_[frame_id].page_id != INVALID_PAGE_ID) {
        auto result = evict_page(static_cast<size_t>(frame_id));
        if (!result.ok()) {
            disk_manager_->deallocate_page(page_id);
            return nullptr;
        }
    }

    // Initialize the new page
    Page* page = pages_[frame_id].get();
    page->reset();
    page->set_page_id(page_id);

    // Update metadata
    frame_info_[frame_id].page_id = page_id;
    frame_info_[frame_id].is_dirty = true;  // New page is dirty
    frame_info_[frame_id].pin_count = 1;
    page_table_[page_id] = static_cast<size_t>(frame_id);
    replacer_.pin(static_cast<size_t>(frame_id));

    return page;
}

bool BufferPool::unpin_page(PageId page_id, bool is_dirty) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }

    size_t frame_id = it->second;
    FrameInfo& info = frame_info_[frame_id];

    if (info.pin_count == 0) {
        return false;  // Already unpinned
    }

    info.pin_count--;
    if (is_dirty) {
        info.is_dirty = true;
    }

    if (info.pin_count == 0) {
        replacer_.unpin(frame_id);  // Add to eviction candidates
    }

    return true;
}

void BufferPool::mark_dirty(PageId page_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        frame_info_[it->second].is_dirty = true;
    }
}

Result<void> BufferPool::flush_page(PageId page_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return Ok();  // Not in buffer pool, nothing to flush
    }

    size_t frame_id = it->second;
    FrameInfo& info = frame_info_[frame_id];

    if (info.is_dirty) {
        Page* page = pages_[frame_id].get();
        page->update_checksum();

        auto result = disk_manager_->write_page(page_id, page->get_raw_data());
        if (!result.ok()) {
            return result;
        }

        info.is_dirty = false;
    }

    return Ok();
}

Result<void> BufferPool::flush_all_pages() {
    std::lock_guard<std::mutex> lock(mutex_);

    for (size_t i = 0; i < pool_size_; ++i) {
        if (frame_info_[i].page_id != INVALID_PAGE_ID && frame_info_[i].is_dirty) {
            Page* page = pages_[i].get();
            page->update_checksum();

            auto result = disk_manager_->write_page(
                frame_info_[i].page_id, page->get_raw_data());
            if (!result.ok()) {
                return result;
            }

            frame_info_[i].is_dirty = false;
        }
    }

    return disk_manager_->flush();
}

bool BufferPool::delete_page(PageId page_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        size_t frame_id = it->second;
        FrameInfo& info = frame_info_[frame_id];

        if (info.pin_count > 0) {
            return false;  // Cannot delete a pinned page
        }

        // Remove from replacer and page table
        replacer_.pin(frame_id);  // Remove from eviction candidates
        page_table_.erase(it);

        // Reset frame
        info.page_id = INVALID_PAGE_ID;
        info.is_dirty = false;
        info.pin_count = 0;
        pages_[frame_id]->reset();
        free_frames_.push_back(frame_id);
    }

    // Deallocate on disk
    disk_manager_->deallocate_page(page_id);
    return true;
}

size_t BufferPool::get_free_frame_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return free_frames_.size() + replacer_.size();
}

bool BufferPool::contains_page(PageId page_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return page_table_.find(page_id) != page_table_.end();
}

uint32_t BufferPool::get_pin_count(PageId page_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return 0;
    }
    return frame_info_[it->second].pin_count;
}

int BufferPool::find_victim_frame() {
    // First, try to get a free frame
    if (!free_frames_.empty()) {
        size_t frame_id = free_frames_.back();
        free_frames_.pop_back();
        return static_cast<int>(frame_id);
    }

    // Otherwise, find a victim using LRU
    auto victim = replacer_.victim();
    if (victim.has_value()) {
        return static_cast<int>(victim.value());
    }

    return -1;  // All frames are pinned
}

Result<void> BufferPool::evict_page(size_t frame_id) {
    FrameInfo& info = frame_info_[frame_id];

    if (info.is_dirty) {
        // Write dirty page to disk
        Page* page = pages_[frame_id].get();
        page->update_checksum();

        auto result = disk_manager_->write_page(info.page_id, page->get_raw_data());
        if (!result.ok()) {
            return result;
        }
    }

    // Remove from page table
    page_table_.erase(info.page_id);

    // Reset frame info
    info.page_id = INVALID_PAGE_ID;
    info.is_dirty = false;
    info.pin_count = 0;

    return Ok();
}

}  // namespace dam
