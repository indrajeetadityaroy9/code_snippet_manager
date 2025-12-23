#pragma once

#include <dam/core_types.hpp>
#include <dam/result.hpp>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace dam {

namespace fs = std::filesystem;

/**
 * DiskManager - Handles reading and writing pages to disk.
 *
 * Provides:
 * - Page-aligned I/O
 * - Page allocation and deallocation
 * - Database file management
 */
class DiskManager {
public:
    /**
     * Create a DiskManager for a database file.
     *
     * @param db_path Path to the database file
     */
    explicit DiskManager(const fs::path& db_path);

    // Prevent copying
    DiskManager(const DiskManager&) = delete;
    DiskManager& operator=(const DiskManager&) = delete;

    virtual ~DiskManager();

    /**
     * Read a page from disk into the provided buffer.
     *
     * @param page_id The page to read
     * @param data Buffer to read into (must be PAGE_SIZE bytes)
     * @return Result indicating success or failure
     */
    virtual Result<void> read_page(PageId page_id, char* data);

    /**
     * Write a page from the buffer to disk.
     *
     * @param page_id The page to write
     * @param data Buffer to write from (must be PAGE_SIZE bytes)
     * @return Result indicating success or failure
     */
    virtual Result<void> write_page(PageId page_id, const char* data);

    /**
     * Allocate a new page.
     *
     * @return The new page's ID, or INVALID_PAGE_ID on failure
     */
    virtual PageId allocate_page();

    /**
     * Deallocate a page (mark as free for reuse).
     *
     * @param page_id The page to deallocate
     */
    virtual void deallocate_page(PageId page_id);

    /**
     * Get the number of pages in the database file.
     */
    PageId get_num_pages() const { return num_pages_; }

    /**
     * Flush all pending writes to disk.
     */
    Result<void> flush();

    /**
     * Get the database file path.
     */
    const fs::path& get_db_path() const { return db_path_; }

    /**
     * Check if the database file exists and is valid.
     */
    bool is_valid() const { return is_open_; }

    /**
     * Get file size in bytes.
     */
    uint64_t get_file_size() const;

private:
    // Calculate file offset for a page
    uint64_t get_file_offset(PageId page_id) const {
        return static_cast<uint64_t>(page_id) * PAGE_SIZE;
    }

    // Open or create the database file
    Result<void> open_file();

    // Read the file header (first page) to get metadata
    Result<void> read_header();

    // Write the file header
    Result<void> write_header();

    fs::path db_path_;
    std::fstream db_file_;
    PageId num_pages_;
    PageId next_page_id_;
    std::vector<PageId> free_pages_;  // List of deallocated pages
    bool is_open_;
    mutable std::mutex mutex_;
};

/**
 * In-memory disk manager for testing.
 */
class InMemoryDiskManager : public DiskManager {
public:
    InMemoryDiskManager();

    Result<void> read_page(PageId page_id, char* data) override;
    Result<void> write_page(PageId page_id, const char* data) override;
    PageId allocate_page() override;
    void deallocate_page(PageId page_id) override;
    PageId get_num_pages() const { return static_cast<PageId>(pages_.size()); }

private:
    std::unordered_map<PageId, std::array<char, PAGE_SIZE>> pages_;
    PageId next_page_id_;
    std::vector<PageId> free_pages_;
    mutable std::mutex in_memory_mutex_;
};

}  // namespace dam
