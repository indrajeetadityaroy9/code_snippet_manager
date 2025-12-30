#pragma once

#include <dam/result.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dam::search {

// ============================================================================
// Embedding Types
// ============================================================================

using Embedding = std::vector<float>;

// Callback for batch embedding progress
using EmbedProgressCallback = std::function<void(size_t completed, size_t total)>;

// ============================================================================
// Embedder Configuration
// ============================================================================

struct EmbedderConfig {
    // Model path for local embedders
    std::string model_path;

    // Embedding dimension (auto-detected if 0)
    int dimension = 0;

    // Batch size for batch operations
    int batch_size = 32;

    // Normalize embeddings to unit vectors
    bool normalize = true;

    // Number of threads for inference
    int num_threads = 4;

    // GPU layers to offload (for llama.cpp)
    int gpu_layers = 0;

    // Context size
    int context_size = 512;
};

// ============================================================================
// Embedder Interface
// ============================================================================

/**
 * Abstract base class for text embedding providers.
 *
 * Implementations:
 * - LlamaCppEmbedder: Uses llama.cpp with dedicated embedding model
 * - OllamaEmbedder: Uses Ollama's /api/embeddings endpoint
 */
class Embedder {
public:
    virtual ~Embedder() = default;

    /**
     * Get the embedding dimension.
     */
    virtual int dimension() const = 0;

    /**
     * Get the model name/identifier.
     */
    virtual std::string model_name() const = 0;

    /**
     * Check if the embedder is ready.
     */
    virtual bool is_available() const = 0;

    /**
     * Embed a single text.
     *
     * @param text The text to embed
     * @return Embedding vector or error
     */
    virtual Result<Embedding> embed(const std::string& text) = 0;

    /**
     * Embed multiple texts in batch.
     *
     * @param texts The texts to embed
     * @param callback Optional progress callback
     * @return Vector of embeddings or error
     */
    virtual Result<std::vector<Embedding>> embed_batch(
        const std::vector<std::string>& texts,
        EmbedProgressCallback callback = nullptr) = 0;

    /**
     * Calculate cosine similarity between two embeddings.
     */
    static float cosine_similarity(const Embedding& a, const Embedding& b);

    /**
     * Calculate Euclidean distance between two embeddings.
     */
    static float euclidean_distance(const Embedding& a, const Embedding& b);

    /**
     * Normalize an embedding to unit length.
     */
    static void normalize(Embedding& embedding);
};

using EmbedderPtr = std::unique_ptr<Embedder>;

// ============================================================================
// llama.cpp Embedder
// ============================================================================

#ifdef DAM_HAS_LLAMACPP

struct LlamaCppEmbedderConfig : EmbedderConfig {
    // Specific llama.cpp options can be added here
};

/**
 * LlamaCppEmbedder - Uses llama.cpp for local text embeddings.
 *
 * Designed for dedicated embedding models like:
 * - nomic-embed-text-v1.5
 * - all-MiniLM-L6-v2
 * - bge-small-en
 */
class LlamaCppEmbedder : public Embedder {
public:
    explicit LlamaCppEmbedder(LlamaCppEmbedderConfig config);
    ~LlamaCppEmbedder() override;

    // Non-copyable, movable
    LlamaCppEmbedder(const LlamaCppEmbedder&) = delete;
    LlamaCppEmbedder& operator=(const LlamaCppEmbedder&) = delete;
    LlamaCppEmbedder(LlamaCppEmbedder&& other) noexcept;
    LlamaCppEmbedder& operator=(LlamaCppEmbedder&& other) noexcept;

    int dimension() const override { return dimension_; }
    std::string model_name() const override { return model_name_; }
    bool is_available() const override { return initialized_; }

    Result<Embedding> embed(const std::string& text) override;
    Result<std::vector<Embedding>> embed_batch(
        const std::vector<std::string>& texts,
        EmbedProgressCallback callback = nullptr) override;

    /**
     * Create from model path.
     */
    static Result<EmbedderPtr> create(LlamaCppEmbedderConfig config);

    /**
     * Create from environment variable DAM_EMBEDDING_MODEL_PATH.
     */
    static Result<EmbedderPtr> create_from_env();

private:
    Result<void> initialize();
    void shutdown();

    LlamaCppEmbedderConfig config_;
    void* model_ = nullptr;      // llama_model*
    void* context_ = nullptr;    // llama_context*
    int dimension_ = 0;
    std::string model_name_;
    bool initialized_ = false;
};

#endif  // DAM_HAS_LLAMACPP

// ============================================================================
// Ollama Embedder
// ============================================================================

struct OllamaEmbedderConfig : EmbedderConfig {
    std::string base_url = "http://localhost:11434";
    std::string model = "nomic-embed-text";
    int timeout_ms = 30000;
};

/**
 * OllamaEmbedder - Uses Ollama's embedding API.
 *
 * Requires Ollama to be running with an embedding model pulled.
 */
class OllamaEmbedder : public Embedder {
public:
    explicit OllamaEmbedder(OllamaEmbedderConfig config);
    ~OllamaEmbedder() override;

    OllamaEmbedder(const OllamaEmbedder&) = delete;
    OllamaEmbedder& operator=(const OllamaEmbedder&) = delete;
    OllamaEmbedder(OllamaEmbedder&& other) noexcept;
    OllamaEmbedder& operator=(OllamaEmbedder&& other) noexcept;

    int dimension() const override { return dimension_; }
    std::string model_name() const override { return config_.model; }
    bool is_available() const override;

    Result<Embedding> embed(const std::string& text) override;
    Result<std::vector<Embedding>> embed_batch(
        const std::vector<std::string>& texts,
        EmbedProgressCallback callback = nullptr) override;

    /**
     * Create with config.
     */
    static Result<EmbedderPtr> create(OllamaEmbedderConfig config);

    /**
     * Create with default settings (localhost:11434, nomic-embed-text).
     */
    static Result<EmbedderPtr> create_default();

    /**
     * Create from environment.
     */
    static Result<EmbedderPtr> create_from_env();

private:
    Result<void> initialize();
    void init_curl();
    void cleanup_curl();

    OllamaEmbedderConfig config_;
    void* curl_handle_ = nullptr;
    int dimension_ = 0;
    bool initialized_ = false;
};

// ============================================================================
// Embedder Factory
// ============================================================================

class EmbedderFactory {
public:
    /**
     * Create the best available embedder from environment.
     *
     * Priority:
     * 1. LlamaCpp if DAM_EMBEDDING_MODEL_PATH is set
     * 2. Ollama if running
     */
    static Result<EmbedderPtr> create_from_env();

    /**
     * Create embedder by type name.
     *
     * @param type "llamacpp" or "ollama"
     */
    static Result<EmbedderPtr> create(const std::string& type);
};

}  // namespace dam::search
