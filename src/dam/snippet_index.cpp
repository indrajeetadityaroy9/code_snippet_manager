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

    // Variable-size strings
    writer.write_string(s.name);
    writer.write_string(s.content);
    writer.write_string(s.language);
    writer.write_string(s.description);

    // Tags
    writer.write_uint32(static_cast<uint32_t>(s.tags.size()));
    for (const auto& tag : s.tags) {
        writer.write_string(tag);
    }

    return writer.data();
}

SnippetMetadata SnippetIndex::deserialize(const std::string& data) {
    SnippetMetadata s;
    BinaryReader reader(data);

    // Fixed-size fields
    reader.read_uint64(&s.id);
    reader.read_uint32(&s.checksum);

    uint64_t created_time, modified_time;
    reader.read_uint64(&created_time);
    reader.read_uint64(&modified_time);
    s.created_at = std::chrono::system_clock::time_point(
        std::chrono::system_clock::duration(static_cast<int64_t>(created_time)));
    s.modified_at = std::chrono::system_clock::time_point(
        std::chrono::system_clock::duration(static_cast<int64_t>(modified_time)));

    // Variable-size strings
    reader.read_string(&s.name);
    reader.read_string(&s.content);
    reader.read_string(&s.language);
    reader.read_string(&s.description);

    // Tags
    uint32_t tag_count = 0;
    reader.read_uint32(&tag_count);
    s.tags.reserve(tag_count);
    for (uint32_t i = 0; i < tag_count; ++i) {
        std::string tag;
        reader.read_string(&tag);
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

    if (!primary_tree_.insert(key, value)) {
        return INVALID_SNIPPET_ID;
    }

    // Add to name index
    name_tree_.insert(s.name, key);

    ++count_;
    return s.id;
}

bool SnippetIndex::update(SnippetId id, const SnippetMetadata& snippet) {
    std::string key = std::to_string(id);

    // Get old snippet for name update
    auto old_data = primary_tree_.find(key);
    if (old_data.has_value()) {
        SnippetMetadata old_snippet = deserialize(old_data.value());
        if (old_snippet.name != snippet.name) {
            name_tree_.remove(old_snippet.name);
            name_tree_.insert(snippet.name, key);
        }
    }

    return primary_tree_.update(key, serialize(snippet));
}

bool SnippetIndex::remove(SnippetId id) {
    std::string key = std::to_string(id);

    // Get snippet to remove from name index
    auto data = primary_tree_.find(key);
    if (data.has_value()) {
        SnippetMetadata s = deserialize(data.value());
        name_tree_.remove(s.name);
    }

    if (primary_tree_.remove(key)) {
        --count_;
        return true;
    }
    return false;
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

    return static_cast<SnippetId>(std::stoull(id_str.value()));
}

std::vector<SnippetMetadata> SnippetIndex::get_all() const {
    std::vector<SnippetMetadata> result;
    result.reserve(primary_tree_.size());

    primary_tree_.for_each([&result](const std::string&, const std::string& value) {
        result.push_back(deserialize(value));
        return true;
    });

    return result;
}

bool SnippetIndex::exists(SnippetId id) const {
    return primary_tree_.contains(std::to_string(id));
}

}  // namespace dam
