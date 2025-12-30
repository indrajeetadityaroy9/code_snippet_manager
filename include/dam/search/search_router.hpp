#pragma once

#include <dam/core_types.hpp>
#include <dam/types.hpp>
#include <dam/result.hpp>
#include <dam/storage/buffer_pool.hpp>
#include <dam/search/inverted_index.hpp>
#include <dam/search/trigram_index.hpp>
#include <dam/search/vector_index.hpp>

#include <memory>
#include <string>
#include <vector>

namespace dam::search {

// ============================================================================
// Search Query Types
// ============================================================================

enum class SearchMode {
    AUTO,           // Automatically detect best mode
    KEYWORD,        // Full-text keyword search (inverted index)
    FUZZY,          // Fuzzy/typo-tolerant search (trigram index)
    SUBSTRING,      // Substring matching (trigram index)
    SEMANTIC,       // Semantic similarity search (vector index)
    HYBRID          // Combine all available indexes
};

struct SearchQuery {
    std::string query;
    SearchMode mode = SearchMode::AUTO;

    // Result limits
    size_t max_results = 50;

    // Fuzzy search threshold (0.0 - 1.0)
    float fuzzy_threshold = 0.3f;

    // Semantic search threshold (0.0 - 1.0)
    float semantic_threshold = 0.5f;

    // Hybrid scoring weights (must sum to 1.0)
    float keyword_weight = 0.4f;
    float fuzzy_weight = 0.2f;
    float semantic_weight = 0.4f;

    // Query modifiers (parsed from query string)
    bool require_exact = false;     // "exact term"
    bool allow_fuzzy = false;       // Force fuzzy search (~prefix)
    std::vector<std::string> required_terms;    // +term
    std::vector<std::string> excluded_terms;    // -term
};

// ============================================================================
// Unified Search Result
// ============================================================================

struct UnifiedSearchResult {
    SnippetId doc_id;

    // Individual scores (0.0 - 1.0, higher = better)
    float keyword_score = 0.0f;
    float fuzzy_score = 0.0f;
    float semantic_score = 0.0f;

    // Combined score
    float final_score = 0.0f;

    // Match details
    std::vector<uint32_t> positions;    // Matched positions (from keyword search)
    std::string matched_text;           // Best matching text (from fuzzy search)

    bool operator<(const UnifiedSearchResult& other) const {
        return final_score > other.final_score;
    }
};

// ============================================================================
// Search Router Configuration
// ============================================================================

struct SearchRouterConfig {
    // Enable/disable individual indexes
    bool enable_keyword_search = true;
    bool enable_fuzzy_search = true;
    bool enable_semantic_search = true;

    // Default weights for hybrid search
    float default_keyword_weight = 0.4f;
    float default_fuzzy_weight = 0.2f;
    float default_semantic_weight = 0.4f;

    // Auto-detection thresholds
    size_t semantic_query_min_length = 10;  // Use semantic for longer queries
    float fuzzy_trigger_similarity = 0.8f;  // Below this, add fuzzy results

    // Tokenizer config for inverted index
    TokenizerConfig tokenizer_config;

    // Trigram index config
    TrigramIndexConfig trigram_config;

    // Vector index config
    VectorIndexConfig vector_config;
};

// ============================================================================
// Search Router
// ============================================================================

/**
 * SearchRouter - Unified interface for all search indexes.
 *
 * Combines inverted index (keyword), trigram index (fuzzy),
 * and vector index (semantic) with intelligent query routing
 * and hybrid scoring.
 *
 * Features:
 * - Auto-detects query type
 * - Hybrid scoring with configurable weights
 * - Parses query syntax (+required, -excluded, "phrase")
 * - Fallback between indexes
 */
class SearchRouter {
public:
    /**
     * Create a search router.
     *
     * @param buffer_pool Buffer pool for B+ tree-based indexes
     * @param config Router configuration
     */
    SearchRouter(BufferPool* buffer_pool, SearchRouterConfig config = {});

    ~SearchRouter();

    // Non-copyable
    SearchRouter(const SearchRouter&) = delete;
    SearchRouter& operator=(const SearchRouter&) = delete;

    // ========================================================================
    // Initialization
    // ========================================================================

    /**
     * Initialize all enabled indexes.
     */
    Result<void> initialize();

    /**
     * Initialize with existing root page IDs.
     */
    Result<void> initialize(PageId inverted_root,
                             PageId trigram_root,
                             const std::string& vector_path = "");

    /**
     * Check if initialized.
     */
    bool is_initialized() const { return initialized_; }

    // ========================================================================
    // Indexing
    // ========================================================================

    /**
     * Index a document.
     *
     * @param doc_id Document identifier
     * @param content Document content
     * @return Success or error
     */
    Result<void> index_document(SnippetId doc_id, const std::string& content);

