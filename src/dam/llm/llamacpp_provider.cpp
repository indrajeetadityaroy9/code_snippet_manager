#include <dam/llm/llamacpp_provider.hpp>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>

#ifdef DAM_HAS_LLAMACPP

namespace dam::llm {

LlamaCppProvider::LlamaCppProvider(LlamaCppConfig config)
    : config_(std::move(config)) {}

LlamaCppProvider::~LlamaCppProvider() {
    shutdown();
}

LlamaCppProvider::LlamaCppProvider(LlamaCppProvider&& other) noexcept
    : config_(std::move(other.config_))
    , model_(other.model_)
    , context_(other.context_)
    , sampler_(other.sampler_)
    , initialized_(other.initialized_.load())
    , abort_requested_(other.abort_requested_.load()) {
    other.model_ = nullptr;
    other.context_ = nullptr;
    other.sampler_ = nullptr;
    other.initialized_ = false;
}

LlamaCppProvider& LlamaCppProvider::operator=(LlamaCppProvider&& other) noexcept {
    if (this != &other) {
        shutdown();
        config_ = std::move(other.config_);
        model_ = other.model_;
        context_ = other.context_;
        sampler_ = other.sampler_;
        initialized_ = other.initialized_.load();
        abort_requested_ = other.abort_requested_.load();
        other.model_ = nullptr;
        other.context_ = nullptr;
        other.sampler_ = nullptr;
        other.initialized_ = false;
    }
    return *this;
}

ProviderInfo LlamaCppProvider::info() const {
    ProviderInfo pinfo;
    pinfo.name = "llama.cpp";
    pinfo.model_id = config_.model_path;
    pinfo.is_local = true;
    pinfo.supports_streaming = true;
    pinfo.context_length = config_.n_ctx;

    if (model_) {
        pinfo.model_size_bytes = llama_model_size(model_);
    }

    return pinfo;
}

Result<void> LlamaCppProvider::initialize() {
    if (initialized_) {
        return {};
    }

    std::lock_guard<std::mutex> lock(inference_mutex_);

    // Initialize backend
    llama_backend_init();

    // Load model
    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = config_.n_gpu_layers;
    model_params.main_gpu = config_.main_gpu;
    model_params.use_mmap = config_.use_mmap;
    model_params.use_mlock = config_.use_mlock;

    model_ = llama_load_model_from_file(config_.model_path.c_str(), model_params);
    if (!model_) {
        return Error(ErrorCode::IO_ERROR,
            "Failed to load model: " + config_.model_path);
    }

    // Create context
    llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = config_.n_ctx;
    ctx_params.n_batch = config_.n_batch;
    ctx_params.n_threads = config_.n_threads;
    ctx_params.n_threads_batch = config_.n_threads_batch;
    ctx_params.flash_attn = config_.flash_attn;

    context_ = llama_new_context_with_model(model_, ctx_params);
    if (!context_) {
        llama_free_model(model_);
        model_ = nullptr;
        return Error(ErrorCode::INTERNAL_ERROR, "Failed to create context");
    }

    // Create sampler chain
    sampler_ = llama_sampler_chain_init(llama_sampler_chain_default_params());

    initialized_ = true;
    return {};
}

void LlamaCppProvider::shutdown() {
    std::lock_guard<std::mutex> lock(inference_mutex_);

    if (sampler_) {
        llama_sampler_free(sampler_);
        sampler_ = nullptr;
    }

    if (context_) {
        llama_free(context_);
        context_ = nullptr;
    }

    if (model_) {
        llama_free_model(model_);
        model_ = nullptr;
    }

    initialized_ = false;
}

bool LlamaCppProvider::is_available() const {
    return initialized_ && model_ != nullptr && context_ != nullptr;
}

Result<CompletionResult> LlamaCppProvider::complete(const CompletionRequest& request) {
    if (!is_available()) {
        return Error(ErrorCode::INVALID_ARGUMENT, "Provider not initialized");
    }

    std::lock_guard<std::mutex> lock(inference_mutex_);
    reset_abort();

    auto start_time = std::chrono::steady_clock::now();
    CompletionResult result;

    // Build and tokenize prompt
    std::string prompt = build_prompt(request);
    std::vector<llama_token> tokens = tokenize(prompt, true);

    if (tokens.empty()) {
        return Error(ErrorCode::INVALID_ARGUMENT, "Empty prompt");
    }

    result.prompt_tokens = static_cast<int>(tokens.size());

    // Check context length
    if (tokens.size() > static_cast<size_t>(config_.n_ctx)) {
        return Error(ErrorCode::INVALID_ARGUMENT,
            "Prompt exceeds context length: " + std::to_string(tokens.size()) +
            " > " + std::to_string(config_.n_ctx));
    }

    // Clear KV cache
    llama_kv_cache_clear(context_);

    // Process prompt in batches
    llama_batch batch = llama_batch_init(config_.n_batch, 0, 1);

    for (size_t i = 0; i < tokens.size(); i += config_.n_batch) {
        if (abort_requested_) {
            llama_batch_free(batch);
            result.stop_reason = "aborted";
            return result;
        }

        size_t n_tokens = std::min(static_cast<size_t>(config_.n_batch),
                                   tokens.size() - i);

        // Clear batch
        batch.n_tokens = 0;
        // Add tokens to batch
        for (size_t j = 0; j < n_tokens; ++j) {
            batch.token[batch.n_tokens] = tokens[i + j];
            batch.pos[batch.n_tokens] = static_cast<llama_pos>(i + j);
            batch.n_seq_id[batch.n_tokens] = 1;
            batch.seq_id[batch.n_tokens][0] = 0;
            batch.logits[batch.n_tokens] = false;
            batch.n_tokens++;
        }

        // Mark last token for logits
        if (i + n_tokens == tokens.size()) {
            batch.logits[batch.n_tokens - 1] = true;
        }

        if (llama_decode(context_, batch) != 0) {
            llama_batch_free(batch);
            return Error(ErrorCode::INTERNAL_ERROR, "Failed to decode prompt");
        }
    }

    // Setup sampler for generation
    setup_sampler(request);

    // Generate tokens
    int n_cur = static_cast<int>(tokens.size());

    while (result.completion_tokens < request.max_tokens) {
        if (abort_requested_) {
            result.stop_reason = "aborted";
            break;
        }

        // Sample next token
        llama_token new_token = llama_sampler_sample(sampler_, context_, -1);

        // Check for EOS
        const auto* vocab = llama_model_get_vocab(model_);
        if (llama_vocab_is_eog(vocab, new_token)) {
            result.stop_reason = "eos";
            break;
        }

        // Decode token to text
        std::string piece = detokenize({new_token});
        result.content += piece;
        result.completion_tokens++;

        // Check stop sequences
        bool should_stop = false;
        for (const auto& stop : request.stop_sequences) {
            if (result.content.size() >= stop.size() &&
                result.content.substr(result.content.size() - stop.size()) == stop) {
                // Remove stop sequence from output
                result.content = result.content.substr(0,
                    result.content.size() - stop.size());
                result.stop_reason = "stop_sequence";
                should_stop = true;
                break;
            }
        }
        if (should_stop) break;

        // Stream callback
        if (request.on_chunk && !request.on_chunk(piece)) {
            result.stop_reason = "aborted";
            break;
        }

        // Prepare next batch
        batch.n_tokens = 0;
        batch.token[batch.n_tokens] = new_token;
        batch.pos[batch.n_tokens] = n_cur;
        batch.n_seq_id[batch.n_tokens] = 1;
        batch.seq_id[batch.n_tokens][0] = 0;
        batch.logits[batch.n_tokens] = true;
        batch.n_tokens++;
        n_cur++;

        if (llama_decode(context_, batch) != 0) {
            llama_batch_free(batch);
            return Error(ErrorCode::INTERNAL_ERROR, "Failed to decode token");
        }
    }

    llama_batch_free(batch);

    if (result.stop_reason.empty()) {
        result.stop_reason = "max_tokens";
    }

    // Calculate metrics
    auto end_time = std::chrono::steady_clock::now();
    result.latency_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time).count());

    if (result.latency_ms > 0 && result.completion_tokens > 0) {
        result.tokens_per_second =
            static_cast<float>(result.completion_tokens) /
            (static_cast<float>(result.latency_ms) / 1000.0f);
    }

    return result;
}

