#include <dam/search/embedder.hpp>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdlib>
#include <mutex>
#include <numeric>
#include <sstream>

#ifdef DAM_HAS_LLAMACPP
#include <llama.h>
#endif

namespace dam::search {

using json = nlohmann::json;

// ============================================================================
// Static Utility Functions
// ============================================================================

float Embedder::cosine_similarity(const Embedding& a, const Embedding& b) {
    if (a.size() != b.size() || a.empty()) {
        return 0.0f;
    }

    float dot = 0.0f;
    float norm_a = 0.0f;
    float norm_b = 0.0f;

    for (size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
        norm_a += a[i] * a[i];
        norm_b += b[i] * b[i];
    }

    if (norm_a == 0.0f || norm_b == 0.0f) {
        return 0.0f;
    }

    return dot / (std::sqrt(norm_a) * std::sqrt(norm_b));
}

float Embedder::euclidean_distance(const Embedding& a, const Embedding& b) {
    if (a.size() != b.size()) {
        return std::numeric_limits<float>::max();
    }

    float sum = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) {
        float diff = a[i] - b[i];
        sum += diff * diff;
    }

    return std::sqrt(sum);
}

void Embedder::normalize(Embedding& embedding) {
    float norm = 0.0f;
    for (float v : embedding) {
        norm += v * v;
    }

    if (norm > 0.0f) {
        norm = std::sqrt(norm);
        for (float& v : embedding) {
            v /= norm;
        }
    }
}

// ============================================================================
// Ollama Embedder Implementation
// ============================================================================

namespace {

struct CurlResponseBuffer {
    std::string data;
};

size_t curl_write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buffer = static_cast<CurlResponseBuffer*>(userdata);
    size_t total_size = size * nmemb;
    buffer->data.append(ptr, total_size);
    return total_size;
}

}  // namespace

OllamaEmbedder::OllamaEmbedder(OllamaEmbedderConfig config)
    : config_(std::move(config)) {}

OllamaEmbedder::~OllamaEmbedder() {
    cleanup_curl();
}

OllamaEmbedder::OllamaEmbedder(OllamaEmbedder&& other) noexcept
    : config_(std::move(other.config_))
    , curl_handle_(other.curl_handle_)
    , dimension_(other.dimension_)
    , initialized_(other.initialized_) {
    other.curl_handle_ = nullptr;
    other.initialized_ = false;
}

OllamaEmbedder& OllamaEmbedder::operator=(OllamaEmbedder&& other) noexcept {
    if (this != &other) {
        cleanup_curl();
        config_ = std::move(other.config_);
        curl_handle_ = other.curl_handle_;
        dimension_ = other.dimension_;
        initialized_ = other.initialized_;
        other.curl_handle_ = nullptr;
        other.initialized_ = false;
    }
    return *this;
}

void OllamaEmbedder::init_curl() {
    static std::once_flag curl_init_flag;
    std::call_once(curl_init_flag, []() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    });
    curl_handle_ = curl_easy_init();
}

void OllamaEmbedder::cleanup_curl() {
    if (curl_handle_) {
        curl_easy_cleanup(static_cast<CURL*>(curl_handle_));
        curl_handle_ = nullptr;
    }
}

Result<void> OllamaEmbedder::initialize() {
    init_curl();
    if (!curl_handle_) {
        return Error(ErrorCode::INTERNAL_ERROR, "Failed to initialize CURL");
    }

    // Test connection and get dimension
    auto test_result = embed("test");
    if (!test_result.ok()) {
        return test_result.error();
    }

    dimension_ = static_cast<int>(test_result->size());
    initialized_ = true;
    return {};
}

bool OllamaEmbedder::is_available() const {
    return initialized_ && curl_handle_ != nullptr;
}

Result<Embedding> OllamaEmbedder::embed(const std::string& text) {
    if (!curl_handle_) {
        return Error(ErrorCode::INVALID_ARGUMENT, "Embedder not initialized");
    }

    CURL* curl = static_cast<CURL*>(curl_handle_);
    curl_easy_reset(curl);

    // Build request
    json body;
    body["model"] = config_.model;
    body["prompt"] = text;
    std::string request_body = body.dump();

    std::string url = config_.base_url + "/api/embeddings";

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    CurlResponseBuffer response;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(config_.timeout_ms));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        return Error(ErrorCode::IO_ERROR,
            std::string("Network error: ") + curl_easy_strerror(res));
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (http_code >= 400) {
        return Error(ErrorCode::IO_ERROR,
            "HTTP error " + std::to_string(http_code));
    }

    // Parse response
    try {
        auto j = json::parse(response.data);
        if (!j.contains("embedding")) {
            return Error(ErrorCode::CORRUPTION, "Response missing 'embedding' field");
        }

        Embedding embedding = j["embedding"].get<std::vector<float>>();

        if (config_.normalize) {
            Embedder::normalize(embedding);
        }

        return embedding;
    } catch (const json::exception& e) {
        return Error(ErrorCode::CORRUPTION,
            std::string("Failed to parse response: ") + e.what());
    }
}

