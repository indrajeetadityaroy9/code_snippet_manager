#include <dam/snippet_store.hpp>
#include <dam/util/crc32.hpp>

#include <algorithm>
#include <fstream>
#include <iostream>

namespace dam {

namespace {

// Metadata file structure:
// - uint32: magic number (0xDAD01234)
// - uint32: snippet_index primary root
// - uint32: snippet_index name root
// - uint32: tag_index root
// - uint64: next_id
constexpr uint32_t METADATA_MAGIC = 0xDAD01234;

struct StoreMetadata {
    uint32_t magic = METADATA_MAGIC;
    PageId snippet_primary_root = INVALID_PAGE_ID;
    PageId snippet_name_root = INVALID_PAGE_ID;
    PageId tag_root = INVALID_PAGE_ID;
    uint64_t next_id = 1;
    uint64_t snippet_count = 0;
};

bool load_metadata(const fs::path& path, StoreMetadata& meta) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;

    file.read(reinterpret_cast<char*>(&meta), sizeof(meta));
    return file.good() && meta.magic == METADATA_MAGIC;
}

bool save_metadata(const fs::path& path, const StoreMetadata& meta) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;

    file.write(reinterpret_cast<const char*>(&meta), sizeof(meta));
    return file.good();
}

}  // namespace

SnippetStore::~SnippetStore() {
    close();
}

Result<std::unique_ptr<SnippetStore>> SnippetStore::open(const Config& config) {
    auto store = std::unique_ptr<SnippetStore>(new SnippetStore());

    // Create root directory
    std::error_code ec;
    fs::create_directories(config.root_directory, ec);
    if (ec) {
        return Error(ErrorCode::IO_ERROR,
                               "Failed to create root directory: " + ec.message());
    }

    store->root_dir_ = config.root_directory;
    fs::path db_path = config.root_directory / "dam.db";
    fs::path meta_path = config.root_directory / "dam.meta";

    // Initialize disk manager
    store->disk_manager_ = std::make_unique<DiskManager>(db_path);
    if (!store->disk_manager_->is_valid()) {
        return Error(ErrorCode::IO_ERROR,
                               "Failed to open database file");
    }

    // Initialize buffer pool
    store->buffer_pool_ = std::make_unique<BufferPool>(
        config.buffer_pool_size, store->disk_manager_.get());

    // Load metadata (if exists)
    StoreMetadata meta;
    load_metadata(meta_path, meta);

    // Initialize indexes with persisted root page IDs
    store->snippet_index_ = std::make_unique<SnippetIndex>(
        store->buffer_pool_.get(),
        meta.snippet_primary_root,
        meta.snippet_name_root);

    // Reset next_id to 1 if store is empty (allows ID reuse after all snippets removed)
    if (meta.snippet_count == 0) {
        store->snippet_index_->set_next_id(1);
        store->snippet_index_->set_count(0);
    } else {
        store->snippet_index_->set_next_id(meta.next_id);
        store->snippet_index_->set_count(static_cast<size_t>(meta.snippet_count));
    }

    store->tag_index_ = std::make_unique<TagIndex>(
        store->buffer_pool_.get(),
        meta.tag_root);

    store->is_open_ = true;

    if (config.verbose) {
        std::cout << "DAM store opened at: " << config.root_directory << std::endl;
    }

    return store;
}

void SnippetStore::close() {
    if (!is_open_) return;

    // Save metadata before closing
    fs::path meta_path = root_dir_ / "dam.meta";
    StoreMetadata meta;
    meta.snippet_primary_root = snippet_index_->get_primary_root_id();
    meta.snippet_name_root = snippet_index_->get_name_root_id();
    meta.tag_root = tag_index_->get_root_page_id();
    meta.next_id = snippet_index_->get_next_id();
    meta.snippet_count = static_cast<uint64_t>(snippet_index_->size());
    save_metadata(meta_path, meta);

    // Flush buffer pool
    if (buffer_pool_) {
        buffer_pool_->flush_all_pages();
    }

    // Release resources
    tag_index_.reset();
    snippet_index_.reset();
    buffer_pool_.reset();
    disk_manager_.reset();

    is_open_ = false;
}

