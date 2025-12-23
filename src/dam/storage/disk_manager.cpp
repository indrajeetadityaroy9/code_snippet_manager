#include <dam/storage/disk_manager.hpp>
#include <dam/util/crc32.hpp>
#include <algorithm>
#include <cstring>

namespace dam {

// File header structure (stored in page 0)
// Fixed header fields: 28 bytes, free list fills remaining space
constexpr size_t FREE_LIST_HEADER_SIZE = 28;  // magic(8) + version(4) + num_pages(4) + next_page_id(4) + free_list_count(4) + checksum(4)
constexpr size_t FREE_LIST_MAX_ENTRIES = (PAGE_SIZE - FREE_LIST_HEADER_SIZE) / sizeof(PageId);  // 1017 entries

struct FileHeader {
    char magic[8] = {'D', 'O', 'C', 'S', 'T', 'O', 'R', 'E'};
    uint32_t version = 1;
    PageId num_pages = 1;  // Header is page 0
    PageId next_page_id = 1;
    uint32_t free_list_count = 0;  // Number of free pages stored
    uint32_t checksum = 0;  // CRC32 checksum of header (excluding free_list)
    PageId free_list[FREE_LIST_MAX_ENTRIES] = {};  // Free page IDs

    // Compute checksum over header data (fixed fields only, 24 bytes)
    uint32_t compute_checksum() const {
        return CRC32::compute(reinterpret_cast<const char*>(this), 24);
    }

    void update_checksum() {
        checksum = compute_checksum();
    }

    bool verify_checksum() const {
        return checksum == compute_checksum();
    }
};

static_assert(sizeof(FileHeader) == PAGE_SIZE, "FileHeader must be PAGE_SIZE");

DiskManager::DiskManager(const fs::path& db_path)
    : db_path_(db_path)
    , num_pages_(0)
    , next_page_id_(1)
    , is_open_(false)
{
    auto result = open_file();
    if (!result.ok()) {
        // Log error but don't throw - check is_valid() instead
        is_open_ = false;
    }
}

DiskManager::~DiskManager() {
    // Implementation moved to non-virtual close method would be cleaner
    // but for now just check and close
    if (is_open_) {
        flush();
        write_header();
        db_file_.close();
    }
}

// ============================================================================
// InMemoryDiskManager Implementation
// ============================================================================

InMemoryDiskManager::InMemoryDiskManager()
    : DiskManager(fs::temp_directory_path() / "inmemory_dummy.db")
    , next_page_id_(1)
{}

Result<void> DiskManager::open_file() {
    std::lock_guard<std::mutex> lock(mutex_);

    bool file_exists = fs::exists(db_path_);

    if (file_exists) {
        // Open existing file
        db_file_.open(db_path_, std::ios::in | std::ios::out | std::ios::binary);
        if (!db_file_.is_open()) {
            return Error(ErrorCode::IO_ERROR, "Failed to open database file");
        }

        auto result = read_header();
        if (!result.ok()) {
            return result;
        }
    } else {
        // Create new file
        fs::create_directories(db_path_.parent_path());
        db_file_.open(db_path_, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
        if (!db_file_.is_open()) {
            return Error(ErrorCode::IO_ERROR, "Failed to create database file");
        }

        num_pages_ = 1;
        next_page_id_ = 1;

        auto result = write_header();
        if (!result.ok()) {
            return result;
        }
    }

    is_open_ = true;
    return Ok();
}

Result<void> DiskManager::read_header() {
    FileHeader header;
    db_file_.seekg(0);
    db_file_.read(reinterpret_cast<char*>(&header), sizeof(header));

    if (!db_file_.good()) {
        return Error(ErrorCode::IO_ERROR, "Failed to read file header");
    }

    // Verify magic
    if (std::memcmp(header.magic, "DOCSTORE", 8) != 0) {
        return Error(ErrorCode::CORRUPTION, "Invalid database file format");
    }

    // Verify checksum (skip for legacy files where checksum was 0)
    if (header.checksum != 0 && !header.verify_checksum()) {
        return Error(ErrorCode::CORRUPTION, "Header checksum mismatch");
    }

    num_pages_ = header.num_pages;
    next_page_id_ = header.next_page_id;

    // Load free list from header
    free_pages_.clear();
    if (header.free_list_count > 0 && header.free_list_count <= FREE_LIST_MAX_ENTRIES) {
        free_pages_.reserve(header.free_list_count);
        for (uint32_t i = 0; i < header.free_list_count; ++i) {
            free_pages_.push_back(header.free_list[i]);
        }
    }

    return Ok();
}

Result<void> DiskManager::write_header() {
    FileHeader header;
    header.num_pages = num_pages_;
    header.next_page_id = next_page_id_;

    // Check for free list overflow - fail if we'd lose pages
    if (free_pages_.size() > FREE_LIST_MAX_ENTRIES) {
        return Error(ErrorCode::OUT_OF_SPACE,
                     "Free list overflow: " + std::to_string(free_pages_.size()) +
                     " pages exceed max " + std::to_string(FREE_LIST_MAX_ENTRIES) +
                     ". Run compaction to reclaim space.");
    }

    // Save free list
    header.free_list_count = static_cast<uint32_t>(free_pages_.size());
    for (uint32_t i = 0; i < header.free_list_count; ++i) {
        header.free_list[i] = free_pages_[i];
    }

    header.update_checksum();  // Compute and set checksum

    db_file_.seekp(0);
    db_file_.write(reinterpret_cast<const char*>(&header), sizeof(header));

    if (!db_file_.good()) {
        return Error(ErrorCode::IO_ERROR, "Failed to write file header");
    }

    return Ok();
}

Result<void> DiskManager::read_page(PageId page_id, char* data) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (page_id == 0 || page_id >= next_page_id_) {
        return Error(ErrorCode::INVALID_ARGUMENT, "Invalid page ID");
    }

    if (!data) {
        return Error(ErrorCode::INVALID_ARGUMENT, "Null data buffer");
    }

    uint64_t offset = get_file_offset(page_id);

    // Validate file is large enough (detect truncation/corruption)
    db_file_.seekg(0, std::ios::end);
    auto file_size = db_file_.tellg();
    if (file_size < 0 || static_cast<uint64_t>(file_size) < offset + PAGE_SIZE) {
        return Error(ErrorCode::CORRUPTION, "File too small for page ID");
    }

    db_file_.seekg(static_cast<std::streamoff>(offset));
    db_file_.read(data, PAGE_SIZE);

    if (!db_file_.good()) {
        return Error(ErrorCode::IO_ERROR, "Failed to read page");
    }

    return Ok();
}

Result<void> DiskManager::write_page(PageId page_id, const char* data) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (page_id == 0) {
        return Error(ErrorCode::INVALID_ARGUMENT, "Cannot write to header page");
    }

