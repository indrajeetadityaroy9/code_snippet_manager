#include <dam/index/tag_index.hpp>
#include <algorithm>
#include <cstring>

namespace dam {

TagIndex::TagIndex(BufferPool* buffer_pool, PageId root_page_id)
    : tree_(buffer_pool, root_page_id)
{}

std::string TagIndex::serialize_file_ids(const std::set<FileId>& ids) {
    std::string result;
    result.resize(ids.size() * sizeof(FileId));

    size_t offset = 0;
    for (FileId id : ids) {
        std::memcpy(result.data() + offset, &id, sizeof(FileId));
        offset += sizeof(FileId);
    }

    return result;
}

std::set<FileId> TagIndex::deserialize_file_ids(const std::string& data) {
    std::set<FileId> result;

    size_t count = data.size() / sizeof(FileId);
    for (size_t i = 0; i < count; ++i) {
        FileId id;
        std::memcpy(&id, data.data() + i * sizeof(FileId), sizeof(FileId));
        result.insert(id);
    }

    return result;
}

bool TagIndex::add_file_to_tag(const std::string& tag, FileId file_id) {
    auto existing = tree_.find(tag);

    std::set<FileId> ids;
    if (existing.has_value()) {
        ids = deserialize_file_ids(existing.value());
    }

    auto [_, inserted] = ids.insert(file_id);
    if (!inserted) {
        return false;  // Already in the set
    }

    std::string serialized = serialize_file_ids(ids);

    if (existing.has_value()) {
        return tree_.update(tag, serialized);
    } else {
        return tree_.insert(tag, serialized);
    }
}

bool TagIndex::remove_file_from_tag(const std::string& tag, FileId file_id) {
    auto existing = tree_.find(tag);
    if (!existing.has_value()) {
        return false;
    }

    std::set<FileId> ids = deserialize_file_ids(existing.value());
    size_t erased = ids.erase(file_id);

    if (erased == 0) {
        return false;  // Was not in the set
    }

    if (ids.empty()) {
        return tree_.remove(tag);
    }

    return tree_.update(tag, serialize_file_ids(ids));
}

void TagIndex::remove_file_from_all_tags(FileId file_id, const std::vector<std::string>& tags) {
    for (const auto& tag : tags) {
        remove_file_from_tag(tag, file_id);
    }
}

std::set<FileId> TagIndex::get_files_for_tag(const std::string& tag) const {
    auto data = tree_.find(tag);
    if (!data.has_value()) {
        return {};
    }
    return deserialize_file_ids(data.value());
}

std::set<FileId> TagIndex::get_files_for_all_tags(const std::vector<std::string>& tags) const {
    if (tags.empty()) {
        return {};
    }

    // Start with the first tag's files
    std::set<FileId> result = get_files_for_tag(tags[0]);

    // Intersect with each subsequent tag
    for (size_t i = 1; i < tags.size() && !result.empty(); ++i) {
        std::set<FileId> tag_files = get_files_for_tag(tags[i]);

        std::set<FileId> intersection;
        std::set_intersection(
            result.begin(), result.end(),
            tag_files.begin(), tag_files.end(),
            std::inserter(intersection, intersection.begin())
        );
        result = std::move(intersection);
    }

    return result;
}

std::set<FileId> TagIndex::get_files_for_any_tag(const std::vector<std::string>& tags) const {
    std::set<FileId> result;

    for (const auto& tag : tags) {
        std::set<FileId> tag_files = get_files_for_tag(tag);
        result.insert(tag_files.begin(), tag_files.end());
    }

    return result;
}

std::vector<std::string> TagIndex::get_tags_in_range(
    const std::string& start_tag,
    const std::string& end_tag) const
{
    std::vector<std::string> result;

    auto entries = tree_.range(start_tag, end_tag);
    for (const auto& entry : entries) {
        result.push_back(entry.first);
    }

    return result;
}

std::vector<std::string> TagIndex::get_all_tags() const {
    std::vector<std::string> result;

    tree_.for_each([&result](const std::string& tag, const std::string&) {
        result.push_back(tag);
        return true;
    });

    return result;
}

size_t TagIndex::get_tag_count(const std::string& tag) const {
    return get_files_for_tag(tag).size();
}

bool TagIndex::tag_exists(const std::string& tag) const {
    return tree_.contains(tag);
}

}  // namespace dam