Result<std::vector<Embedding>> OllamaEmbedder::embed_batch(
    const std::vector<std::string>& texts,
    EmbedProgressCallback callback) {

    std::vector<Embedding> results;
    results.reserve(texts.size());

    for (size_t i = 0; i < texts.size(); ++i) {
        auto result = embed(texts[i]);
        if (!result.ok()) {
            return result.error();
        }
        results.push_back(std::move(result.value()));

        if (callback) {
            callback(i + 1, texts.size());
        }
    }

    return results;
}

Result<EmbedderPtr> OllamaEmbedder::create(OllamaEmbedderConfig config) {
    auto embedder = std::make_unique<OllamaEmbedder>(std::move(config));
    auto init_result = embedder->initialize();
    if (!init_result.ok()) {
        return init_result.error();
    }
    return EmbedderPtr(std::move(embedder));
}

Result<EmbedderPtr> OllamaEmbedder::create_default() {
    return create(OllamaEmbedderConfig{});
}

Result<EmbedderPtr> OllamaEmbedder::create_from_env() {
    OllamaEmbedderConfig config;

    if (const char* url = std::getenv("DAM_OLLAMA_URL")) {
        config.base_url = url;
    }
    if (const char* model = std::getenv("DAM_OLLAMA_EMBED_MODEL")) {
        config.model = model;
    }

    return create(std::move(config));
}

// ============================================================================
// LlamaCpp Embedder Implementation
// ============================================================================

#ifdef DAM_HAS_LLAMACPP

LlamaCppEmbedder::LlamaCppEmbedder(LlamaCppEmbedderConfig config)
    : config_(std::move(config)) {}

LlamaCppEmbedder::~LlamaCppEmbedder() {
    shutdown();
}

LlamaCppEmbedder::LlamaCppEmbedder(LlamaCppEmbedder&& other) noexcept
    : config_(std::move(other.config_))
    , model_(other.model_)
    , context_(other.context_)
    , dimension_(other.dimension_)
    , model_name_(std::move(other.model_name_))
    , initialized_(other.initialized_) {
    other.model_ = nullptr;
    other.context_ = nullptr;
    other.initialized_ = false;
}

LlamaCppEmbedder& LlamaCppEmbedder::operator=(LlamaCppEmbedder&& other) noexcept {
    if (this != &other) {
        shutdown();
        config_ = std::move(other.config_);
        model_ = other.model_;
        context_ = other.context_;
        dimension_ = other.dimension_;
        model_name_ = std::move(other.model_name_);
        initialized_ = other.initialized_;
        other.model_ = nullptr;
        other.context_ = nullptr;
        other.initialized_ = false;
    }
    return *this;
}

Result<void> LlamaCppEmbedder::initialize() {
    if (config_.model_path.empty()) {
        return Error(ErrorCode::INVALID_ARGUMENT, "Model path is required");
    }

    // Initialize llama backend
    llama_backend_init();

    // Load model
    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = config_.gpu_layers;

    model_ = llama_load_model_from_file(config_.model_path.c_str(), model_params);
    if (!model_) {
        return Error(ErrorCode::IO_ERROR,
            "Failed to load embedding model: " + config_.model_path);
    }

    // Get model name from path
    size_t last_slash = config_.model_path.find_last_of("/\\");
    model_name_ = (last_slash != std::string::npos)
        ? config_.model_path.substr(last_slash + 1)
        : config_.model_path;

    // Create context
    llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = config_.context_size;
    ctx_params.n_batch = config_.batch_size;
    ctx_params.n_threads = config_.num_threads;
    ctx_params.embeddings = true;  // Enable embedding mode

    context_ = llama_new_context_with_model(
        static_cast<llama_model*>(model_), ctx_params);

    if (!context_) {
        llama_free_model(static_cast<llama_model*>(model_));
        model_ = nullptr;
        return Error(ErrorCode::INTERNAL_ERROR, "Failed to create embedding context");
    }

    // Get embedding dimension
    dimension_ = llama_n_embd(static_cast<llama_model*>(model_));

    initialized_ = true;
    return {};
}

void LlamaCppEmbedder::shutdown() {
    if (context_) {
        llama_free(static_cast<llama_context*>(context_));
        context_ = nullptr;
    }
    if (model_) {
        llama_free_model(static_cast<llama_model*>(model_));
        model_ = nullptr;
    }
    initialized_ = false;
}

