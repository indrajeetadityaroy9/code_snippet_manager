#include <dam/storage/lru_replacer.hpp>

namespace dam {

LRUReplacer::LRUReplacer(size_t capacity)
    : capacity_(capacity)
{}

void LRUReplacer::unpin(size_t frame_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = frame_map_.find(frame_id);
    if (it != frame_map_.end()) {
        // Already in replacer, move to front (MRU)
        lru_list_.erase(it->second);
        lru_list_.push_front(frame_id);
        it->second = lru_list_.begin();
    } else {
        // Add new frame at front (MRU)
        if (lru_list_.size() >= capacity_) {
            // Should not happen if used correctly with buffer pool
            return;
        }
        lru_list_.push_front(frame_id);
        frame_map_[frame_id] = lru_list_.begin();
    }
}

void LRUReplacer::pin(size_t frame_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = frame_map_.find(frame_id);
    if (it != frame_map_.end()) {
        lru_list_.erase(it->second);
        frame_map_.erase(it);
    }
}

std::optional<size_t> LRUReplacer::victim() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (lru_list_.empty()) {
        return std::nullopt;
    }

    // Evict from back (LRU)
    size_t victim_id = lru_list_.back();
    lru_list_.pop_back();
    frame_map_.erase(victim_id);

    return victim_id;
}

size_t LRUReplacer::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lru_list_.size();
}

bool LRUReplacer::contains(size_t frame_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frame_map_.find(frame_id) != frame_map_.end();
}

}  // namespace dam
