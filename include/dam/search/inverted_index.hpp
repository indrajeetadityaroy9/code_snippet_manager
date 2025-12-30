#pragma once

#include <dam/core_types.hpp>
#include <dam/result.hpp>
#include <dam/storage/btree.hpp>
#include <dam/search/tokenizer.hpp>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace dam::search {

// ============================================================================
// Posting Entry - Single document occurrence
// ============================================================================

struct Posting {
    FileId doc_id;
    uint32_t frequency;                  // Term frequency in document
    std::vector<uint32_t> positions;     // Word positions for phrase queries

    bool operator<(const Posting& other) const {
        return doc_id < other.doc_id;
    }

    bool operator==(const Posting& other) const {
        return doc_id == other.doc_id;
    }
};

// ============================================================================
// Posting List - All documents containing a term
// ============================================================================

struct PostingList {
    std::string term;
    uint32_t document_frequency;         // Number of documents containing term
    std::vector<Posting> postings;       // Sorted by doc_id

    // Find posting for a specific document
    const Posting* find(FileId doc_id) const;

    // Add or update a posting
    void add_posting(FileId doc_id, const std::vector<uint32_t>& positions);

    // Remove a document from this posting list
    bool remove_posting(FileId doc_id);
};

// ============================================================================
// Search Result with Scoring
// ============================================================================

struct SearchResult {
    FileId doc_id;
    float score;                         // TF-IDF or BM25 score
    std::vector<uint32_t> positions;     // Matching positions (for highlighting)

    bool operator<(const SearchResult& other) const {
        return score > other.score;      // Higher score = better rank
    }
};

// ============================================================================
// Inverted Index Configuration
// ============================================================================

struct InvertedIndexConfig {
    // Tokenizer settings
    TokenizerConfig tokenizer;

    // Scoring parameters (BM25)
    float k1 = 1.2f;                     // Term frequency saturation
    float b = 0.75f;                     // Length normalization

    // Result limits
    size_t max_results = 100;

    // Index behavior
    bool store_positions = true;          // Required for phrase queries
    bool enable_prefix_search = false;    // Allow prefix matching
};

// ============================================================================
// Inverted Index
// ============================================================================

/**
 * InvertedIndex - Full-text search index using inverted file structure.
 *
 * Maps terms to posting lists (documents containing the term).
 * Supports:
 * - Single term queries
 * - Boolean queries (AND, OR, NOT)
 * - Phrase queries (when positions stored)
 * - TF-IDF / BM25 ranking
 */
class InvertedIndex {
public:
    /**
     * Create an inverted index backed by a B+ tree.
     *
     * @param buffer_pool The buffer pool for page management
     * @param root_page_id Root page ID (INVALID_PAGE_ID to create new)
     * @param config Index configuration
     */
    InvertedIndex(BufferPool* buffer_pool,
                  PageId root_page_id = INVALID_PAGE_ID,
                  InvertedIndexConfig config = {});

    // ========================================================================
    // Indexing Operations
    // ========================================================================

    /**
     * Index a document's content.
     *
     * @param doc_id The document identifier
     * @param content The text content to index
     * @return Success or error
     */
    Result<void> index_document(FileId doc_id, const std::string& content);

    /**
     * Index code content with code-aware tokenization.
     *
     * @param doc_id The document identifier
     * @param code The code content to index
     * @return Success or error
     */
    Result<void> index_code(FileId doc_id, const std::string& code);

    /**
     * Remove a document from the index.
     *
     * @param doc_id The document to remove
     * @param content The document's content (needed for term lookup)
     * @return Success or error
     */
    Result<void> remove_document(FileId doc_id, const std::string& content);

    /**
     * Update a document (remove old, add new).
     *
     * @param doc_id The document identifier
     * @param old_content Previous content
     * @param new_content New content
     * @return Success or error
     */
    Result<void> update_document(FileId doc_id,
                                  const std::string& old_content,
                                  const std::string& new_content);

    // ========================================================================
    // Search Operations
    // ========================================================================