void LlamaCppProvider::abort() {
    abort_requested_ = true;
}

bool LlamaCppProvider::is_aborted() const {
    return abort_requested_;
}

void LlamaCppProvider::reset_abort() {
    abort_requested_ = false;
}

std::vector<int32_t> LlamaCppProvider::tokenize(const std::string& text, bool add_bos) {
    const auto* vocab = llama_model_get_vocab(model_);
    int n_tokens = static_cast<int>(text.size()) + (add_bos ? 1 : 0);
    std::vector<llama_token> tokens(n_tokens);

    n_tokens = llama_tokenize(vocab, text.c_str(), static_cast<int>(text.size()),
                              tokens.data(), static_cast<int>(tokens.size()),
                              add_bos, true);

    if (n_tokens < 0) {
        tokens.resize(-n_tokens);
        n_tokens = llama_tokenize(vocab, text.c_str(), static_cast<int>(text.size()),
                                  tokens.data(), static_cast<int>(tokens.size()),
                                  add_bos, true);
    }

    tokens.resize(n_tokens);
    return tokens;
}

std::string LlamaCppProvider::detokenize(const std::vector<int32_t>& tokens) {
    const auto* vocab = llama_model_get_vocab(model_);
    std::string result;
    for (llama_token token : tokens) {
        char buf[256];
        int n = llama_token_to_piece(vocab, token, buf, sizeof(buf), 0, true);
        if (n > 0) {
            result.append(buf, n);
        }
    }
    return result;
}

