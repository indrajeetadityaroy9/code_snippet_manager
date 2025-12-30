#include <dam/llm/router.hpp>

#include <algorithm>
#include <functional>
#include <limits>
#include <sstream>
#include <thread>

namespace dam::llm {

LLMRouter::LLMRouter(LLMRouterConfig config)
    : config_(std::move(config)) {}

LLMRouter::~LLMRouter() {
    abort();
}

void LLMRouter::add_local_provider(ProviderPtr provider) {
    std::lock_guard<std::mutex> lock(providers_mutex_);
    local_providers_.push_back(std::move(provider));
}

void LLMRouter::add_cloud_provider(ProviderPtr provider) {
    std::lock_guard<std::mutex> lock(providers_mutex_);
    cloud_providers_.push_back(std::move(provider));
}

void LLMRouter::remove_provider(const std::string& name) {
    std::lock_guard<std::mutex> lock(providers_mutex_);

    auto remove_from = [&name](std::vector<ProviderPtr>& providers) {
        providers.erase(
            std::remove_if(providers.begin(), providers.end(),
                [&name](const ProviderPtr& p) { return p->name() == name; }),
            providers.end());
    };

    remove_from(local_providers_);
    remove_from(cloud_providers_);
}

bool LLMRouter::has_local_provider() const {
    std::lock_guard<std::mutex> lock(providers_mutex_);
    return std::any_of(local_providers_.begin(), local_providers_.end(),
                       [](const ProviderPtr& p) { return p->is_available(); });
}

bool LLMRouter::has_cloud_provider() const {
    std::lock_guard<std::mutex> lock(providers_mutex_);
    return std::any_of(cloud_providers_.begin(), cloud_providers_.end(),
                       [](const ProviderPtr& p) { return p->is_available(); });
}

std::vector<ProviderInfo> LLMRouter::list_providers() const {
    std::lock_guard<std::mutex> lock(providers_mutex_);
    std::vector<ProviderInfo> result;

    for (const auto& p : local_providers_) {
        result.push_back(p->info());
    }
    for (const auto& p : cloud_providers_) {
        result.push_back(p->info());
    }

    return result;
}

std::string LLMRouter::active_provider_name() const {
    std::lock_guard<std::mutex> lock(providers_mutex_);
    return last_used_provider_;
}

Result<CompletionResult> LLMRouter::complete(const CompletionRequest& request) {
    // Check cache first
    if (config_.enable_cache) {
        std::string key = cache_key(request);
        if (auto cached = get_cached(key)) {
            return *cached;
        }
    }

    LLMProvider* provider = select_provider(request);
    if (!provider) {
        return Error(ErrorCode::NOT_FOUND, "No available LLM providers");
    }

    int retries = 0;
    Error last_error(ErrorCode::INTERNAL_ERROR, "Unknown error");

    while (retries <= config_.max_retries) {
        auto start = std::chrono::steady_clock::now();

        auto result = provider->complete(request);

        auto end = std::chrono::steady_clock::now();
        int latency = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                end - start).count());

        if (result.ok()) {
            // Update stats
            update_latency(provider->name(), latency);
            record_result(provider->name(), true, latency);

            {
                std::lock_guard<std::mutex> lock(providers_mutex_);
                last_used_provider_ = provider->name();
            }

            // Cache result
            if (config_.enable_cache) {
                cache_result(cache_key(request), result.value());
            }

            return result;
        }

        // Record failure
        last_error = result.error();
        record_result(provider->name(), false, latency);

        // Try fallback if enabled
        if (config_.enable_fallback) {
            provider = get_fallback_provider(provider);
            if (!provider) {
                return last_error;  // No fallback available
            }
        } else {
            return last_error;
        }

        retries++;
        if (retries <= config_.max_retries && config_.retry_delay_ms > 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(config_.retry_delay_ms));
        }
    }

    return last_error;
}

Result<std::string> LLMRouter::complete_code(
    const std::string& code_context,
    const std::string& language,
    StreamCallback callback) {

    std::ostringstream user_msg;
    if (!language.empty()) {
        user_msg << "Language: " << language << "\n\n";
    }
    user_msg << "Complete this code:\n\n" << code_context;

    CompletionRequest request;
    request.system_prompt = prompts::CODE_COMPLETION;
    request.messages = {Message::user(user_msg.str())};
    request.max_tokens = 256;  // Shorter for completions
    request.temperature = 0.2f;  // More deterministic
    request.on_chunk = callback;

    auto result = complete(request);
    if (!result.ok()) {
        return result.error();
    }

    return result->content;
}

