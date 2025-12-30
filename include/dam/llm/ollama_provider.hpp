#pragma once

#include "provider.hpp"

#include <atomic>
#include <string>

namespace dam::llm {

struct OllamaConfig {
    std::string host = "http://localhost:11434";
    std::string model = "qwen2.5:1.5b-instruct";
    int timeout_ms = 30000;
    int context_length = 4096;
    bool keep_alive = true;  // Keep model loaded in memory
};

class OllamaProvider : public LLMProvider {
public:
    explicit OllamaProvider(OllamaConfig config);
    ~OllamaProvider() override;

    // Non-copyable, movable
    OllamaProvider(const OllamaProvider&) = delete;
    OllamaProvider& operator=(const OllamaProvider&) = delete;
    OllamaProvider(OllamaProvider&&) noexcept;
    OllamaProvider& operator=(OllamaProvider&&) noexcept;

    // LLMProvider interface
    ProviderInfo info() const override;
    Result<void> initialize() override;
    void shutdown() override;
    bool is_available() const override;
    Result<CompletionResult> complete(const CompletionRequest& request) override;
    void abort() override;
    bool is_aborted() const override;
    void reset_abort() override;

    // Ollama-specific
    Result<std::vector<std::string>> list_models();
    Result<void> pull_model(const std::string& model_name);
    Result<void> preload_model();

    // Factory methods
    static Result<std::unique_ptr<OllamaProvider>> create(OllamaConfig config);
    static Result<std::unique_ptr<OllamaProvider>> create_from_env();

private:
    OllamaConfig config_;
    void* curl_handle_ = nullptr;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> abort_requested_{false};

    void init_curl();
    void cleanup_curl();

    std::string build_request_body(const CompletionRequest& request, bool stream);
    static size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata);
};

}  // namespace dam::llm
