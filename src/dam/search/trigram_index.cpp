#include <dam/search/trigram_index.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>

namespace dam::search {

// ============================================================================
// TrigramIndex Implementation
// ============================================================================

TrigramIndex::TrigramIndex(BufferPool* buffer_pool,
                           PageId root_page_id,
                           TrigramIndexConfig config)
    : tree_(buffer_pool, root_page_id)
    , config_(std::move(config)) {}

std::string TrigramIndex::normalize(const std::string& s) const {
    if (!config_.case_insensitive) {
        return s;
    }

    std::string result;
    result.reserve(s.size());
    for (char c : s) {
        result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return result;
}

std::set<std::string> TrigramIndex::extract_trigrams(const std::string& s,
                                                      bool use_padding) const {
    std::set<std::string> trigrams;

    std::string normalized = normalize(s);
    if (normalized.size() < 3) {
        // For very short strings, create padded trigrams
        if (use_padding && !normalized.empty()) {
            char pad = config_.padding_char;
            std::string padded;
            padded += pad;
            padded += pad;
            padded += normalized;
            padded += pad;
            padded += pad;

            for (size_t i = 0; i + 3 <= padded.size(); ++i) {
                trigrams.insert(padded.substr(i, 3));
            }
        }
        return trigrams;
    }

    if (use_padding && config_.use_padding) {
        // Add padding for prefix/suffix matching
        char pad = config_.padding_char;
        std::string padded;
        padded += pad;
        padded += pad;
        padded += normalized;
        padded += pad;
        padded += pad;

        for (size_t i = 0; i + 3 <= padded.size(); ++i) {
            trigrams.insert(padded.substr(i, 3));
        }
    } else {
        // No padding - just extract consecutive trigrams
        for (size_t i = 0; i + 3 <= normalized.size(); ++i) {
            trigrams.insert(normalized.substr(i, 3));
        }
    }

    return trigrams;
}

float TrigramIndex::jaccard_similarity(const std::set<std::string>& set_a,
                                        const std::set<std::string>& set_b) {
    if (set_a.empty() && set_b.empty()) {
        return 1.0f;
    }
    if (set_a.empty() || set_b.empty()) {
        return 0.0f;
    }

    // Count intersection
    size_t intersection_size = 0;
    for (const auto& tri : set_a) {
        if (set_b.count(tri) > 0) {
            intersection_size++;
        }
    }

    // Union size = |A| + |B| - |A ∩ B|
    size_t union_size = set_a.size() + set_b.size() - intersection_size;

    return static_cast<float>(intersection_size) / static_cast<float>(union_size);
}

float TrigramIndex::jaccard_similarity(const std::string& a, const std::string& b) const {
    auto set_a = extract_trigrams(a, true);
    auto set_b = extract_trigrams(b, true);
    return jaccard_similarity(set_a, set_b);
}

// ============================================================================
// Serialization
// ============================================================================

std::string TrigramIndex::serialize_doc_ids(const std::set<FileId>& ids) {
    std::string result;
    result.resize(ids.size() * sizeof(FileId));

    size_t offset = 0;
    for (FileId id : ids) {
        std::memcpy(result.data() + offset, &id, sizeof(FileId));
        offset += sizeof(FileId);
    }

    return result;
}

std::set<FileId> TrigramIndex::deserialize_doc_ids(const std::string& data) {
    std::set<FileId> result;

    size_t count = data.size() / sizeof(FileId);
    for (size_t i = 0; i < count; ++i) {
        FileId id;
        std::memcpy(&id, data.data() + i * sizeof(FileId), sizeof(FileId));
        result.insert(id);
    }

    return result;
}

// ============================================================================
// Indexing Operations
// ============================================================================

Result<void> TrigramIndex::add_to_trigram(const std::string& trigram, FileId doc_id) {
    auto existing = tree_.find(trigram);

    std::set<FileId> ids;
    if (existing.has_value()) {
        ids = deserialize_doc_ids(existing.value());
    }

    auto [_, inserted] = ids.insert(doc_id);
    if (inserted) {
        std::string serialized = serialize_doc_ids(ids);
        if (existing.has_value()) {
            if (!tree_.update(trigram, serialized)) {
                return Error(ErrorCode::IO_ERROR,
                    "Failed to update trigram: " + trigram);
            }
        } else {
            if (!tree_.insert(trigram, serialized)) {
                return Error(ErrorCode::IO_ERROR,
                    "Failed to insert trigram: " + trigram);
            }
        }
    }
    return {};
}

Result<void> TrigramIndex::remove_from_trigram(const std::string& trigram, FileId doc_id) {
    auto existing = tree_.find(trigram);
    if (!existing.has_value()) {
        return {};
    }

    std::set<FileId> ids = deserialize_doc_ids(existing.value());
    size_t erased = ids.erase(doc_id);

    if (erased > 0) {
        if (ids.empty()) {
            if (!tree_.remove(trigram)) {
                return Error(ErrorCode::IO_ERROR,
                    "Failed to remove trigram: " + trigram);
            }
        } else {
            if (!tree_.update(trigram, serialize_doc_ids(ids))) {
                return Error(ErrorCode::IO_ERROR,
                    "Failed to update trigram: " + trigram);
            }
        }
    }
    return {};
}

Result<void> TrigramIndex::index_document(FileId doc_id, const std::string& content) {
    auto trigrams = extract_trigrams(content, config_.use_padding);

    for (const auto& trigram : trigrams) {
        auto result = add_to_trigram(trigram, doc_id);
        if (!result.ok()) {
            return result;
        }
    }

    document_count_++;
    return {};
}

Result<void> TrigramIndex::index_text(FileId doc_id, const std::string& text,
                                       uint32_t /*field_id*/) {
    return index_document(doc_id, text);
}

Result<void> TrigramIndex::remove_document(FileId doc_id, const std::string& content) {
    auto trigrams = extract_trigrams(content, config_.use_padding);

    for (const auto& trigram : trigrams) {
        auto result = remove_from_trigram(trigram, doc_id);
        if (!result.ok()) {
            return result;
        }
    }

    if (document_count_ > 0) {
        document_count_--;
    }
    return {};
}

Result<void> TrigramIndex::update_document(FileId doc_id,
                                            const std::string& old_content,
                                            const std::string& new_content) {
    auto result = remove_document(doc_id, old_content);
    if (!result.ok()) {
        return result;
    }
    return index_document(doc_id, new_content);
}

// ============================================================================
// Search Operations
// ============================================================================

std::set<FileId> TrigramIndex::get_documents_for_trigram(const std::string& trigram) const {
    auto data = tree_.find(trigram);
    if (!data.has_value()) {
        return {};
    }
    return deserialize_doc_ids(data.value());
}

Result<std::vector<FileId>> TrigramIndex::search_substring(const std::string& pattern) const {
    if (pattern.empty()) {
        return std::vector<FileId>{};
    }

    // Extract trigrams from pattern (no padding for substring search)
    auto pattern_trigrams = extract_trigrams(pattern, false);

    if (pattern_trigrams.empty()) {
        // Pattern too short - fall back to checking all documents
        // In practice, you'd want to return all docs and filter externally
        return std::vector<FileId>{};
    }

    // Find documents that contain ALL pattern trigrams (intersection)
    std::set<FileId> candidates;
    bool first = true;

    for (const auto& trigram : pattern_trigrams) {
        auto docs = get_documents_for_trigram(trigram);

        if (first) {
            candidates = docs;
            first = false;
        } else {
            std::set<FileId> intersection;
            std::set_intersection(candidates.begin(), candidates.end(),
                                  docs.begin(), docs.end(),
                                  std::inserter(intersection, intersection.begin()));
            candidates = std::move(intersection);
        }

        if (candidates.empty()) {
            break;
        }
    }

    return std::vector<FileId>(candidates.begin(), candidates.end());
}

Result<std::vector<FuzzyResult>> TrigramIndex::search_fuzzy(
    const std::string& query,
    float threshold) const {

    if (query.empty()) {
        return std::vector<FuzzyResult>{};
    }

    if (threshold < 0.0f) {
        threshold = config_.default_similarity_threshold;
    }

    auto query_trigrams = extract_trigrams(query, true);
    if (query_trigrams.empty()) {
        return std::vector<FuzzyResult>{};
    }

    // Collect all potentially matching documents
    // Weight by trigram rarity (IDF-like)
    std::map<FileId, float> doc_scores;

    for (const auto& trigram : query_trigrams) {
        auto docs = get_documents_for_trigram(trigram);
        for (FileId doc_id : docs) {
            doc_scores[doc_id] += 1.0f;
        }
    }

    // Calculate similarity and filter by threshold
    std::vector<FuzzyResult> results;
    float query_size = static_cast<float>(query_trigrams.size());

    for (const auto& [doc_id, shared_count] : doc_scores) {
        // Estimate similarity: shared / query_size
        // This is a lower bound; actual Jaccard could be lower
        // For exact Jaccard, we'd need to store doc trigram counts
        float min_similarity = shared_count / query_size;

        // Only include if potentially above threshold
        if (min_similarity >= threshold * 0.5f) {
            FuzzyResult result;
            result.doc_id = doc_id;
            result.similarity = min_similarity;
            results.push_back(result);
        }
    }

    // Sort by similarity descending
    std::sort(results.begin(), results.end());

    // Limit results
    if (results.size() > config_.max_results) {
        results.resize(config_.max_results);
    }

    return results;
}

Result<std::vector<FuzzyResult>> TrigramIndex::find_similar(
    const std::string& query,
    size_t max_results) const {

    auto fuzzy_result = search_fuzzy(query, 0.1f);  // Low threshold to get more candidates
    if (!fuzzy_result.ok()) {
        return fuzzy_result;
    }

    auto results = fuzzy_result.value();
    if (results.size() > max_results) {
        results.resize(max_results);
    }

    return results;
}

bool TrigramIndex::may_contain(FileId doc_id, const std::string& pattern) const {
    auto pattern_trigrams = extract_trigrams(pattern, false);

    for (const auto& trigram : pattern_trigrams) {
        auto docs = get_documents_for_trigram(trigram);
        if (docs.find(doc_id) == docs.end()) {
            return false;  // Missing a required trigram
        }
    }

    return true;
}

// ============================================================================
// Statistics
// ============================================================================

size_t TrigramIndex::get_trigram_frequency(const std::string& trigram) const {
    auto docs = get_documents_for_trigram(trigram);
    return docs.size();
}

std::vector<std::string> TrigramIndex::get_all_trigrams() const {
    std::vector<std::string> trigrams;

    tree_.for_each([&trigrams](const std::string& trigram, const std::string&) {
        trigrams.push_back(trigram);
        return true;
    });

    return trigrams;
}

}  // namespace dam::search