Result<std::string> LLMRouter::generate_from_nl(
    const std::string& description,
    const std::string& language,
    StreamCallback callback) {

    std::ostringstream user_msg;
    if (!language.empty()) {
        user_msg << "Write this in " << language << ":\n\n";
    }
    user_msg << description;

    CompletionRequest request;
    request.system_prompt = prompts::NL_TO_CODE;
    request.messages = {Message::user(user_msg.str())};
    request.max_tokens = 1024;  // Longer for generation
    request.temperature = 0.3f;
    request.on_chunk = callback;

    auto result = complete(request);
    if (!result.ok()) {
        return result.error();
    }

    return result->content;
}

Result<CompletionResult> LLMRouter::complete_with_provider(
    const std::string& provider_name,
    const CompletionRequest& request) {

    std::lock_guard<std::mutex> lock(providers_mutex_);

    // Find provider by name
    auto find_provider = [&provider_name](const std::vector<ProviderPtr>& providers)
        -> LLMProvider* {
        for (const auto& p : providers) {
            if (p->name() == provider_name && p->is_available()) {
                return p.get();
            }
        }
        return nullptr;
    };

    LLMProvider* provider = find_provider(local_providers_);
    if (!provider) {
        provider = find_provider(cloud_providers_);
    }

    if (!provider) {
        return Error(ErrorCode::NOT_FOUND,
            "Provider not found or not available: " + provider_name);
    }

    return provider->complete(request);
}

void LLMRouter::abort() {
    std::lock_guard<std::mutex> lock(providers_mutex_);

    for (auto& p : local_providers_) {
        p->abort();
    }
    for (auto& p : cloud_providers_) {
        p->abort();
    }
}

std::vector<LLMRouter::ProviderStats> LLMRouter::get_stats() const {
    std::lock_guard<std::mutex> lock(providers_mutex_);
    std::vector<ProviderStats> result;

    for (const auto& [name, stats] : stats_) {
        result.push_back(stats);
    }

    return result;
}

void LLMRouter::reset_stats() {
    std::lock_guard<std::mutex> lock(providers_mutex_);
    stats_.clear();
    latency_records_.clear();
}

LLMProvider* LLMRouter::select_provider(const CompletionRequest& request) {
    std::lock_guard<std::mutex> lock(providers_mutex_);
    return select_by_strategy(request);
}

LLMProvider* LLMRouter::select_by_strategy(const CompletionRequest& request) {
    // Helper to find first available provider
    auto find_available = [](const std::vector<ProviderPtr>& providers)
        -> LLMProvider* {
        for (const auto& p : providers) {
            if (p->is_available()) {
                return p.get();
            }
        }
        return nullptr;
    };

    switch (config_.strategy) {
        case RoutingStrategy::LOCAL_ONLY: {
            return find_available(local_providers_);
        }

        case RoutingStrategy::CLOUD_ONLY: {
            return find_available(cloud_providers_);
        }

        case RoutingStrategy::LOCAL_FIRST: {
            if (auto* p = find_available(local_providers_)) {
                return p;
            }
            return find_available(cloud_providers_);
        }

        case RoutingStrategy::CLOUD_FIRST: {
            if (auto* p = find_available(cloud_providers_)) {
                return p;
            }
            return find_available(local_providers_);
        }

        case RoutingStrategy::LATENCY_OPTIMIZED: {
            LLMProvider* best = nullptr;
            float best_latency = std::numeric_limits<float>::max();

            auto check_providers = [&](const std::vector<ProviderPtr>& providers) {
                for (const auto& p : providers) {
                    if (!p->is_available()) continue;

                    auto it = latency_records_.find(p->name());
                    float latency = (it != latency_records_.end())
                        ? it->second.avg_ms
                        : (p->is_local() ? 100.0f : 1000.0f);  // Default estimates

                    if (latency < best_latency) {
                        best_latency = latency;
                        best = p.get();
                    }
                }
            };

            check_providers(local_providers_);
            check_providers(cloud_providers_);

            return best;
        }

        case RoutingStrategy::QUALITY_OPTIMIZED: {
            int complexity = estimate_complexity(request);

            // Use cloud for complex requests
            if (complexity > config_.complexity_threshold_tokens ||
                request.messages.size() > 2) {
                if (auto* p = find_available(cloud_providers_)) {
                    return p;
                }
            }

            // Use local for simple requests
            if (auto* p = find_available(local_providers_)) {
                return p;
            }

            return find_available(cloud_providers_);
        }
    }

    return nullptr;
}

