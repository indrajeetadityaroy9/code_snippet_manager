#include <dam/search/search_router.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <regex>
#include <sstream>

namespace dam::search {

// ============================================================================
// SearchRouter Implementation
// ============================================================================

SearchRouter::SearchRouter(BufferPool* buffer_pool, SearchRouterConfig config)
    : buffer_pool_(buffer_pool)
    , config_(std::move(config)) {}

SearchRouter::~SearchRouter() = default;

Result<void> SearchRouter::initialize() {
    return initialize(INVALID_PAGE_ID, INVALID_PAGE_ID, "");
}

Result<void> SearchRouter::initialize(PageId inverted_root,
                                       PageId trigram_root,
                                       const std::string& vector_path) {
    if (initialized_) {
        return {};
    }

    // Initialize inverted index
    if (config_.enable_keyword_search) {
        InvertedIndexConfig inv_config;
        inv_config.tokenizer = config_.tokenizer_config;
        inverted_ = std::make_unique<InvertedIndex>(
            buffer_pool_, inverted_root, inv_config);
    }

    // Initialize trigram index
    if (config_.enable_fuzzy_search) {
        trigram_ = std::make_unique<TrigramIndex>(
            buffer_pool_, trigram_root, config_.trigram_config);
    }

    // Initialize vector index
    if (config_.enable_semantic_search) {
        auto result = VectorIndexWithEmbedder::create_from_env(config_.vector_config);
        if (result.ok()) {
            vector_ = std::move(result.value());

            // Load from disk if path provided
            if (!vector_path.empty()) {
                auto load_result = vector_->index()->load(vector_path);
                if (!load_result.ok()) {
                    // Not fatal - just start with empty index
                }
            }
        }
        // Not fatal if vector search unavailable
    }

    initialized_ = true;
    return {};
}

// ============================================================================
// Indexing
// ============================================================================

Result<void> SearchRouter::index_document(SnippetId doc_id, const std::string& content) {
    if (!initialized_) {
        return Error(ErrorCode::INVALID_ARGUMENT, "Router not initialized");
    }

    if (inverted_) {
        auto result = inverted_->index_document(doc_id, content);
        if (!result.ok()) {
            return result;
        }
    }

    if (trigram_) {
        auto result = trigram_->index_document(doc_id, content);
        if (!result.ok()) {
            return result;
        }
    }

    if (vector_) {
        auto result = vector_->index_text(doc_id, content);
        if (!result.ok()) {
            // Vector indexing failures are not fatal
        }
    }

    return {};
}

Result<void> SearchRouter::index_code(SnippetId doc_id, const std::string& code) {
    if (!initialized_) {
        return Error(ErrorCode::INVALID_ARGUMENT, "Router not initialized");
    }

    if (inverted_) {
        auto result = inverted_->index_code(doc_id, code);
        if (!result.ok()) {
            return result;
        }
    }

    if (trigram_) {
        auto result = trigram_->index_document(doc_id, code);
        if (!result.ok()) {
            return result;
        }
    }

    if (vector_) {
        auto result = vector_->index_text(doc_id, code);
        // Not fatal
    }

    return {};
}

Result<void> SearchRouter::remove_document(SnippetId doc_id, const std::string& content) {
    if (!initialized_) {
        return Error(ErrorCode::INVALID_ARGUMENT, "Router not initialized");
    }

    if (inverted_) {
        inverted_->remove_document(doc_id, content);
    }

    if (trigram_) {
        trigram_->remove_document(doc_id, content);
    }

    if (vector_ && vector_->index()) {
        vector_->index()->remove(doc_id);
    }

    return {};
}

Result<void> SearchRouter::update_document(SnippetId doc_id,
                                            const std::string& old_content,
                                            const std::string& new_content) {
    auto remove_result = remove_document(doc_id, old_content);
    if (!remove_result.ok()) {
        return remove_result;
    }
    return index_document(doc_id, new_content);
}

Result<void> SearchRouter::index_batch(
    const std::vector<SnippetId>& doc_ids,
    const std::vector<std::string>& contents,
    EmbedProgressCallback callback) {

    if (doc_ids.size() != contents.size()) {
        return Error(ErrorCode::INVALID_ARGUMENT, "Size mismatch");
    }

    for (size_t i = 0; i < doc_ids.size(); ++i) {
        auto result = index_document(doc_ids[i], contents[i]);
        if (!result.ok()) {
            return result;
        }

        if (callback) {
            callback(i + 1, doc_ids.size());
        }
    }

    return {};
}

// ============================================================================
// Search
// ============================================================================

Result<std::vector<UnifiedSearchResult>> SearchRouter::search(
    const std::string& query) const {

    SearchQuery sq = parse_query(query);
    return search(sq);
}

Result<std::vector<UnifiedSearchResult>> SearchRouter::search(
    const SearchQuery& query) const {

    if (!initialized_) {
        return Error(ErrorCode::INVALID_ARGUMENT, "Router not initialized");
    }

    if (query.query.empty()) {
        return std::vector<UnifiedSearchResult>{};
    }

    SearchMode mode = query.mode;
    if (mode == SearchMode::AUTO) {
        mode = detect_search_mode(query);
    }

    std::vector<UnifiedSearchResult> keyword_results;
    std::vector<UnifiedSearchResult> fuzzy_results;
    std::vector<UnifiedSearchResult> semantic_results;

    switch (mode) {
        case SearchMode::KEYWORD:
            keyword_results = do_keyword_search(query);
            break;

        case SearchMode::FUZZY:
            fuzzy_results = do_fuzzy_search(query);
            break;

        case SearchMode::SUBSTRING:
            fuzzy_results = do_substring_search(query);
            break;

        case SearchMode::SEMANTIC:
            semantic_results = do_semantic_search(query);
            break;

        case SearchMode::HYBRID:
        case SearchMode::AUTO:
        default:
            keyword_results = do_keyword_search(query);
            fuzzy_results = do_fuzzy_search(query);
            semantic_results = do_semantic_search(query);
            break;
    }

    auto results = merge_results(keyword_results, fuzzy_results,
                                  semantic_results, query);

    // Sort by final score
    std::sort(results.begin(), results.end());

    // Limit results
    if (results.size() > query.max_results) {
        results.resize(query.max_results);
    }

    return results;
}

Result<std::vector<UnifiedSearchResult>> SearchRouter::search_keyword(
    const std::string& query,
    size_t max_results) const {

    SearchQuery sq;
    sq.query = query;
    sq.mode = SearchMode::KEYWORD;
    sq.max_results = max_results;
    return search(sq);
}

Result<std::vector<UnifiedSearchResult>> SearchRouter::search_fuzzy(
    const std::string& query,
    float threshold,
    size_t max_results) const {

    SearchQuery sq;
    sq.query = query;
    sq.mode = SearchMode::FUZZY;
    sq.fuzzy_threshold = threshold;
    sq.max_results = max_results;
    return search(sq);
}

Result<std::vector<UnifiedSearchResult>> SearchRouter::search_semantic(
    const std::string& query,
    float threshold,
    size_t max_results) const {

    SearchQuery sq;
    sq.query = query;
    sq.mode = SearchMode::SEMANTIC;
    sq.semantic_threshold = threshold;
    sq.max_results = max_results;
    return search(sq);
}

// ============================================================================
// Query Analysis
// ============================================================================

SearchMode SearchRouter::analyze_query(const std::string& query) const {
    SearchQuery sq;
    sq.query = query;
    return detect_search_mode(sq);
}

SearchQuery SearchRouter::parse_query(const std::string& query_str) const {
    SearchQuery query;
    query.query = query_str;

    std::istringstream ss(query_str);
    std::string token;
    std::string clean_query;

    while (ss >> token) {
        if (token.empty()) continue;

        if (token[0] == '+' && token.size() > 1) {
            query.required_terms.push_back(token.substr(1));
        } else if (token[0] == '-' && token.size() > 1) {
            query.excluded_terms.push_back(token.substr(1));
        } else if (token[0] == '~') {
            query.allow_fuzzy = true;
            if (token.size() > 1) {
                if (!clean_query.empty()) clean_query += " ";
                clean_query += token.substr(1);
            }
        } else if (token[0] == '"') {
            query.require_exact = true;
            // Collect phrase
            std::string phrase = token.substr(1);
            while (!phrase.empty() && phrase.back() != '"' && ss >> token) {
                phrase += " " + token;
            }
            if (!phrase.empty() && phrase.back() == '"') {
                phrase.pop_back();
            }
            if (!clean_query.empty()) clean_query += " ";
            clean_query += phrase;
        } else {
            if (!clean_query.empty()) clean_query += " ";
            clean_query += token;
        }
    }

    if (!clean_query.empty()) {
        query.query = clean_query;
    }

    // Set weights from config
    query.keyword_weight = config_.default_keyword_weight;
    query.fuzzy_weight = config_.default_fuzzy_weight;
    query.semantic_weight = config_.default_semantic_weight;

    return query;
}

SearchMode SearchRouter::detect_search_mode(const SearchQuery& query) const {
    const std::string& q = query.query;

    // Check for explicit phrase (quoted)
    if (query.require_exact) {
        return SearchMode::KEYWORD;
    }

    // Check for explicit fuzzy request (~prefix in query)
    if (query.allow_fuzzy && trigram_) {
        return SearchMode::FUZZY;
    }

    // Check for typo patterns (unusual character sequences)
    bool has_unusual_pattern = false;
    for (size_t i = 1; i < q.size(); ++i) {
        if (!std::isalnum(q[i]) && !std::isspace(q[i]) && q[i] != '_') {
            has_unusual_pattern = true;
            break;
        }
    }

    // Short single-word queries: likely exact match
    size_t word_count = 0;
    bool in_word = false;
    for (char c : q) {
        if (std::isalnum(c)) {
            if (!in_word) {
                word_count++;
                in_word = true;
            }
        } else {
            in_word = false;
        }
    }

    if (word_count == 1 && q.size() < 20) {
        // Single word - try keyword first, fuzzy as fallback
        return SearchMode::KEYWORD;
    }

    // Natural language queries (multiple words): prefer semantic
    if (word_count >= 3 && q.size() >= config_.semantic_query_min_length) {
        if (vector_) {
            return SearchMode::SEMANTIC;
        }
    }

    // Questions: prefer semantic
    if (q.find('?') != std::string::npos ||
        q.find("how") != std::string::npos ||
        q.find("what") != std::string::npos ||
        q.find("why") != std::string::npos) {
        if (vector_) {
            return SearchMode::SEMANTIC;
        }
    }

    // Default: hybrid if all indexes available
    if (inverted_ && trigram_ && vector_) {
        return SearchMode::HYBRID;
    }

    // Fallback based on available indexes
    if (inverted_) {
        return SearchMode::KEYWORD;
    }
    if (trigram_) {
        return SearchMode::FUZZY;
    }
    if (vector_) {
        return SearchMode::SEMANTIC;
    }

    return SearchMode::KEYWORD;
}

// ============================================================================
// Individual Search Methods
// ============================================================================

std::vector<UnifiedSearchResult> SearchRouter::do_keyword_search(
    const SearchQuery& query) const {

    std::vector<UnifiedSearchResult> results;

    if (!inverted_) {
        return results;
    }

    auto search_result = inverted_->search(query.query);
    if (!search_result.ok()) {
        return results;
    }

    for (const auto& r : search_result.value()) {
        UnifiedSearchResult ur;
        ur.doc_id = r.doc_id;
        ur.keyword_score = r.score;
        ur.positions = r.positions;
        results.push_back(ur);
    }

    return results;
}

std::vector<UnifiedSearchResult> SearchRouter::do_fuzzy_search(
    const SearchQuery& query) const {

    std::vector<UnifiedSearchResult> results;

    if (!trigram_) {
        return results;
    }

    auto search_result = trigram_->search_fuzzy(query.query, query.fuzzy_threshold);
    if (!search_result.ok()) {
        return results;
    }

    for (const auto& r : search_result.value()) {
        UnifiedSearchResult ur;
        ur.doc_id = r.doc_id;
        ur.fuzzy_score = r.similarity;
        ur.matched_text = r.matched_text;
        results.push_back(ur);
    }

    return results;
}

std::vector<UnifiedSearchResult> SearchRouter::do_substring_search(
    const SearchQuery& query) const {

    std::vector<UnifiedSearchResult> results;

    if (!trigram_) {
        return results;
    }

    auto search_result = trigram_->search_substring(query.query);
    if (!search_result.ok()) {
        return results;
    }

    for (const auto& doc_id : search_result.value()) {
        UnifiedSearchResult ur;
        ur.doc_id = doc_id;
        ur.fuzzy_score = 1.0f;  // Exact substring match gets perfect score
        results.push_back(ur);
    }

    return results;
}

std::vector<UnifiedSearchResult> SearchRouter::do_semantic_search(
    const SearchQuery& query) const {

    std::vector<UnifiedSearchResult> results;

    if (!vector_) {
        return results;
    }

    auto search_result = vector_->search_similar(
        query.query, query.semantic_threshold, query.max_results);

    if (!search_result.ok()) {
        return results;
    }

    for (const auto& r : search_result.value()) {
        UnifiedSearchResult ur;
        ur.doc_id = r.doc_id;
        ur.semantic_score = r.similarity;
        results.push_back(ur);
    }

    return results;
}

// ============================================================================
// Result Merging
// ============================================================================

void SearchRouter::normalize_scores(std::vector<UnifiedSearchResult>& results) const {
    if (results.empty()) return;

    // Find max scores for each type
    float max_keyword = 0.0f;
    float max_fuzzy = 0.0f;
    float max_semantic = 0.0f;

    for (const auto& r : results) {
        max_keyword = std::max(max_keyword, r.keyword_score);
        max_fuzzy = std::max(max_fuzzy, r.fuzzy_score);
        max_semantic = std::max(max_semantic, r.semantic_score);
    }

    // Normalize to 0-1 range
    for (auto& r : results) {
        if (max_keyword > 0) r.keyword_score /= max_keyword;
        if (max_fuzzy > 0) r.fuzzy_score /= max_fuzzy;
        if (max_semantic > 0) r.semantic_score /= max_semantic;
    }
}

std::vector<UnifiedSearchResult> SearchRouter::merge_results(
    std::vector<UnifiedSearchResult>& keyword_results,
    std::vector<UnifiedSearchResult>& fuzzy_results,
    std::vector<UnifiedSearchResult>& semantic_results,
    const SearchQuery& query) const {

    // Normalize individual result sets
    normalize_scores(keyword_results);
    normalize_scores(fuzzy_results);
    normalize_scores(semantic_results);

    // Merge by doc_id
    std::map<SnippetId, UnifiedSearchResult> merged;

    for (const auto& r : keyword_results) {
        merged[r.doc_id].doc_id = r.doc_id;
        merged[r.doc_id].keyword_score = r.keyword_score;
        merged[r.doc_id].positions = r.positions;
    }

    for (const auto& r : fuzzy_results) {
        merged[r.doc_id].doc_id = r.doc_id;
        merged[r.doc_id].fuzzy_score = r.fuzzy_score;
        if (merged[r.doc_id].matched_text.empty()) {
            merged[r.doc_id].matched_text = r.matched_text;
        }
    }

    for (const auto& r : semantic_results) {
        merged[r.doc_id].doc_id = r.doc_id;
        merged[r.doc_id].semantic_score = r.semantic_score;
    }

    // Calculate final scores
    std::vector<UnifiedSearchResult> results;
    results.reserve(merged.size());

    for (auto& [doc_id, result] : merged) {
        result.final_score =
            query.keyword_weight * result.keyword_score +
            query.fuzzy_weight * result.fuzzy_score +
            query.semantic_weight * result.semantic_score;

        results.push_back(std::move(result));
    }

    return results;
}

// ============================================================================
// Statistics
// ============================================================================

SearchRouter::Stats SearchRouter::get_stats() const {
    Stats stats;

    if (inverted_) {
        stats.document_count = inverted_->document_count();
        stats.term_count = inverted_->term_count();
        stats.keyword_available = true;
    }

    if (trigram_) {
        stats.trigram_count = trigram_->trigram_count();
        stats.fuzzy_available = true;
    }

    if (vector_ && vector_->index()) {
        stats.vector_count = vector_->index()->size();
        stats.semantic_available = true;
    }

    return stats;
}

PageId SearchRouter::get_inverted_root_page_id() const {
    return inverted_ ? inverted_->get_root_page_id() : INVALID_PAGE_ID;
}

PageId SearchRouter::get_trigram_root_page_id() const {
    return trigram_ ? trigram_->get_root_page_id() : INVALID_PAGE_ID;
}

// ============================================================================
// Factory
// ============================================================================

Result<std::unique_ptr<SearchRouter>> SearchRouterFactory::create_from_env(
    BufferPool* buffer_pool,
    SearchRouterConfig config) {

    auto router = std::make_unique<SearchRouter>(buffer_pool, std::move(config));
    auto init_result = router->initialize();
    if (!init_result.ok()) {
        return init_result.error();
    }
    return router;
}

}  // namespace dam::search
