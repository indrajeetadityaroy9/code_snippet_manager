#pragma once

#include <dam/core_types.hpp>
#include <dam/result.hpp>
#include <dam/search/embedder.hpp>

#include <memory>
#include <string>
#include <vector>

namespace dam::search {

// ============================================================================
// Vector Search Result
// ============================================================================

struct VectorSearchResult {
    FileId doc_id;
    float distance;          // Distance from query (lower = more similar)
    float similarity;        // Cosine similarity (higher = more similar)

    bool operator<(const VectorSearchResult& other) const {
        return similarity > other.similarity;  // Higher similarity = better
    }
};

// ============================================================================
// Vector Index Configuration
// ============================================================================

struct VectorIndexConfig {
    // Embedding dimension (must match embedder)
    int dimension = 768;

    // HNSW parameters
    size_t max_elements = 100000;      // Maximum number of elements
    size_t M = 16;                      // Max connections per node
    size_t ef_construction = 200;       // Construction-time search width
    size_t ef_search = 50;              // Query-time search width

    // Distance metric: "l2" (Euclidean), "ip" (Inner Product), "cosine"
    std::string distance_metric = "cosine";

    // Thread-safety
    bool allow_replace = true;          // Allow updating existing vectors
    int num_threads = 4;                // Threads for batch operations

    // Persistence
    std::string index_path;             // Path to save/load index
};

// ============================================================================
// Vector Index
// ============================================================================

/**
 * VectorIndex - Approximate nearest neighbor search using HNSW.
 *
 * Uses hnswlib for fast vector similarity search. Integrates with
 * Embedder for text-to-vector conversion.
 *
 * Features:
 * - O(log n) approximate nearest neighbor search
 * - Batch insertion with parallelization
 * - Save/load to disk
 * - Support for multiple distance metrics
 */
class VectorIndex {
public:
    /**
     * Create a vector index with configuration.
     *
     * @param config Index configuration
     */
    explicit VectorIndex(VectorIndexConfig config = {});

    ~VectorIndex();

    // Non-copyable
    VectorIndex(const VectorIndex&) = delete;
    VectorIndex& operator=(const VectorIndex&) = delete;

    // Movable
    VectorIndex(VectorIndex&& other) noexcept;
    VectorIndex& operator=(VectorIndex&& other) noexcept;

    // ========================================================================
    // Index Management
    // ========================================================================

    /**
     * Initialize the index. Must be called before use.
     */
    Result<void> initialize();

    /**
     * Check if index is initialized.
     */
    bool is_initialized() const { return initialized_; }

    /**
     * Save index to disk.
     *
     * @param path File path (uses config.index_path if empty)
     */
    Result<void> save(const std::string& path = "") const;

    /**
     * Load index from disk.
     *
     * @param path File path (uses config.index_path if empty)
     */
    Result<void> load(const std::string& path = "");

    /**
     * Clear all vectors from the index.
     */
    void clear();

    /**
     * Resize the index to accommodate more elements.
     *
     * @param new_max_elements New maximum capacity
     */
    Result<void> resize(size_t new_max_elements);

    // ========================================================================
    // Vector Operations
    // ========================================================================

    /**
     * Add a vector to the index.
     *
     * @param doc_id Document identifier (used as label)
     * @param embedding The embedding vector
     * @return Success or error
     */
    Result<void> add(FileId doc_id, const Embedding& embedding);

    /**
     * Add multiple vectors in batch.
     *
     * @param doc_ids Document identifiers
     * @param embeddings Embedding vectors
     * @param callback Progress callback
     * @return Success or error
     */
    Result<void> add_batch(
        const std::vector<FileId>& doc_ids,
        const std::vector<Embedding>& embeddings,
        EmbedProgressCallback callback = nullptr);

    /**
     * Remove a vector from the index.
     * Note: hnswlib doesn't support true deletion, marks as deleted.
     *
     * @param doc_id Document to remove
     * @return Success or error
     */
    Result<void> remove(FileId doc_id);

    /**
     * Update a vector (remove and re-add).
     *
     * @param doc_id Document identifier
     * @param embedding New embedding
     * @return Success or error
     */
    Result<void> update(FileId doc_id, const Embedding& embedding);