LLMProvider* LLMRouter::get_fallback_provider(LLMProvider* failed) {
    std::lock_guard<std::mutex> lock(providers_mutex_);

    // If local failed, try cloud
    if (failed->is_local()) {
        for (const auto& p : cloud_providers_) {
            if (p->is_available() && p.get() != failed) {
                return p.get();
            }
        }
    }

    // If cloud failed, try local
    for (const auto& p : local_providers_) {
        if (p->is_available() && p.get() != failed) {
            return p.get();
        }
    }

    // Try any other provider
    for (const auto& p : cloud_providers_) {
        if (p->is_available() && p.get() != failed) {
            return p.get();
        }
    }

    return nullptr;
}

int LLMRouter::estimate_complexity(const CompletionRequest& request) const {
    // Estimate output tokens based on input and request
    int input_tokens = 0;
    for (const auto& msg : request.messages) {
        input_tokens += static_cast<int>(msg.content.size()) / 4;  // Rough estimate
    }

    // Larger context usually means more complex output
    if (input_tokens > config_.context_threshold_tokens) {
        return config_.complexity_threshold_tokens + 1;
    }

    // Use max_tokens as complexity estimate
    return request.max_tokens;
}

void LLMRouter::update_latency(const std::string& provider, int latency_ms) {
    auto& record = latency_records_[provider];

    // Exponential moving average
    if (record.sample_count == 0) {
        record.avg_ms = static_cast<float>(latency_ms);
    } else {
        const float alpha = 0.3f;  // Weight for new sample
        record.avg_ms = alpha * static_cast<float>(latency_ms) +
                       (1.0f - alpha) * record.avg_ms;
    }

    record.sample_count++;
    record.last_update = std::chrono::steady_clock::now();
}

void LLMRouter::record_result(const std::string& provider, bool success, int latency_ms) {
    auto& stats = stats_[provider];
    stats.name = provider;
    stats.request_count++;

    if (success) {
        stats.success_count++;
    } else {
        stats.failure_count++;
    }

    // Update average latency
    if (stats.request_count == 1) {
        stats.avg_latency_ms = static_cast<float>(latency_ms);
    } else {
        stats.avg_latency_ms = (stats.avg_latency_ms * (stats.request_count - 1) +
                                static_cast<float>(latency_ms)) / stats.request_count;
    }

    stats.last_used = std::chrono::steady_clock::now();
}

std::string LLMRouter::cache_key(const CompletionRequest& request) const {
    std::ostringstream ss;
    ss << request.system_prompt << "|";
    for (const auto& msg : request.messages) {
        ss << static_cast<int>(msg.role) << ":" << msg.content << "|";
    }
    ss << request.max_tokens << "|" << request.temperature;

    // Simple hash
    std::hash<std::string> hasher;
    return std::to_string(hasher(ss.str()));
}

std::optional<CompletionResult> LLMRouter::get_cached(const std::string& key) const {
    std::lock_guard<std::mutex> lock(cache_mutex_);

    auto it = response_cache_.find(key);
    if (it == response_cache_.end()) {
        return std::nullopt;
    }

    // Check TTL
    auto now = std::chrono::steady_clock::now();
    auto age = std::chrono::duration_cast<std::chrono::seconds>(
        now - it->second.timestamp).count();

    if (age > config_.cache_ttl_seconds) {
        response_cache_.erase(it);
        return std::nullopt;
    }

    return it->second.result;
}

void LLMRouter::cache_result(const std::string& key, const CompletionResult& result) {
    std::lock_guard<std::mutex> lock(cache_mutex_);

    // Clean up old entries if cache is full
    if (response_cache_.size() >= config_.cache_size) {
        cleanup_cache_unlocked();
    }

    response_cache_[key] = CachedResponse{
        result,
        std::chrono::steady_clock::now()
    };
}

void LLMRouter::cleanup_cache() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cleanup_cache_unlocked();
}

