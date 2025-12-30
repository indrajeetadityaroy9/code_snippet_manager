#pragma once

#include <dam/core_types.hpp>
#include <dam/result.hpp>
#include <dam/storage/btree.hpp>

#include <map>
#include <set>
#include <string>
#include <vector>

namespace dam::search {

// ============================================================================
// Trigram Index Configuration
// ============================================================================

struct TrigramIndexConfig {
    // Use padding for prefix/suffix matching
    // "abc" -> {"$$a", "$ab", "abc", "bc$", "c$$"} with padding
    // "abc" -> {"abc"} without padding
    bool use_padding = true;
    char padding_char = '$';

    // Minimum similarity threshold for fuzzy search (0.0 - 1.0)
    float default_similarity_threshold = 0.3f;

    // Maximum results to return
    size_t max_results = 100;

    // Case insensitive matching
    bool case_insensitive = true;
};

// ============================================================================
// Fuzzy Search Result
// ============================================================================

struct FuzzyResult {
    FileId doc_id;
    float similarity;           // Jaccard similarity (0.0 - 1.0)
    std::string matched_text;   // The text that matched

    bool operator<(const FuzzyResult& other) const {
        return similarity > other.similarity;  // Higher similarity = better
    }
};

// ============================================================================
// Trigram Index
// ============================================================================

/**
 * TrigramIndex - Enables fuzzy and substring search using trigram matching.
 *
 * A trigram is a sequence of 3 consecutive characters. By indexing all
 * trigrams in a document, we can find approximate matches efficiently.
 *
 * Features:
 * - Substring search: Find documents containing a pattern
 * - Fuzzy search: Find documents with similar text (typo-tolerant)
 * - Jaccard similarity scoring
 */
class TrigramIndex {
public:
    /**
     * Create a trigram index backed by a B+ tree.
     *
     * @param buffer_pool The buffer pool for page management
     * @param root_page_id Root page ID (INVALID_PAGE_ID to create new)
     * @param config Index configuration
     */
    TrigramIndex(BufferPool* buffer_pool,
                 PageId root_page_id = INVALID_PAGE_ID,
                 TrigramIndexConfig config = {});

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
     * Index a specific field/text with an identifier.
     * Useful for indexing filenames, tags, etc. separately from content.
     *
     * @param doc_id The document identifier
     * @param text The text to index
     * @param field_id Optional field identifier (default 0)
     * @return Success or error
     */
    Result<void> index_text(FileId doc_id, const std::string& text, uint32_t field_id = 0);

    /**
     * Remove a document from the index.
     *
     * @param doc_id The document to remove
     * @param content The document's content (needed for trigram lookup)
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
     * Search for documents containing a substring.
     *
     * @param pattern The pattern to search for
     * @return Document IDs that likely contain the pattern
     */
    Result<std::vector<FileId>> search_substring(const std::string& pattern) const;

    /**
     * Fuzzy search with similarity threshold.
     *
     * @param query The query string
     * @param threshold Minimum Jaccard similarity (0.0 - 1.0)
     * @return Ranked fuzzy results
     */
    Result<std::vector<FuzzyResult>> search_fuzzy(
        const std::string& query,
        float threshold = -1.0f) const;

    /**
     * Find similar strings to query.
     * Useful for "did you mean?" suggestions.
     *
     * @param query The query string
     * @param max_results Maximum number of suggestions
     * @return Similar strings with their similarity scores
     */
    Result<std::vector<FuzzyResult>> find_similar(
        const std::string& query,
        size_t max_results = 10) const;

    /**
     * Check if a document likely contains a pattern.
     * Fast pre-filter before exact matching.
     *
     * @param doc_id The document to check
     * @param pattern The pattern to look for
     * @return true if the document might contain the pattern
     */
    bool may_contain(FileId doc_id, const std::string& pattern) const;

    // ========================================================================
    // Trigram Utilities
    // ========================================================================

    /**
     * Extract trigrams from a string.
     *
     * @param s The input string
     * @param use_padding Whether to add padding for prefix/suffix matching
     * @return Set of trigrams
     */
    std::set<std::string> extract_trigrams(const std::string& s,
                                           bool use_padding = true) const;

    /**
     * Calculate Jaccard similarity between two strings.
     *
     * @param a First string
     * @param b Second string
     * @return Similarity score (0.0 - 1.0)
     */
    float jaccard_similarity(const std::string& a, const std::string& b) const;

    /**
     * Calculate Jaccard similarity between trigram sets.
     *
     * @param set_a First trigram set
     * @param set_b Second trigram set
     * @return Similarity score (0.0 - 1.0)
     */
    static float jaccard_similarity(const std::set<std::string>& set_a,
                                    const std::set<std::string>& set_b);

    // ========================================================================
    // Statistics
    // ========================================================================

    /**
     * Get total number of indexed documents.
     */
    size_t document_count() const { return document_count_; }

    /**
     * Get total number of unique trigrams.
     */
    size_t trigram_count() const { return tree_.size(); }

    /**
     * Get number of documents containing a specific trigram.
     */
    size_t get_trigram_frequency(const std::string& trigram) const;

    /**
     * Get all unique trigrams.
     */
    std::vector<std::string> get_all_trigrams() const;

    /**
     * Get root page ID for persistence.
     */
    PageId get_root_page_id() const { return tree_.get_root_page_id(); }

private:
    // Normalize a string for indexing
    std::string normalize(const std::string& s) const;

    // Get documents for a trigram
    std::set<FileId> get_documents_for_trigram(const std::string& trigram) const;

    // Add document to trigram posting list
    Result<void> add_to_trigram(const std::string& trigram, FileId doc_id);

    // Remove document from trigram posting list
    Result<void> remove_from_trigram(const std::string& trigram, FileId doc_id);

    // Serialize/deserialize document sets
    static std::string serialize_doc_ids(const std::set<FileId>& ids);
    static std::set<FileId> deserialize_doc_ids(const std::string& data);

    BPlusTree tree_;
    TrigramIndexConfig config_;
    size_t document_count_ = 0;

    // Cache: trigram -> document count (for IDF weighting)
    mutable std::map<std::string, size_t> trigram_doc_counts_;
};

}  // namespace dam::search
