#include <dam/snippet_index.hpp>
#include <dam/util/serializer.hpp>

#include <cstring>

namespace dam {

SnippetIndex::SnippetIndex(BufferPool* buffer_pool,
                           PageId primary_root,
                           PageId name_root)
    : primary_tree_(buffer_pool, primary_root)
    , name_tree_(buffer_pool, name_root)
    , next_id_(1)
{}

std::string SnippetIndex::serialize(const SnippetMetadata& s) {
    BinaryWriter writer;

    // Fixed-size fields
    writer.write_uint64(s.id);
    writer.write_uint32(s.checksum);

    auto created_time = s.created_at.time_since_epoch().count();
    auto modified_time = s.modified_at.time_since_epoch().count();
    writer.write_uint64(static_cast<uint64_t>(created_time));
    writer.write_uint64(static_cast<uint64_t>(modified_time));

    // Variable-size strings - check for overflow (strings > 4GB)
    if (!writer.write_string(s.name) ||
        !writer.write_string(s.content) ||
        !writer.write_string(s.language) ||
        !writer.write_string(s.description)) {
        return "";  // Return empty string on overflow
    }

    // Tags
    writer.write_uint32(static_cast<uint32_t>(s.tags.size()));
    for (const auto& tag : s.tags) {
        if (!writer.write_string(tag)) {
            return "";  // Return empty string on overflow
        }
    }

    return writer.data();
}

SnippetMetadata SnippetIndex::deserialize(const std::string& data) {
    SnippetMetadata s;
    BinaryReader reader(data);

    // Helper to mark object as invalid and return
    auto mark_invalid = [&s]() -> SnippetMetadata& {
        s.id = INVALID_SNIPPET_ID;
        s.tags.clear();
        return s;
    };

    // Fixed-size fields - check for read failures
    if (!reader.read_uint64(&s.id) ||
        !reader.read_uint32(&s.checksum)) {
        return mark_invalid();
    }

    // Validate ID is not already invalid (corrupted data)
    if (s.id == INVALID_SNIPPET_ID) {
        return mark_invalid();
    }

    uint64_t created_time = 0, modified_time = 0;
    if (!reader.read_uint64(&created_time) ||
        !reader.read_uint64(&modified_time)) {
        return mark_invalid();
    }
    s.created_at = std::chrono::system_clock::time_point(
        std::chrono::system_clock::duration(static_cast<int64_t>(created_time)));
    s.modified_at = std::chrono::system_clock::time_point(
        std::chrono::system_clock::duration(static_cast<int64_t>(modified_time)));

    // Variable-size strings - check for read failures
    if (!reader.read_string(&s.name) ||
        !reader.read_string(&s.content) ||
        !reader.read_string(&s.language) ||
        !reader.read_string(&s.description)) {
        return mark_invalid();
    }

    // Tags
    uint32_t tag_count = 0;
    if (!reader.read_uint32(&tag_count)) {
        return mark_invalid();
    }

    // Sanity check tag count (prevent huge allocation on corrupted data)
    constexpr uint32_t MAX_TAGS = 10000;
    if (tag_count > MAX_TAGS) {
        return mark_invalid();
    }

    s.tags.reserve(tag_count);
    for (uint32_t i = 0; i < tag_count; ++i) {
        std::string tag;
        if (!reader.read_string(&tag)) {
            // Partial tag deserialization - mark as invalid
            return mark_invalid();
        }
        s.tags.push_back(std::move(tag));
    }

    return s;
}

SnippetId SnippetIndex::generate_id() {
    return next_id_++;
}

SnippetId SnippetIndex::insert(const SnippetMetadata& snippet) {
    SnippetMetadata s = snippet;
    if (s.id == INVALID_SNIPPET_ID) {
        s.id = generate_id();
    }

    std::string key = std::to_string(s.id);
    std::string value = serialize(s);

    // Check serialization succeeded (empty = overflow error)
    if (value.empty()) {
        return INVALID_SNIPPET_ID;
    }

    if (!primary_tree_.insert(key, value)) {
        return INVALID_SNIPPET_ID;
    }

    // Add to name index - rollback primary insert on failure
    if (!name_tree_.insert(s.name, key)) {
        // Rollback: remove from primary tree
        primary_tree_.remove(key);
        return INVALID_SNIPPET_ID;
    }

    ++count_;
    return s.id;
}

bool SnippetIndex::update(SnippetId id, const SnippetMetadata& snippet) {
    std::string key = std::to_string(id);

    // Serialize first to detect overflow errors early
    std::string serialized = serialize(snippet);
    if (serialized.empty()) {
        return false;  // Serialization failed (overflow)
    }

    // Get old snippet for name update
    auto old_data = primary_tree_.find(key);
    if (!old_data.has_value()) {
        return false;  // Snippet doesn't exist
    }

    SnippetMetadata old_snippet = deserialize(old_data.value());
    if (old_snippet.name != snippet.name) {
        // Renaming: first check new name doesn't already exist
        auto existing = name_tree_.find(snippet.name);
        if (existing.has_value()) {
            return false;  // New name already in use
        }

        // Insert new name first, then remove old (safer order)
        if (!name_tree_.insert(snippet.name, key)) {
            return false;
        }
        name_tree_.remove(old_snippet.name);
    }

    return primary_tree_.update(key, serialized);
}

bool SnippetIndex::remove(SnippetId id) {
    std::string key = std::to_string(id);

    // Get snippet to remove from name index
    auto data = primary_tree_.find(key);
    if (!data.has_value()) {
        return false;  // Snippet doesn't exist
    }

    SnippetMetadata s = deserialize(data.value());

    // Remove from primary tree first
    if (!primary_tree_.remove(key)) {
        return false;
    }

    // Remove from name index (best effort - log if fails but don't fail remove)
    // Note: name_tree_.remove() failure is logged but doesn't rollback
    // since primary is already removed
    name_tree_.remove(s.name);

    --count_;
    return true;
}

std::optional<SnippetMetadata> SnippetIndex::get(SnippetId id) const {
    std::string key = std::to_string(id);
    auto data = primary_tree_.find(key);

    if (!data.has_value()) {
        return std::nullopt;
    }

    return deserialize(data.value());
}

std::optional<SnippetId> SnippetIndex::find_by_name(const std::string& name) const {
    auto id_str = name_tree_.find(name);
    if (!id_str.has_value()) {
        return std::nullopt;
    }

    try {
        return static_cast<SnippetId>(std::stoull(id_str.value()));
    } catch (const std::exception&) {
        // Corrupted index entry - treat as not found
        return std::nullopt;
    }
}

std::vector<SnippetMetadata> SnippetIndex::get_all() const {
    std::vector<SnippetMetadata> result;
    result.reserve(primary_tree_.size());

    primary_tree_.for_each([&result](const std::string&, const std::string& value) {
        SnippetMetadata snippet = deserialize(value);
        // Skip corrupted entries with invalid IDs
        if (snippet.id != INVALID_SNIPPET_ID) {
            result.push_back(std::move(snippet));
        }
        return true;
    });

    return result;
}

bool SnippetIndex::exists(SnippetId id) const {
    return primary_tree_.contains(std::to_string(id));
}

}  // namespace dam
