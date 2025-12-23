#include <dam/storage/disk_manager.hpp>
#include <cstring>

namespace dam {

// File header structure (stored in page 0)
struct FileHeader {
    char magic[8] = {'D', 'O', 'C', 'S', 'T', 'O', 'R', 'E'};
    uint32_t version = 1;
    PageId num_pages = 1;  // Header is page 0
    PageId next_page_id = 1;
    uint32_t free_list_head = 0;  // Page ID of first free page, 0 if none
    uint8_t reserved[PAGE_SIZE - 24];
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

    num_pages_ = header.num_pages;
    next_page_id_ = header.next_page_id;

    // TODO: Load free list from header.free_list_head

    return Ok();
}

Result<void> DiskManager::write_header() {
    FileHeader header;
    header.num_pages = num_pages_;
    header.next_page_id = next_page_id_;
    // TODO: Save free list

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

    uint64_t offset = get_file_offset(page_id);
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

    if (!free_pages_.empty()) {
        page_id = free_pages_.back();
        free_pages_.pop_back();
    } else {
        page_id = next_page_id_++;
    }

    // Initialize the page with zeros
    char zero_page[PAGE_SIZE] = {0};
    uint64_t offset = get_file_offset(page_id);
    db_file_.seekp(static_cast<std::streamoff>(offset));
    db_file_.write(zero_page, PAGE_SIZE);

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

    pages_.erase(page_id);
    free_pages_.push_back(page_id);
}

}  // namespace dam
