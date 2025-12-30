#include <dam/search/inverted_index.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <regex>
#include <sstream>

namespace dam::search {

// ============================================================================
// PostingList Implementation
// ============================================================================

const Posting* PostingList::find(FileId doc_id) const {
    // Binary search since postings are sorted by doc_id
    auto it = std::lower_bound(postings.begin(), postings.end(), doc_id,
        [](const Posting& p, FileId id) { return p.doc_id < id; });

    if (it != postings.end() && it->doc_id == doc_id) {
        return &(*it);
    }
    return nullptr;
}

void PostingList::add_posting(FileId doc_id, const std::vector<uint32_t>& positions) {
    auto it = std::lower_bound(postings.begin(), postings.end(), doc_id,
        [](const Posting& p, FileId id) { return p.doc_id < id; });

    if (it != postings.end() && it->doc_id == doc_id) {
        // Update existing posting
        it->frequency = static_cast<uint32_t>(positions.size());
        it->positions = positions;
    } else {
        // Insert new posting
        Posting p;
        p.doc_id = doc_id;
        p.frequency = static_cast<uint32_t>(positions.size());
        p.positions = positions;
        postings.insert(it, p);
        document_frequency++;
    }
}

bool PostingList::remove_posting(FileId doc_id) {
    auto it = std::lower_bound(postings.begin(), postings.end(), doc_id,
        [](const Posting& p, FileId id) { return p.doc_id < id; });

    if (it != postings.end() && it->doc_id == doc_id) {
        postings.erase(it);
        document_frequency--;
        return true;
    }
    return false;
}

// ============================================================================
// Serialization Format
// ============================================================================

// Format:
// [4 bytes: term length]
// [N bytes: term]
// [4 bytes: document_frequency]
// [4 bytes: number of postings]
// For each posting:
//   [8 bytes: doc_id (FileId)]
//   [4 bytes: frequency]
//   [4 bytes: number of positions]
//   [M * 4 bytes: positions]

std::string InvertedIndex::serialize_posting_list(const PostingList& list) {
    std::string result;

    // Calculate size
    size_t size = 4 + list.term.size() + 4 + 4;  // term + df + posting count
    for (const auto& p : list.postings) {
        size += 8 + 4 + 4 + p.positions.size() * 4;
    }
    result.reserve(size);

    // Term length and term
    uint32_t term_len = static_cast<uint32_t>(list.term.size());
    result.append(reinterpret_cast<const char*>(&term_len), 4);
    result.append(list.term);

    // Document frequency
    result.append(reinterpret_cast<const char*>(&list.document_frequency), 4);

    // Number of postings
    uint32_t posting_count = static_cast<uint32_t>(list.postings.size());
    result.append(reinterpret_cast<const char*>(&posting_count), 4);

    // Each posting
    for (const auto& p : list.postings) {
        result.append(reinterpret_cast<const char*>(&p.doc_id), 8);
        result.append(reinterpret_cast<const char*>(&p.frequency), 4);

        uint32_t pos_count = static_cast<uint32_t>(p.positions.size());
        result.append(reinterpret_cast<const char*>(&pos_count), 4);

        for (uint32_t pos : p.positions) {
            result.append(reinterpret_cast<const char*>(&pos), 4);
        }
    }

    return result;
}

PostingList InvertedIndex::deserialize_posting_list(const std::string& data) {
    PostingList list;
    const char* ptr = data.data();
    const char* end = ptr + data.size();

    if (ptr + 4 > end) return list;

    // Term length and term
    uint32_t term_len;
    std::memcpy(&term_len, ptr, 4);
    ptr += 4;

    if (ptr + term_len > end) return list;
    list.term.assign(ptr, term_len);
    ptr += term_len;

    if (ptr + 8 > end) return list;

    // Document frequency
    std::memcpy(&list.document_frequency, ptr, 4);
    ptr += 4;

    // Number of postings
    uint32_t posting_count;
    std::memcpy(&posting_count, ptr, 4);
    ptr += 4;

    list.postings.reserve(posting_count);

    // Each posting
    for (uint32_t i = 0; i < posting_count && ptr + 16 <= end; ++i) {
        Posting p;

        std::memcpy(&p.doc_id, ptr, 8);
        ptr += 8;

        std::memcpy(&p.frequency, ptr, 4);
        ptr += 4;

        uint32_t pos_count;
        std::memcpy(&pos_count, ptr, 4);
        ptr += 4;

        if (ptr + pos_count * 4 > end) break;

        p.positions.reserve(pos_count);
        for (uint32_t j = 0; j < pos_count; ++j) {
            uint32_t pos;
            std::memcpy(&pos, ptr, 4);
            ptr += 4;
            p.positions.push_back(pos);
        }

        list.postings.push_back(std::move(p));
    }

    return list;
}

// ============================================================================
// InvertedIndex Implementation
// ============================================================================

InvertedIndex::InvertedIndex(BufferPool* buffer_pool,
                             PageId root_page_id,
                             InvertedIndexConfig config)
    : tree_(buffer_pool, root_page_id)
    , tokenizer_(config.tokenizer)
    , config_(std::move(config)) {}

Result<void> InvertedIndex::index_document(FileId doc_id, const std::string& content) {
    // Tokenize with positions
    auto tokens = tokenizer_.tokenize_with_positions(content);

    if (tokens.empty()) {
        return {};  // Nothing to index
    }

    // Group tokens by term
    std::map<std::string, std::vector<uint32_t>> term_positions;
    for (const auto& [token, position] : tokens) {
        term_positions[token].push_back(position);
    }

    // Update posting lists
    for (const auto& [term, positions] : term_positions) {
        auto existing = tree_.find(term);

        PostingList list;
        if (existing.has_value()) {
            list = deserialize_posting_list(existing.value());
        } else {
            list.term = term;
            list.document_frequency = 0;
        }

        list.add_posting(doc_id, positions);

        std::string serialized = serialize_posting_list(list);

        if (existing.has_value()) {
            if (!tree_.update(term, serialized)) {
                return Error(ErrorCode::INTERNAL_ERROR,
                    "Failed to update posting list for term: " + term);
            }
        } else {
            if (!tree_.insert(term, serialized)) {
                return Error(ErrorCode::INTERNAL_ERROR,
                    "Failed to insert posting list for term: " + term);
            }
        }
    }

    // Update statistics
    document_count_++;
    uint32_t doc_length = static_cast<uint32_t>(tokens.size());
    total_document_length_ += doc_length;
    doc_lengths_[doc_id] = doc_length;

    return {};
}

Result<void> InvertedIndex::index_code(FileId doc_id, const std::string& code) {
    // Use code-aware tokenization
    auto tokens = tokenizer_.tokenize_code(code);

    if (tokens.empty()) {
        return {};
    }

    // For code, we track positions sequentially
    std::map<std::string, std::vector<uint32_t>> term_positions;
    uint32_t position = 0;
    for (const auto& token : tokens) {
        term_positions[token].push_back(position++);
    }

    // Update posting lists (same as text)
    for (const auto& [term, positions] : term_positions) {
        auto existing = tree_.find(term);

        PostingList list;
        if (existing.has_value()) {
            list = deserialize_posting_list(existing.value());
        } else {
            list.term = term;
            list.document_frequency = 0;
        }

        list.add_posting(doc_id, positions);

        std::string serialized = serialize_posting_list(list);

        if (existing.has_value()) {
            if (!tree_.update(term, serialized)) {
                return Error(ErrorCode::IO_ERROR,
                    "Failed to update posting list for term: " + term);
            }
        } else {
            if (!tree_.insert(term, serialized)) {
                return Error(ErrorCode::IO_ERROR,
                    "Failed to insert posting list for term: " + term);
            }
        }
    }

    document_count_++;
    uint32_t doc_length = static_cast<uint32_t>(tokens.size());
    total_document_length_ += doc_length;
    doc_lengths_[doc_id] = doc_length;

    return {};
}

Result<void> InvertedIndex::remove_document(FileId doc_id, const std::string& content) {
    // Get unique terms from content
    auto terms = tokenizer_.unique_terms(content);

    for (const auto& term : terms) {
        auto existing = tree_.find(term);
        if (!existing.has_value()) {
            continue;
        }

        PostingList list = deserialize_posting_list(existing.value());

        if (list.remove_posting(doc_id)) {
            if (list.postings.empty()) {
                if (!tree_.remove(term)) {
                    return Error(ErrorCode::IO_ERROR,
                        "Failed to remove posting list for term: " + term);
                }
            } else {
                if (!tree_.update(term, serialize_posting_list(list))) {
                    return Error(ErrorCode::IO_ERROR,
                        "Failed to update posting list for term: " + term);
                }
            }
        }
    }

    // Update statistics
    auto it = doc_lengths_.find(doc_id);
    if (it != doc_lengths_.end()) {
        total_document_length_ -= it->second;
        doc_lengths_.erase(it);
        document_count_--;
    }

    return {};
}

Result<void> InvertedIndex::update_document(FileId doc_id,
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

std::string InvertedIndex::normalize_term(const std::string& term) const {
    auto tokens = tokenizer_.tokenize(term);
    if (tokens.empty()) {
        return "";
    }
    return tokens[0];
}

Result<std::vector<SearchResult>> InvertedIndex::search_term(const std::string& term) const {
    std::string normalized = normalize_term(term);
    if (normalized.empty()) {
        return std::vector<SearchResult>{};
    }

    auto posting_opt = get_posting_list(normalized);
    if (!posting_opt.has_value()) {
        return std::vector<SearchResult>{};
    }

    const PostingList& list = posting_opt.value();
    std::vector<SearchResult> results;
    results.reserve(list.postings.size());

    for (const auto& posting : list.postings) {
        SearchResult result;
        result.doc_id = posting.doc_id;

        // Get document length
        uint32_t doc_length = 0;
        auto len_it = doc_lengths_.find(posting.doc_id);
        if (len_it != doc_lengths_.end()) {
            doc_length = len_it->second;
        }

        result.score = calculate_bm25(posting.frequency, list.document_frequency, doc_length);
        result.positions = posting.positions;

        results.push_back(result);
    }

    // Sort by score descending
    std::sort(results.begin(), results.end());

    // Limit results
    if (results.size() > config_.max_results) {
        results.resize(config_.max_results);
    }

    return results;
}

Result<std::vector<SearchResult>> InvertedIndex::search_and(
    const std::vector<std::string>& terms) const {

    if (terms.empty()) {
        return std::vector<SearchResult>{};
    }

    // Get posting lists for all terms
    std::vector<PostingList> lists;
    for (const auto& term : terms) {
        std::string normalized = normalize_term(term);
        if (normalized.empty()) continue;

        auto posting_opt = get_posting_list(normalized);
        if (!posting_opt.has_value()) {
            // Term not found, AND query returns empty
            return std::vector<SearchResult>{};
        }
        lists.push_back(posting_opt.value());
    }

    if (lists.empty()) {
        return std::vector<SearchResult>{};
    }

    return merge_and_results(lists);
}

Result<std::vector<SearchResult>> InvertedIndex::search_or(
    const std::vector<std::string>& terms) const {

    if (terms.empty()) {
        return std::vector<SearchResult>{};
    }

    std::vector<PostingList> lists;
    for (const auto& term : terms) {
        std::string normalized = normalize_term(term);
        if (normalized.empty()) continue;

        auto posting_opt = get_posting_list(normalized);
        if (posting_opt.has_value()) {
            lists.push_back(posting_opt.value());
        }
    }

    if (lists.empty()) {
        return std::vector<SearchResult>{};
    }

    return merge_or_results(lists);
}

Result<std::vector<SearchResult>> InvertedIndex::search_phrase(
    const std::string& phrase) const {

    if (!config_.store_positions) {
        return Error(ErrorCode::INVALID_ARGUMENT,
            "Phrase search requires position storage");
    }

    // Tokenize phrase
    auto tokens = tokenizer_.tokenize(phrase);
    if (tokens.size() < 2) {
        // Single word, treat as term search
        if (tokens.size() == 1) {
            return search_term(tokens[0]);
        }
        return std::vector<SearchResult>{};
    }

    // Get posting lists for all terms
    std::vector<PostingList> lists;
    for (const auto& token : tokens) {
        auto posting_opt = get_posting_list(token);
        if (!posting_opt.has_value()) {
            return std::vector<SearchResult>{};
        }
        lists.push_back(posting_opt.value());
    }

    // Find documents containing all terms
    auto and_results = merge_and_results(lists);

    // Filter to only those with phrase match
    std::vector<SearchResult> phrase_results;
    for (const auto& result : and_results) {
        if (check_phrase_match(lists, result.doc_id)) {
            phrase_results.push_back(result);
        }
    }

    return phrase_results;
}

Result<std::vector<SearchResult>> InvertedIndex::search(const std::string& query) const {
    auto components = parse_query(query);

    if (components.empty()) {
        return std::vector<SearchResult>{};
    }

    // Separate into required, optional, and excluded
    std::vector<std::string> required_terms;
    std::vector<std::string> optional_terms;
    std::vector<std::string> excluded_terms;
    std::vector<std::string> phrases;

    for (const auto& comp : components) {
        switch (comp.type) {
            case QueryComponent::Type::REQUIRED:
                required_terms.push_back(comp.value);
                break;
            case QueryComponent::Type::EXCLUDED:
                excluded_terms.push_back(comp.value);
                break;
            case QueryComponent::Type::PHRASE:
                phrases.push_back(comp.value);
                break;
            case QueryComponent::Type::TERM:
                optional_terms.push_back(comp.value);
                break;
        }
    }

    // Start with required terms (AND)
    std::vector<SearchResult> results;

    if (!required_terms.empty()) {
        auto req_result = search_and(required_terms);
        if (!req_result.ok()) return req_result;
        results = req_result.value();
    } else if (!optional_terms.empty()) {
        auto opt_result = search_or(optional_terms);
        if (!opt_result.ok()) return opt_result;
        results = opt_result.value();
    } else if (!phrases.empty()) {
        auto phrase_result = search_phrase(phrases[0]);
        if (!phrase_result.ok()) return phrase_result;
        results = phrase_result.value();
        phrases.erase(phrases.begin());
    }

    // Filter by additional phrases
    for (const auto& phrase : phrases) {
        auto phrase_result = search_phrase(phrase);
        if (!phrase_result.ok()) continue;

        std::set<FileId> phrase_docs;
        for (const auto& r : phrase_result.value()) {
            phrase_docs.insert(r.doc_id);
        }

        results.erase(
            std::remove_if(results.begin(), results.end(),
                [&phrase_docs](const SearchResult& r) {
                    return phrase_docs.find(r.doc_id) == phrase_docs.end();
                }),
            results.end());
    }

    // Exclude documents
    if (!excluded_terms.empty()) {
        std::set<FileId> excluded_docs;
        for (const auto& term : excluded_terms) {
            auto term_result = search_term(term);
            if (term_result.ok()) {
                for (const auto& r : term_result.value()) {
                    excluded_docs.insert(r.doc_id);
                }
            }
        }

        results.erase(
            std::remove_if(results.begin(), results.end(),
                [&excluded_docs](const SearchResult& r) {
                    return excluded_docs.find(r.doc_id) != excluded_docs.end();
                }),
            results.end());
    }

    return results;
}

// ============================================================================
// Low-Level Access
// ============================================================================

std::optional<PostingList> InvertedIndex::get_posting_list(const std::string& term) const {
    auto data = tree_.find(term);
    if (!data.has_value()) {
        return std::nullopt;
    }
    return deserialize_posting_list(data.value());
}

std::vector<std::string> InvertedIndex::get_all_terms() const {
    std::vector<std::string> terms;

    tree_.for_each([&terms](const std::string& term, const std::string&) {
        terms.push_back(term);
        return true;
    });

    return terms;
}

std::vector<std::string> InvertedIndex::get_terms_with_prefix(
    const std::string& prefix) const {

    std::vector<std::string> terms;

    // Use range query with prefix bounds
    std::string end_prefix = prefix;
    if (!end_prefix.empty()) {
        end_prefix.back()++;
    }

    auto entries = tree_.range(prefix, end_prefix);
    for (const auto& [term, _] : entries) {
        if (term.substr(0, prefix.size()) == prefix) {
            terms.push_back(term);
        }
    }

    return terms;
}

bool InvertedIndex::term_exists(const std::string& term) const {
    return tree_.contains(term);
}

uint32_t InvertedIndex::get_document_frequency(const std::string& term) const {
    auto posting_opt = get_posting_list(term);
    if (!posting_opt.has_value()) {
        return 0;
    }
    return posting_opt->document_frequency;
}

float InvertedIndex::average_document_length() const {
    if (document_count_ == 0) {
        return 0.0f;
    }
    return static_cast<float>(total_document_length_) /
           static_cast<float>(document_count_);
}

// ============================================================================
// Scoring
// ============================================================================

float InvertedIndex::calculate_tfidf(uint32_t term_freq, uint32_t doc_freq,
                                      uint32_t /*doc_length*/) const {
    if (document_count_ == 0 || doc_freq == 0) {
        return 0.0f;
    }

    // TF: log(1 + term_freq)
    float tf = std::log(1.0f + static_cast<float>(term_freq));

    // IDF: log(N / df)
    float idf = std::log(static_cast<float>(document_count_) /
                         static_cast<float>(doc_freq));

    return tf * idf;
}

float InvertedIndex::calculate_bm25(uint32_t term_freq, uint32_t doc_freq,
                                     uint32_t doc_length) const {
    if (document_count_ == 0 || doc_freq == 0) {
        return 0.0f;
    }

    float avgdl = average_document_length();
    if (avgdl == 0.0f) avgdl = 1.0f;

    // IDF component
    float idf = std::log((static_cast<float>(document_count_) - doc_freq + 0.5f) /
                         (doc_freq + 0.5f) + 1.0f);

    // TF component with length normalization
    float tf = static_cast<float>(term_freq);
    float dl = static_cast<float>(doc_length);

    float numerator = tf * (config_.k1 + 1.0f);
    float denominator = tf + config_.k1 * (1.0f - config_.b + config_.b * (dl / avgdl));

    return idf * (numerator / denominator);
}

// ============================================================================
// Result Merging
// ============================================================================

std::vector<SearchResult> InvertedIndex::merge_and_results(
    const std::vector<PostingList>& lists) const {

    if (lists.empty()) {
        return {};
    }

    // Start with smallest posting list for efficiency
    size_t smallest_idx = 0;
    size_t smallest_size = lists[0].postings.size();
    for (size_t i = 1; i < lists.size(); ++i) {
        if (lists[i].postings.size() < smallest_size) {
            smallest_size = lists[i].postings.size();
            smallest_idx = i;
        }
    }

    // Get candidate documents from smallest list
    std::set<FileId> candidates;
    for (const auto& p : lists[smallest_idx].postings) {
        candidates.insert(p.doc_id);
    }

    // Intersect with other lists
    for (size_t i = 0; i < lists.size(); ++i) {
        if (i == smallest_idx) continue;

        std::set<FileId> list_docs;
        for (const auto& p : lists[i].postings) {
            list_docs.insert(p.doc_id);
        }

        std::set<FileId> intersection;
        std::set_intersection(candidates.begin(), candidates.end(),
                              list_docs.begin(), list_docs.end(),
                              std::inserter(intersection, intersection.begin()));
        candidates = std::move(intersection);

        if (candidates.empty()) break;
    }

    // Calculate scores for surviving documents
    std::vector<SearchResult> results;
    for (FileId doc_id : candidates) {
        SearchResult result;
        result.doc_id = doc_id;
        result.score = 0.0f;

        uint32_t doc_length = 0;
        auto len_it = doc_lengths_.find(doc_id);
        if (len_it != doc_lengths_.end()) {
            doc_length = len_it->second;
        }

        // Sum scores from all terms
        for (const auto& list : lists) {
            const Posting* posting = list.find(doc_id);
            if (posting) {
                result.score += calculate_bm25(posting->frequency,
                                               list.document_frequency,
                                               doc_length);
                // Merge positions
                result.positions.insert(result.positions.end(),
                                        posting->positions.begin(),
                                        posting->positions.end());
            }
        }

        results.push_back(result);
    }

    std::sort(results.begin(), results.end());

    if (results.size() > config_.max_results) {
        results.resize(config_.max_results);
    }

    return results;
}

std::vector<SearchResult> InvertedIndex::merge_or_results(
    const std::vector<PostingList>& lists) const {

    // Collect all documents with their scores
    std::map<FileId, SearchResult> doc_scores;

    for (const auto& list : lists) {
        for (const auto& posting : list.postings) {
            uint32_t doc_length = 0;
            auto len_it = doc_lengths_.find(posting.doc_id);
            if (len_it != doc_lengths_.end()) {
                doc_length = len_it->second;
            }

            float score = calculate_bm25(posting.frequency,
                                         list.document_frequency,
                                         doc_length);

            auto& result = doc_scores[posting.doc_id];
            result.doc_id = posting.doc_id;
            result.score += score;
            result.positions.insert(result.positions.end(),
                                   posting.positions.begin(),
                                   posting.positions.end());
        }
    }

    std::vector<SearchResult> results;
    results.reserve(doc_scores.size());
    for (auto& [_, result] : doc_scores) {
        results.push_back(std::move(result));
    }

    std::sort(results.begin(), results.end());

    if (results.size() > config_.max_results) {
        results.resize(config_.max_results);
    }

    return results;
}

bool InvertedIndex::check_phrase_match(const std::vector<PostingList>& lists,
                                        FileId doc_id) const {
    if (lists.empty()) return false;

    // Get positions for first term
    const Posting* first = lists[0].find(doc_id);
    if (!first || first->positions.empty()) return false;

    // Check each starting position
    for (uint32_t start_pos : first->positions) {
        bool match = true;

        for (size_t i = 1; i < lists.size() && match; ++i) {
            const Posting* posting = lists[i].find(doc_id);
            if (!posting) {
                match = false;
                break;
            }

            // Look for position = start_pos + i
            uint32_t expected = start_pos + static_cast<uint32_t>(i);
            bool found = std::binary_search(posting->positions.begin(),
                                           posting->positions.end(),
                                           expected);
            if (!found) {
                match = false;
            }
        }

        if (match) return true;
    }

    return false;
}

// ============================================================================
// Query Parsing
// ============================================================================

std::vector<InvertedIndex::QueryComponent> InvertedIndex::parse_query(
    const std::string& query) const {

    std::vector<QueryComponent> components;
    std::istringstream ss(query);
    std::string token;

    while (ss >> token) {
        if (token.empty()) continue;

        QueryComponent comp;

        if (token[0] == '+' && token.size() > 1) {
            comp.type = QueryComponent::Type::REQUIRED;
            comp.value = token.substr(1);
        } else if (token[0] == '-' && token.size() > 1) {
            comp.type = QueryComponent::Type::EXCLUDED;
            comp.value = token.substr(1);
        } else if (token[0] == '"') {
            // Start of phrase
            comp.type = QueryComponent::Type::PHRASE;
            std::string phrase = token.substr(1);

            // Continue until closing quote
            while (!phrase.empty() && phrase.back() != '"' && ss >> token) {
                phrase += " " + token;
            }

            if (!phrase.empty() && phrase.back() == '"') {
                phrase.pop_back();
            }
            comp.value = phrase;
        } else if (token == "AND" || token == "OR") {
            // Skip boolean operators (handled implicitly)
            continue;
        } else {
            comp.type = QueryComponent::Type::TERM;
            comp.value = token;
        }

        if (!comp.value.empty()) {
            components.push_back(comp);
        }
    }

    return components;
}

}  // namespace dam::search