Result<SnippetId> SnippetStore::add(const std::string& content,
                                              const std::string& name,
                                              const std::vector<std::string>& tags,
                                              const std::string& language,
                                              const std::string& description) {
    if (!is_open_) {
        return Error(ErrorCode::INVALID_ARGUMENT,
                               "Store is not open");
    }

    if (name.empty()) {
        return Error(ErrorCode::INVALID_ARGUMENT,
                               "Snippet name cannot be empty");
    }

    // Check for duplicate name
    if (snippet_index_->find_by_name(name).has_value()) {
        return Error(ErrorCode::ALREADY_EXISTS,
                               "Snippet with name '" + name + "' already exists");
    }

    // Create snippet metadata
    SnippetMetadata snippet;
    snippet.name = name;
    snippet.content = content;
    snippet.description = description;
    snippet.tags = tags;
    snippet.created_at = std::chrono::system_clock::now();
    snippet.modified_at = snippet.created_at;
    snippet.checksum = CRC32::compute(content);

    // Auto-detect language if not provided
    if (language.empty()) {
        snippet.language = LanguageDetector::detect(content, name);
    } else {
        snippet.language = language;
    }

    // Insert into index
    SnippetId id = snippet_index_->insert(snippet);
    if (id == INVALID_SNIPPET_ID) {
        return Error(ErrorCode::INTERNAL_ERROR,
                               "Failed to insert snippet");
    }

    // Add to tag index - track which tags were added for rollback
    std::vector<std::string> added_tags;
    bool tag_failed = false;
    for (const auto& tag : tags) {
        if (!tag_index_->add_file_to_tag(tag, id)) {
            tag_failed = true;
            break;
        }
        added_tags.push_back(tag);
    }

    // Rollback on tag failure
    if (tag_failed) {
        // Best-effort rollback: attempt to remove all tags that were added
        // Continue even if individual removals fail to clean up as much as possible
        bool rollback_failed = false;
        for (const auto& tag : added_tags) {
            if (!tag_index_->remove_file_from_tag(tag, id)) {
                rollback_failed = true;
            }
        }
        // Remove snippet from index
        if (!snippet_index_->remove(id)) {
            rollback_failed = true;
        }

        if (rollback_failed) {
            return Error(ErrorCode::INTERNAL_ERROR,
                                   "Failed to add tags and rollback was incomplete");
        }
        return Error(ErrorCode::INTERNAL_ERROR,
                               "Failed to add tags to snippet");
    }

    return id;
}

std::optional<SnippetMetadata> SnippetStore::get(SnippetId id) const {
    if (!is_open_) return std::nullopt;
    return snippet_index_->get(id);
}

std::optional<SnippetId> SnippetStore::find_by_name(const std::string& name) const {
    if (!is_open_) return std::nullopt;
    return snippet_index_->find_by_name(name);
}

Result<void> SnippetStore::remove(SnippetId id) {
    if (!is_open_) {
        return Error(ErrorCode::INVALID_ARGUMENT,
                               "Store is not open");
    }

    auto snippet = snippet_index_->get(id);
    if (!snippet.has_value()) {
        return Error(ErrorCode::NOT_FOUND,
                               "Snippet not found");
    }

    // Remove from tag index (best effort - log failures but continue)
    if (!tag_index_->remove_file_from_all_tags(id, snippet->tags)) {
        // Tag removal partially failed, but we still proceed with snippet removal
        // This could leave orphaned tag entries, but is preferable to failing entirely
    }

    // Remove from snippet index
    if (!snippet_index_->remove(id)) {
        return Error(ErrorCode::INTERNAL_ERROR,
                               "Failed to remove snippet");
    }

    return Ok();
}