std::string LlamaCppProvider::build_prompt(const CompletionRequest& request) {
    std::ostringstream ss;

    // Use simple chat template compatible with most models
    if (!request.system_prompt.empty()) {
        ss << "<|system|>\n" << request.system_prompt << "\n";
    }

    for (const auto& msg : request.messages) {
        switch (msg.role) {
            case Message::Role::SYSTEM:
                ss << "<|system|>\n" << msg.content << "\n";
                break;
            case Message::Role::USER:
                ss << "<|user|>\n" << msg.content << "\n";
                break;
            case Message::Role::ASSISTANT:
                ss << "<|assistant|>\n" << msg.content << "\n";
                break;
        }
    }

    ss << "<|assistant|>\n";
    return ss.str();
}

void LlamaCppProvider::setup_sampler(const CompletionRequest& request) {
    // Reset sampler chain
    llama_sampler_free(sampler_);
    sampler_ = llama_sampler_chain_init(llama_sampler_chain_default_params());

    // Add samplers in order
    llama_sampler_chain_add(sampler_,
        llama_sampler_init_top_k(request.top_k));
    llama_sampler_chain_add(sampler_,
        llama_sampler_init_top_p(request.top_p, 1));
    llama_sampler_chain_add(sampler_,
        llama_sampler_init_temp(request.temperature));
    llama_sampler_chain_add(sampler_,
        llama_sampler_init_dist(LLAMA_DEFAULT_SEED));
}

Result<void> LlamaCppProvider::warmup() {
    if (!is_available()) {
        return Error(ErrorCode::INVALID_ARGUMENT, "Provider not initialized");
    }

    // Run a minimal generation to warm up the model
    CompletionRequest warmup_req;
    warmup_req.messages = {Message::user("Hi")};
    warmup_req.max_tokens = 1;

    auto result = complete(warmup_req);
    if (!result.ok()) {
        return result.error();
    }

    return {};
}