void LLMRouter::cleanup_cache_unlocked() {
    // Note: caller must hold cache_mutex_
    auto now = std::chrono::steady_clock::now();

    // Remove expired entries
    for (auto it = response_cache_.begin(); it != response_cache_.end(); ) {
        auto age = std::chrono::duration_cast<std::chrono::seconds>(
            now - it->second.timestamp).count();

        if (age > config_.cache_ttl_seconds) {
            it = response_cache_.erase(it);
        } else {
            ++it;
        }
    }

    // If still too full, remove oldest entries
    while (response_cache_.size() >= config_.cache_size && !response_cache_.empty()) {
        auto oldest = response_cache_.begin();
        for (auto it = response_cache_.begin(); it != response_cache_.end(); ++it) {
            if (it->second.timestamp < oldest->second.timestamp) {
                oldest = it;
            }
        }
        response_cache_.erase(oldest);
    }
}

// ============================================================================
// Factory Implementation
// ============================================================================

std::string LLMFactory::get_ollama_host() {
    const char* host = std::getenv("DAM_OLLAMA_HOST");
    return host ? host : "http://localhost:11434";
}

Result<std::unique_ptr<LLMRouter>> LLMFactory::create_from_env() {
    // Check if explicit model is set via environment
    const char* model_env = std::getenv("DAM_OLLAMA_MODEL");
    if (model_env && model_env[0] != '\0') {
        return create_with_ollama_model(model_env, get_ollama_host());
    }

    // Use discovery-based creation
    auto result = create_with_discovery();
    if (!result.ok()) {
        return result.error();
    }

    auto& create_result = result.value();

    // If model selection is required, return error (CLI handles this)
    if (create_result.requires_model_selection) {
        return Error(ErrorCode::REQUIRES_MODEL_SELECTION,
            "Multiple Ollama models available. Please select a model.");
    }

    if (!create_result.router) {
        return Error(ErrorCode::NOT_FOUND,
            "No LLM providers available.");
    }

    return std::move(create_result.router);
}

Result<LLMCreateResult> LLMFactory::create_with_discovery() {
    LLMCreateResult result;
    std::string host = get_ollama_host();

    // Try llama.cpp first if configured
#ifdef DAM_HAS_LLAMACPP
    if (std::getenv("DAM_LLAMACPP_MODEL_PATH")) {
        auto provider = LlamaCppProvider::create_from_env();
        if (provider.ok()) {
            result.router = std::make_unique<LLMRouter>();
            result.router->add_local_provider(std::move(provider.value()));
            return result;
        }
    }
#endif

    // Discover Ollama
    result.discovery = ModelDiscovery::discover_ollama(host);

    if (!result.discovery.ollama_running) {
        return Error(ErrorCode::OLLAMA_NOT_RUNNING,
            result.discovery.error_message.empty()
                ? "Cannot connect to Ollama at " + host
                : result.discovery.error_message);
    }

    if (result.discovery.available_models.empty()) {
        return Error(ErrorCode::MODEL_NOT_FOUND,
            "No models installed in Ollama");
    }

    // Single model - auto-select
    if (result.discovery.available_models.size() == 1) {
        auto router_result = create_with_ollama_model(
            result.discovery.available_models[0].name, host);
        if (router_result.ok()) {
            result.router = std::move(router_result.value());
            return result;
        }
        return router_result.error();
    }

    // Multiple models - check for recommended code model
    if (!result.discovery.recommended_model.empty()) {
        // Auto-select recommended model for non-interactive use
        auto router_result = create_with_ollama_model(
            result.discovery.recommended_model, host);
        if (router_result.ok()) {
            result.router = std::move(router_result.value());
            // Still flag for potential interactive selection
            result.requires_model_selection = true;
            return result;
        }
    }

    // Multiple models, no recommendation - require selection
    result.requires_model_selection = true;
    return result;
}

Result<std::unique_ptr<LLMRouter>> LLMFactory::create_with_ollama_model(
    const std::string& model_name,
    const std::string& host) {

    auto router = std::make_unique<LLMRouter>();

    // Create Ollama provider with specific model
    OllamaConfig config;
    config.host = host;
    config.model = model_name;

    auto provider = std::make_unique<OllamaProvider>(std::move(config));

    // Verify the provider is available
    if (!provider->is_available()) {
        return Error(ErrorCode::NETWORK_ERROR,
            "Cannot connect to Ollama at " + host);
    }

    router->add_local_provider(std::move(provider));
    return router;
}

Result<std::unique_ptr<LLMRouter>> LLMFactory::create_default() {
    return create_from_env();
}

}  // namespace dam::llm
