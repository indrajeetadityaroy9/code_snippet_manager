#pragma once

#include "provider.hpp"
#include "llamacpp_provider.hpp"
#include "model_discovery.hpp"
#include "ollama_provider.hpp"

#include <chrono>
#include <mutex>
#include <unordered_map>

namespace dam::llm {

// ============================================================================
// Routing Configuration
// ============================================================================

enum class RoutingStrategy {
    LOCAL_ONLY,        // Only use local models (fail if unavailable)
    CLOUD_ONLY,        // Only use cloud models
    LOCAL_FIRST,       // Try local, fallback to cloud (default)
    CLOUD_FIRST,       // Try cloud, fallback to local
    LATENCY_OPTIMIZED, // Track latencies, pick fastest
    QUALITY_OPTIMIZED  // Use cloud for complex, local for simple
};

struct LLMRouterConfig {
    RoutingStrategy strategy = RoutingStrategy::LOCAL_FIRST;

    // Timeouts
    int local_timeout_ms = 10000;   // Max wait for local model
    int cloud_timeout_ms = 30000;   // Max wait for cloud model

    // Fallback behavior
    bool enable_fallback = true;
    int max_retries = 2;
    int retry_delay_ms = 100;

    // Quality-based routing thresholds
    int complexity_threshold_tokens = 50;   // > 50 output tokens -> cloud
    int context_threshold_tokens = 1000;    // > 1000 context tokens -> cloud

    // Caching
    bool enable_cache = true;
    size_t cache_size = 100;
    int cache_ttl_seconds = 300;  // 5 minutes
};

// ============================================================================
// LLM Router
// ============================================================================

class LLMRouter {
public:
    explicit LLMRouter(LLMRouterConfig config = {});
    ~LLMRouter();

    // Provider management
    void add_local_provider(ProviderPtr provider);
    void add_cloud_provider(ProviderPtr provider);
    void remove_provider(const std::string& name);

    // Provider queries
    bool has_local_provider() const;
    bool has_cloud_provider() const;
    std::vector<ProviderInfo> list_providers() const;
    std::string active_provider_name() const;

    // Main completion interface (routes automatically)
    Result<CompletionResult> complete(const CompletionRequest& request);

    // Specialized methods (with intelligent routing)
    Result<std::string> complete_code(
        const std::string& code_context,
        const std::string& language = "",
        StreamCallback callback = nullptr);

    Result<std::string> generate_from_nl(
        const std::string& description,
        const std::string& language = "",
        StreamCallback callback = nullptr);

    // Direct provider access (bypass routing)
    Result<CompletionResult> complete_with_provider(
        const std::string& provider_name,
        const CompletionRequest& request);

    // Abort all in-progress requests
    void abort();

    // Statistics
    struct ProviderStats {
        std::string name;
        int request_count = 0;
        int success_count = 0;
        int failure_count = 0;
        float avg_latency_ms = 0;
        float tokens_per_second = 0;
        std::chrono::steady_clock::time_point last_used;
    };
    std::vector<ProviderStats> get_stats() const;
    void reset_stats();

private:
    LLMRouterConfig config_;

    std::vector<ProviderPtr> local_providers_;
    std::vector<ProviderPtr> cloud_providers_;
    mutable std::mutex providers_mutex_;

    // Provider selection state
    mutable std::string last_used_provider_;

    // Latency tracking
    struct LatencyRecord {
        float avg_ms = 0;
        int sample_count = 0;
        std::chrono::steady_clock::time_point last_update;
    };
    mutable std::unordered_map<std::string, LatencyRecord> latency_records_;

    // Response caching
    struct CachedResponse {
        CompletionResult result;
        std::chrono::steady_clock::time_point timestamp;
    };
    mutable std::unordered_map<std::string, CachedResponse> response_cache_;
    mutable std::mutex cache_mutex_;

    // Statistics
    mutable std::unordered_map<std::string, ProviderStats> stats_;

    // Routing logic
    LLMProvider* select_provider(const CompletionRequest& request);
    LLMProvider* select_by_strategy(const CompletionRequest& request);
    LLMProvider* get_fallback_provider(LLMProvider* failed);

    int estimate_complexity(const CompletionRequest& request) const;
    void update_latency(const std::string& provider, int latency_ms);
    void record_result(const std::string& provider, bool success, int latency_ms);

    // Caching
    std::string cache_key(const CompletionRequest& request) const;
    std::optional<CompletionResult> get_cached(const std::string& key) const;
    void cache_result(const std::string& key, const CompletionResult& result);
    void cleanup_cache();
    void cleanup_cache_unlocked();  // Must hold cache_mutex_
};

// ============================================================================
// Factory for easy setup
// ============================================================================

/**
 * Result of attempting to create an LLM router.
 * May indicate that user input is needed (model selection).
 */
struct LLMCreateResult {
    std::unique_ptr<LLMRouter> router;           // Valid if creation succeeded
    DiscoveryResult discovery;                   // Discovery info (for error messages)
    bool requires_model_selection = false;       // True if multiple models available
};

class LLMFactory {
public:
    /**
     * Create router from environment with auto-discovery.
     *
     * Checks DAM_OLLAMA_MODEL env var first. If not set, discovers Ollama
     * and auto-selects if single model, or returns REQUIRES_MODEL_SELECTION
     * if multiple models need user selection.
     */
    static Result<std::unique_ptr<LLMRouter>> create_from_env();

    /**
     * Create router with discovery information.
     *
     * Returns full discovery details to allow CLI to show model picker
     * or detailed error messages.
     */
    static Result<LLMCreateResult> create_with_discovery();

    /**
     * Create router with specific Ollama model.
     *
     * @param model_name Ollama model name (e.g., "codellama:7b-instruct")
     * @param host Ollama server URL (default: http://localhost:11434)
     */
    static Result<std::unique_ptr<LLMRouter>> create_with_ollama_model(
        const std::string& model_name,
        const std::string& host = "http://localhost:11434");

    /**
     * Create with best available provider.
     */
    static Result<std::unique_ptr<LLMRouter>> create_default();

    /**
     * Get Ollama host from environment or default.
     */
    static std::string get_ollama_host();
};

}  // namespace dam::llm