size_t LlamaCppProvider::memory_usage() const {
    if (!model_) return 0;
    return llama_model_size(model_);
}

Result<std::unique_ptr<LlamaCppProvider>> LlamaCppProvider::create(LlamaCppConfig config) {
    auto provider = std::make_unique<LlamaCppProvider>(std::move(config));
    auto init_result = provider->initialize();
    if (!init_result.ok()) {
        return init_result.error();
    }
    return provider;
}

Result<std::unique_ptr<LlamaCppProvider>> LlamaCppProvider::create_from_env() {
    LlamaCppConfig config;

    if (const char* path = std::getenv("DAM_LLAMACPP_MODEL_PATH")) {
        config.model_path = path;
    } else {
        return Error(ErrorCode::INVALID_ARGUMENT,
            "DAM_LLAMACPP_MODEL_PATH environment variable not set");
    }

    if (const char* ctx = std::getenv("DAM_LLAMACPP_CTX_SIZE")) {
        config.n_ctx = std::atoi(ctx);
    }

    if (const char* threads = std::getenv("DAM_LLAMACPP_THREADS")) {
        config.n_threads = std::atoi(threads);
        config.n_threads_batch = config.n_threads;
    }

    if (const char* gpu = std::getenv("DAM_LLAMACPP_GPU_LAYERS")) {
        config.n_gpu_layers = std::atoi(gpu);
    }

    return create(std::move(config));
}

}  // namespace dam::llm

#else  // !DAM_HAS_LLAMACPP

// Stub implementation when llama.cpp is not available
namespace dam::llm {

LlamaCppProvider::LlamaCppProvider(LlamaCppConfig config)
    : config_(std::move(config)) {}

LlamaCppProvider::~LlamaCppProvider() = default;

LlamaCppProvider::LlamaCppProvider(LlamaCppProvider&& other) noexcept
    : config_(std::move(other.config_)) {}

LlamaCppProvider& LlamaCppProvider::operator=(LlamaCppProvider&& other) noexcept {
    config_ = std::move(other.config_);
    return *this;
}

ProviderInfo LlamaCppProvider::info() const {
    return {"llama.cpp", config_.model_path, true, true, 0, 0};
}

Result<void> LlamaCppProvider::initialize() {
    return Error(ErrorCode::INTERNAL_ERROR,
        "llama.cpp support not compiled. Rebuild with -DDAM_ENABLE_LLAMACPP=ON");
}

void LlamaCppProvider::shutdown() {}
bool LlamaCppProvider::is_available() const { return false; }

Result<CompletionResult> LlamaCppProvider::complete(const CompletionRequest&) {
    return Error(ErrorCode::INTERNAL_ERROR, "llama.cpp not available");
}

void LlamaCppProvider::abort() {}
bool LlamaCppProvider::is_aborted() const { return false; }
void LlamaCppProvider::reset_abort() {}

Result<void> LlamaCppProvider::warmup() {
    return Error(ErrorCode::INTERNAL_ERROR, "llama.cpp not available");
}

size_t LlamaCppProvider::memory_usage() const { return 0; }

std::vector<int32_t> LlamaCppProvider::tokenize(const std::string&, bool) {
    return {};
}

std::string LlamaCppProvider::detokenize(const std::vector<int32_t>&) {
    return "";
}

std::string LlamaCppProvider::build_prompt(const CompletionRequest&) {
    return "";
}

void LlamaCppProvider::setup_sampler(const CompletionRequest&) {}

Result<std::unique_ptr<LlamaCppProvider>> LlamaCppProvider::create(LlamaCppConfig) {
    return Error(ErrorCode::INTERNAL_ERROR, "llama.cpp not available");
}

Result<std::unique_ptr<LlamaCppProvider>> LlamaCppProvider::create_from_env() {
    return Error(ErrorCode::INTERNAL_ERROR, "llama.cpp not available");
}

}  // namespace dam::llm

#endif  // DAM_HAS_LLAMACPP