    /**
     * Index code with code-aware tokenization.
     */
    Result<void> index_code(SnippetId doc_id, const std::string& code);

    /**
     * Remove a document from all indexes.
     */
    Result<void> remove_document(SnippetId doc_id, const std::string& content);

    /**
     * Update a document in all indexes.
     */
    Result<void> update_document(SnippetId doc_id,
                                  const std::string& old_content,
                                  const std::string& new_content);

    /**
     * Index multiple documents in batch.
     */
    Result<void> index_batch(
        const std::vector<SnippetId>& doc_ids,
        const std::vector<std::string>& contents,
        EmbedProgressCallback callback = nullptr);

    // ========================================================================
    // Search
    // ========================================================================

    /**
     * Search with a query string.
     *
     * @param query The search query
     * @return Ranked results
     */
    Result<std::vector<UnifiedSearchResult>> search(const std::string& query) const;

    /**
     * Search with full query options.
     *
     * @param query The search query with options
     * @return Ranked results
     */
    Result<std::vector<UnifiedSearchResult>> search(const SearchQuery& query) const;

    /**
     * Keyword search only.
     */
    Result<std::vector<UnifiedSearchResult>> search_keyword(
        const std::string& query,
        size_t max_results = 50) const;

    /**
     * Fuzzy search only.
     */
    Result<std::vector<UnifiedSearchResult>> search_fuzzy(
        const std::string& query,
        float threshold = 0.3f,
        size_t max_results = 50) const;

    /**
     * Semantic search only.
     */
    Result<std::vector<UnifiedSearchResult>> search_semantic(
        const std::string& query,
        float threshold = 0.5f,
        size_t max_results = 50) const;

    // ========================================================================
    // Query Analysis
    // ========================================================================

    /**
     * Analyze a query string to determine the best search mode.
     */
    SearchMode analyze_query(const std::string& query) const;

    /**
     * Parse query string into structured query.
     */
    SearchQuery parse_query(const std::string& query_str) const;

    // ========================================================================
    // Index Access
    // ========================================================================

    /**
     * Get underlying indexes (for advanced use).
     */
    InvertedIndex* inverted_index() { return inverted_.get(); }
    const InvertedIndex* inverted_index() const { return inverted_.get(); }

    TrigramIndex* trigram_index() { return trigram_.get(); }
    const TrigramIndex* trigram_index() const { return trigram_.get(); }

    VectorIndex* vector_index() { return vector_ ? vector_->index() : nullptr; }
    const VectorIndex* vector_index() const {
        return vector_ ? vector_->index() : nullptr;
    }

    // ========================================================================
    // Statistics
    // ========================================================================

    /**
     * Get statistics about the indexes.
     */
    struct Stats {
        size_t document_count = 0;
        size_t term_count = 0;
        size_t trigram_count = 0;
        size_t vector_count = 0;
        bool keyword_available = false;
        bool fuzzy_available = false;
        bool semantic_available = false;
    };
    Stats get_stats() const;

    /**
     * Get root page IDs for persistence.
     */
    PageId get_inverted_root_page_id() const;
    PageId get_trigram_root_page_id() const;

private:
    // Detect query type from content
    SearchMode detect_search_mode(const SearchQuery& query) const;

    // Individual search methods returning unified results
    std::vector<UnifiedSearchResult> do_keyword_search(
        const SearchQuery& query) const;

    std::vector<UnifiedSearchResult> do_fuzzy_search(
        const SearchQuery& query) const;

    std::vector<UnifiedSearchResult> do_substring_search(
        const SearchQuery& query) const;

    std::vector<UnifiedSearchResult> do_semantic_search(
        const SearchQuery& query) const;

    // Merge results from different indexes
    std::vector<UnifiedSearchResult> merge_results(
        std::vector<UnifiedSearchResult>& keyword_results,
        std::vector<UnifiedSearchResult>& fuzzy_results,
        std::vector<UnifiedSearchResult>& semantic_results,
        const SearchQuery& query) const;

    // Normalize scores to 0-1 range
    void normalize_scores(std::vector<UnifiedSearchResult>& results) const;

    BufferPool* buffer_pool_;
    SearchRouterConfig config_;

    std::unique_ptr<InvertedIndex> inverted_;
    std::unique_ptr<TrigramIndex> trigram_;
    std::unique_ptr<VectorIndexWithEmbedder> vector_;

    bool initialized_ = false;
};

// ============================================================================
// Factory
// ============================================================================

class SearchRouterFactory {
public:
    /**
     * Create search router with all available indexes from environment.
     */
    static Result<std::unique_ptr<SearchRouter>> create_from_env(
        BufferPool* buffer_pool,
        SearchRouterConfig config = {});
};

}  // namespace dam::search