Result<void> SnippetStore::add_tag(SnippetId id, const std::string& tag) {
    if (!is_open_) {
        return Error(ErrorCode::INVALID_ARGUMENT,
                               "Store is not open");
    }

    auto snippet = snippet_index_->get(id);
    if (!snippet.has_value()) {
        return Error(ErrorCode::NOT_FOUND,
                               "Snippet not found");
    }

    // Check if tag already exists
    auto& tags = snippet->tags;
    if (std::find(tags.begin(), tags.end(), tag) != tags.end()) {
        return Ok();  // Already has tag
    }

    // Add to tag index first (easier to rollback if snippet update fails)
    if (!tag_index_->add_file_to_tag(tag, id)) {
        return Error(ErrorCode::INTERNAL_ERROR,
                               "Failed to add tag to index");
    }

    // Add tag to snippet metadata
    snippet->tags.push_back(tag);
    snippet->modified_at = std::chrono::system_clock::now();

    if (!snippet_index_->update(id, *snippet)) {
        // Rollback: remove from tag index
        if (!tag_index_->remove_file_from_tag(tag, id)) {
            return Error(ErrorCode::INTERNAL_ERROR,
                                   "Failed to update snippet and rollback was incomplete");
        }
        return Error(ErrorCode::INTERNAL_ERROR,
                               "Failed to update snippet");
    }

    return Ok();
}

Result<void> SnippetStore::remove_tag(SnippetId id, const std::string& tag) {
    if (!is_open_) {
        return Error(ErrorCode::INVALID_ARGUMENT,
                               "Store is not open");
    }

    auto snippet = snippet_index_->get(id);
    if (!snippet.has_value()) {
        return Error(ErrorCode::NOT_FOUND,
                               "Snippet not found");
    }

    // Find and remove tag from snippet metadata
    auto& tags = snippet->tags;
    auto it = std::find(tags.begin(), tags.end(), tag);
    if (it == tags.end()) {
        return Ok();  // Tag not present
    }

    // Remove from tag index first - must succeed before modifying metadata
    if (!tag_index_->remove_file_from_tag(tag, id)) {
        // Tag wasn't in index - this is an inconsistency but we can still
        // remove it from the snippet metadata to restore consistency
    }

    tags.erase(it);
    snippet->modified_at = std::chrono::system_clock::now();

    if (!snippet_index_->update(id, *snippet)) {
        // Rollback: re-add to tag index
        if (!tag_index_->add_file_to_tag(tag, id)) {
            return Error(ErrorCode::INTERNAL_ERROR,
                                   "Failed to update snippet and rollback was incomplete");
        }
        return Error(ErrorCode::INTERNAL_ERROR,
                               "Failed to update snippet");
    }

    return Ok();
}

std::vector<SnippetMetadata> SnippetStore::list_all() const {
    if (!is_open_) return {};
    return snippet_index_->get_all();
}

std::vector<SnippetMetadata> SnippetStore::find_by_tag(const std::string& tag) const {
    if (!is_open_) return {};

    std::set<FileId> ids = tag_index_->get_files_for_tag(tag);
    std::vector<SnippetMetadata> result;
    result.reserve(ids.size());

    for (auto id : ids) {
        auto snippet = snippet_index_->get(id);
        if (snippet.has_value()) {
            result.push_back(std::move(*snippet));
        }
    }

    return result;
}

std::vector<SnippetMetadata> SnippetStore::find_by_language(const std::string& language) const {
    if (!is_open_) return {};

    std::vector<SnippetMetadata> result;
    auto all = snippet_index_->get_all();

    for (auto& snippet : all) {
        if (snippet.language == language) {
            result.push_back(std::move(snippet));
        }
    }

    return result;
}

std::vector<std::string> SnippetStore::get_all_tags() const {
    if (!is_open_) return {};
    return tag_index_->get_all_tags();
}

std::map<std::string, size_t> SnippetStore::get_tag_counts() const {
    if (!is_open_) return {};

    std::map<std::string, size_t> result;
    auto tags = tag_index_->get_all_tags();

    for (const auto& tag : tags) {
        result[tag] = tag_index_->get_tag_count(tag);
    }

    return result;
}

size_t SnippetStore::count() const {
    if (!is_open_) return 0;
    return snippet_index_->size();
}

}  // namespace dam