Result<Embedding> LlamaCppEmbedder::embed(const std::string& text) {
    if (!initialized_) {
        return Error(ErrorCode::INVALID_ARGUMENT, "Embedder not initialized");
    }

    auto* model = static_cast<llama_model*>(model_);
    auto* ctx = static_cast<llama_context*>(context_);
    const auto* vocab = llama_model_get_vocab(model);

    // Tokenize
    std::vector<llama_token> tokens(config_.context_size);
    int n_tokens = llama_tokenize(
        vocab,
        text.c_str(),
        static_cast<int>(text.size()),
        tokens.data(),
        static_cast<int>(tokens.size()),
        true,   // add_special
        false   // parse_special
    );

    if (n_tokens < 0) {
        return Error(ErrorCode::INVALID_ARGUMENT, "Text too long for context");
    }
    tokens.resize(n_tokens);

    // Clear KV cache
    llama_kv_cache_clear(ctx);

    // Create batch
    llama_batch batch = llama_batch_init(n_tokens, 0, 1);

    for (int i = 0; i < n_tokens; ++i) {
        batch.token[i] = tokens[i];
        batch.pos[i] = i;
        batch.n_seq_id[i] = 1;
        batch.seq_id[i][0] = 0;
        batch.logits[i] = false;
    }
    batch.n_tokens = n_tokens;

    // Mark last token for embedding output
    batch.logits[n_tokens - 1] = true;

    // Decode
    if (llama_decode(ctx, batch) != 0) {
        llama_batch_free(batch);
        return Error(ErrorCode::INTERNAL_ERROR, "Failed to compute embedding");
    }

    // Get embedding
    float* emb = llama_get_embeddings_seq(ctx, 0);
    if (!emb) {
        // Try getting from last token
        emb = llama_get_embeddings_ith(ctx, n_tokens - 1);
    }

    if (!emb) {
        llama_batch_free(batch);
        return Error(ErrorCode::INTERNAL_ERROR, "Failed to get embedding output");
    }

    Embedding result(emb, emb + dimension_);
    llama_batch_free(batch);

    if (config_.normalize) {
        Embedder::normalize(result);
    }

    return result;
}

Result<std::vector<Embedding>> LlamaCppEmbedder::embed_batch(
    const std::vector<std::string>& texts,
    EmbedProgressCallback callback) {

    std::vector<Embedding> results;
    results.reserve(texts.size());

    for (size_t i = 0; i < texts.size(); ++i) {
        auto result = embed(texts[i]);
        if (!result.ok()) {
            return result.error();
        }
        results.push_back(std::move(result.value()));

        if (callback) {
            callback(i + 1, texts.size());
        }
    }

    return results;
}

Result<EmbedderPtr> LlamaCppEmbedder::create(LlamaCppEmbedderConfig config) {
    auto embedder = std::make_unique<LlamaCppEmbedder>(std::move(config));
    auto init_result = embedder->initialize();
    if (!init_result.ok()) {
        return init_result.error();
    }
    return EmbedderPtr(std::move(embedder));
}

Result<EmbedderPtr> LlamaCppEmbedder::create_from_env() {
    const char* model_path = std::getenv("DAM_EMBEDDING_MODEL_PATH");
    if (!model_path || model_path[0] == '\0') {
        return Error(ErrorCode::NOT_FOUND,
            "DAM_EMBEDDING_MODEL_PATH environment variable not set");
    }

    LlamaCppEmbedderConfig config;
    config.model_path = model_path;

    if (const char* gpu = std::getenv("DAM_EMBEDDING_GPU_LAYERS")) {
        config.gpu_layers = std::atoi(gpu);
    }
    if (const char* threads = std::getenv("DAM_EMBEDDING_THREADS")) {
        config.num_threads = std::atoi(threads);
    }

    return create(std::move(config));
}

#endif  // DAM_HAS_LLAMACPP

// ============================================================================
// Embedder Factory
// ============================================================================

Result<EmbedderPtr> EmbedderFactory::create_from_env() {
#ifdef DAM_HAS_LLAMACPP
    // Try llama.cpp first if model path is set
    if (std::getenv("DAM_EMBEDDING_MODEL_PATH")) {
        auto result = LlamaCppEmbedder::create_from_env();
        if (result.ok()) {
            return result;
        }
    }
#endif

    // Fall back to Ollama
    auto ollama_result = OllamaEmbedder::create_from_env();
    if (ollama_result.ok()) {
        return ollama_result;
    }

    return Error(ErrorCode::NOT_FOUND,
        "No embedding provider available. Set DAM_EMBEDDING_MODEL_PATH "
        "or ensure Ollama is running with nomic-embed-text.");
}

Result<EmbedderPtr> EmbedderFactory::create(const std::string& type) {
    if (type == "ollama") {
        return OllamaEmbedder::create_default();
    }

#ifdef DAM_HAS_LLAMACPP
    if (type == "llamacpp") {
        return LlamaCppEmbedder::create_from_env();
    }
#endif

    return Error(ErrorCode::INVALID_ARGUMENT,
        "Unknown embedder type: " + type);
}

}  // namespace dam::search