    /**
     * Check if a document exists in the index.
     */
    bool contains(FileId doc_id) const;

    // ========================================================================
    // Search Operations
    // ========================================================================

    /**
     * Search for nearest neighbors.
     *
     * @param query Query embedding
     * @param k Number of results to return
     * @return Ranked search results
     */
    Result<std::vector<VectorSearchResult>> search(
        const Embedding& query,
        size_t k = 10) const;

    /**
     * Search with distance threshold.
     *
     * @param query Query embedding
     * @param max_distance Maximum distance to include
     * @param k Maximum number of results
     * @return Filtered search results
     */
    Result<std::vector<VectorSearchResult>> search_threshold(
        const Embedding& query,
        float max_distance,
        size_t k = 100) const;

    /**
     * Search with similarity threshold.
     *
     * @param query Query embedding
     * @param min_similarity Minimum similarity to include (0.0 - 1.0)
     * @param k Maximum number of results
     * @return Filtered search results
     */
    Result<std::vector<VectorSearchResult>> search_similarity(
        const Embedding& query,
        float min_similarity,
        size_t k = 100) const;

    /**
     * Set search quality parameter (ef_search).
     * Higher values = more accurate but slower.
     *
     * @param ef_search New ef_search value
     */
    void set_ef_search(size_t ef_search);

    // ========================================================================
    // Statistics
    // ========================================================================

    /**
     * Get number of vectors in the index.
     */
    size_t size() const;

    /**
     * Get maximum capacity.
     */
    size_t capacity() const { return config_.max_elements; }

    /**
     * Get embedding dimension.
     */
    int dimension() const { return config_.dimension; }

    /**
     * Get configuration.
     */
    const VectorIndexConfig& config() const { return config_; }

    // ========================================================================
    // Factory
    // ========================================================================

    /**
     * Create index with embedder integration.
     *
     * @param embedder Embedder for text-to-vector conversion
     * @param config Index configuration (dimension auto-detected)
     */
    static Result<std::unique_ptr<VectorIndex>> create_with_embedder(
        Embedder* embedder,
        VectorIndexConfig config = {});

private:
    void cleanup();
    float distance_to_similarity(float distance) const;

    VectorIndexConfig config_;
    void* hnsw_index_ = nullptr;  // hnswlib::HierarchicalNSW<float>*
    bool initialized_ = false;
};

// ============================================================================
// Vector Index with Embedder
// ============================================================================

/**
 * VectorIndexWithEmbedder - Convenience wrapper that combines
 * VectorIndex with Embedder for end-to-end text search.
 */
class VectorIndexWithEmbedder {
public:
    VectorIndexWithEmbedder(std::unique_ptr<VectorIndex> index,
                            std::unique_ptr<Embedder> embedder);

    /**
     * Index a text document.
     */
    Result<void> index_text(FileId doc_id, const std::string& text);

    /**
     * Index multiple texts in batch.
     */
    Result<void> index_batch(
        const std::vector<FileId>& doc_ids,
        const std::vector<std::string>& texts,
        EmbedProgressCallback callback = nullptr);

    /**
     * Search by text query.
     */
    Result<std::vector<VectorSearchResult>> search(
        const std::string& query,
        size_t k = 10) const;

    /**
     * Search by text with similarity threshold.
     */
    Result<std::vector<VectorSearchResult>> search_similar(
        const std::string& query,
        float min_similarity,
        size_t k = 100) const;

    /**
     * Access underlying components.
     */
    VectorIndex* index() { return index_.get(); }
    const VectorIndex* index() const { return index_.get(); }
    Embedder* embedder() { return embedder_.get(); }
    const Embedder* embedder() const { return embedder_.get(); }

    /**
     * Create from environment.
     */
    static Result<std::unique_ptr<VectorIndexWithEmbedder>> create_from_env(
        VectorIndexConfig config = {});

private:
    std::unique_ptr<VectorIndex> index_;
    std::unique_ptr<Embedder> embedder_;
};

}  // namespace dam::search