    if (!data) {
        return Error(ErrorCode::INVALID_ARGUMENT, "Null data buffer");
    }

    // Prevent sparse file creation - only allow writing to allocated pages
    // page_id must be < next_page_id_ (i.e., already allocated)
    if (page_id >= next_page_id_) {
        return Error(ErrorCode::INVALID_ARGUMENT,
                     "Cannot write to unallocated page (use allocate_page first)");
    }

    uint64_t offset = get_file_offset(page_id);
    db_file_.seekp(static_cast<std::streamoff>(offset));
    db_file_.write(data, PAGE_SIZE);

    if (!db_file_.good()) {
        return Error(ErrorCode::IO_ERROR, "Failed to write page");
    }

    if (page_id >= num_pages_) {
        num_pages_ = page_id + 1;
    }

    return Ok();
}

PageId DiskManager::allocate_page() {
    std::lock_guard<std::mutex> lock(mutex_);

    PageId page_id;
    bool from_free_list = false;

    if (!free_pages_.empty()) {
        page_id = free_pages_.back();
        free_pages_.pop_back();
        from_free_list = true;
    } else {
        page_id = next_page_id_++;
    }

    // Initialize the page with zeros
    char zero_page[PAGE_SIZE] = {0};
    uint64_t offset = get_file_offset(page_id);
    db_file_.seekp(static_cast<std::streamoff>(offset));
    db_file_.write(zero_page, PAGE_SIZE);

    // Check if write succeeded - if not, rollback allocation
    if (!db_file_.good()) {
        db_file_.clear();  // Clear error state for future operations
        if (from_free_list) {
            // Put page back on free list
            free_pages_.push_back(page_id);
        } else {
            // Decrement next_page_id back
            --next_page_id_;
        }
        return INVALID_PAGE_ID;
    }

    if (page_id >= num_pages_) {
        num_pages_ = page_id + 1;
    }

    return page_id;
}

void DiskManager::deallocate_page(PageId page_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (page_id != 0 && page_id < next_page_id_) {
        free_pages_.push_back(page_id);
    }
}

Result<void> DiskManager::flush() {
    std::lock_guard<std::mutex> lock(mutex_);

    db_file_.flush();
    if (!db_file_.good()) {
        return Error(ErrorCode::IO_ERROR, "Failed to flush database file");
    }

    return Ok();
}

uint64_t DiskManager::get_file_size() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::error_code ec;
    return static_cast<uint64_t>(fs::file_size(db_path_, ec));
}

Result<void> InMemoryDiskManager::read_page(PageId page_id, char* data) {
    std::lock_guard<std::mutex> lock(in_memory_mutex_);

    auto it = pages_.find(page_id);
    if (it == pages_.end()) {
        std::memset(data, 0, PAGE_SIZE);
        return Ok();
    }

    std::memcpy(data, it->second.data(), PAGE_SIZE);
    return Ok();
}

Result<void> InMemoryDiskManager::write_page(PageId page_id, const char* data) {
    std::lock_guard<std::mutex> lock(in_memory_mutex_);

    auto& page = pages_[page_id];
    std::memcpy(page.data(), data, PAGE_SIZE);
    return Ok();
}

PageId InMemoryDiskManager::allocate_page() {
    std::lock_guard<std::mutex> lock(in_memory_mutex_);

    PageId page_id;
    if (!free_pages_.empty()) {
        page_id = free_pages_.back();
        free_pages_.pop_back();
    } else {
        page_id = next_page_id_++;
    }

    pages_[page_id] = {};  // Initialize to zeros
    return page_id;
}

void InMemoryDiskManager::deallocate_page(PageId page_id) {
    std::lock_guard<std::mutex> lock(in_memory_mutex_);

    // Only deallocate if page exists (prevent double deallocation)
    if (pages_.erase(page_id) > 0) {
        free_pages_.push_back(page_id);
    }
}

}  // namespace dam
