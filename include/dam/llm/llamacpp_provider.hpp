#pragma once

#include "provider.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#ifdef DAM_HAS_LLAMACPP
#include <llama.h>
#endif

namespace dam::llm {

struct LlamaCppConfig {
    std::string model_path;

    // Context configuration
    int n_ctx = 4096;       // Context window size
    int n_batch = 512;      // Batch size for prompt processing

    // Threading
    int n_threads = 4;          // CPU threads for generation
    int n_threads_batch = 4;    // CPU threads for batch processing

    // GPU offloading
    int n_gpu_layers = 0;   // Layers to offload to GPU (0 = CPU only)
    int main_gpu = 0;       // Main GPU device index

    // Memory optimization
    bool use_mmap = true;   // Memory-map model file
    bool use_mlock = false; // Lock model in RAM (prevents swapping)

    // Attention
    bool flash_attn = false;    // Use flash attention if available
};

class LlamaCppProvider : public LLMProvider {
public:
    explicit LlamaCppProvider(LlamaCppConfig config);
    ~LlamaCppProvider() override;

    // Non-copyable, movable
    LlamaCppProvider(const LlamaCppProvider&) = delete;
    LlamaCppProvider& operator=(const LlamaCppProvider&) = delete;
    LlamaCppProvider(LlamaCppProvider&&) noexcept;
    LlamaCppProvider& operator=(LlamaCppProvider&&) noexcept;

    // LLMProvider interface
    ProviderInfo info() const override;
    Result<void> initialize() override;
    void shutdown() override;
    bool is_available() const override;
    Result<CompletionResult> complete(const CompletionRequest& request) override;
    void abort() override;
    bool is_aborted() const override;
    void reset_abort() override;

    // llama.cpp specific
    Result<void> warmup();  // Pre-load model into memory
    size_t memory_usage() const;

    // Factory methods
    static Result<std::unique_ptr<LlamaCppProvider>> create(LlamaCppConfig config);
    static Result<std::unique_ptr<LlamaCppProvider>> create_from_env();

private:
    LlamaCppConfig config_;

#ifdef DAM_HAS_LLAMACPP
    llama_model* model_ = nullptr;
    llama_context* context_ = nullptr;
    llama_sampler* sampler_ = nullptr;
#else
    void* model_ = nullptr;
    void* context_ = nullptr;
    void* sampler_ = nullptr;
#endif

    std::atomic<bool> initialized_{false};
    std::atomic<bool> abort_requested_{false};
    mutable std::mutex inference_mutex_;

    // Internal helpers
    std::vector<int32_t> tokenize(const std::string& text, bool add_bos);
    std::string detokenize(const std::vector<int32_t>& tokens);
    std::string build_prompt(const CompletionRequest& request);
    void setup_sampler(const CompletionRequest& request);
};

}  // namespace dam::llm