    /**
     * Search for a single term.
     *
     * @param term The term to search for
     * @return Ranked results
     */
    Result<std::vector<SearchResult>> search_term(const std::string& term) const;

    /**
     * Search for documents containing ALL terms (AND query).
     *
     * @param terms The terms that must all appear
     * @return Ranked results
     */
    Result<std::vector<SearchResult>> search_and(
        const std::vector<std::string>& terms) const;

    /**
     * Search for documents containing ANY term (OR query).
     *
     * @param terms The terms where at least one must appear
     * @return Ranked results
     */
    Result<std::vector<SearchResult>> search_or(
        const std::vector<std::string>& terms) const;

    /**
     * Search for an exact phrase.
     *
     * @param phrase The phrase to search for
     * @return Ranked results
     */
    Result<std::vector<SearchResult>> search_phrase(
        const std::string& phrase) const;

    /**
     * Search with a parsed query string.
     * Supports: term, "phrase", +required, -excluded, term1 AND term2, term1 OR term2
     *
     * @param query The query string
     * @return Ranked results
     */
    Result<std::vector<SearchResult>> search(const std::string& query) const;

    // ========================================================================
    // Low-Level Access
    // ========================================================================

    /**
     * Get the posting list for a term.
     *
     * @param term The term to look up
     * @return The posting list or nullopt if not found
     */
    std::optional<PostingList> get_posting_list(const std::string& term) const;

    /**
     * Get all terms in the index.
     *
     * @return Vector of all indexed terms
     */
    std::vector<std::string> get_all_terms() const;

    /**
     * Get terms starting with a prefix.
     *
     * @param prefix The prefix to match
     * @return Vector of matching terms
     */
    std::vector<std::string> get_terms_with_prefix(const std::string& prefix) const;

    /**
     * Check if a term exists in the index.
     */
    bool term_exists(const std::string& term) const;

    /**
     * Get document frequency for a term.
     */
    uint32_t get_document_frequency(const std::string& term) const;

    // ========================================================================
    // Statistics
    // ========================================================================

    /**
     * Get total number of indexed documents.
     */
    size_t document_count() const { return document_count_; }

    /**
     * Get total number of unique terms.
     */
    size_t term_count() const { return tree_.size(); }

    /**
     * Get average document length.
     */
    float average_document_length() const;

    /**
     * Get root page ID for persistence.
     */
    PageId get_root_page_id() const { return tree_.get_root_page_id(); }

    /**
     * Get the tokenizer.
     */
    const Tokenizer& tokenizer() const { return tokenizer_; }

private:
    // Serialize a posting list to string
    static std::string serialize_posting_list(const PostingList& list);

    // Deserialize a string to posting list
    static PostingList deserialize_posting_list(const std::string& data);

    // Calculate TF-IDF score
    float calculate_tfidf(uint32_t term_freq, uint32_t doc_freq,
                          uint32_t doc_length) const;

    // Calculate BM25 score
    float calculate_bm25(uint32_t term_freq, uint32_t doc_freq,
                         uint32_t doc_length) const;

    // Merge results from multiple terms
    std::vector<SearchResult> merge_and_results(
        const std::vector<PostingList>& lists) const;

    std::vector<SearchResult> merge_or_results(
        const std::vector<PostingList>& lists) const;

    // Check for phrase match at positions
    bool check_phrase_match(const std::vector<PostingList>& lists,
                            FileId doc_id) const;

    // Normalize a term using the tokenizer
    std::string normalize_term(const std::string& term) const;

    // Parse query string into components
    struct QueryComponent {
        enum class Type { TERM, PHRASE, REQUIRED, EXCLUDED };
        Type type;
        std::string value;
    };
    std::vector<QueryComponent> parse_query(const std::string& query) const;

    BPlusTree tree_;
    Tokenizer tokenizer_;
    InvertedIndexConfig config_;

    // Statistics (stored separately)
    size_t document_count_ = 0;
    uint64_t total_document_length_ = 0;

    // Document length cache (doc_id -> length in tokens)
    mutable std::map<FileId, uint32_t> doc_lengths_;
};

}  // namespace dam::search
