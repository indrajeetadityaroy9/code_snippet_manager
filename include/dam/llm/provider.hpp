#pragma once

#include <dam/result.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dam::llm {

// ============================================================================
// Common Types
// ============================================================================

struct Message {
    enum class Role { SYSTEM, USER, ASSISTANT };
    Role role;
    std::string content;

    static Message system(std::string content) {
        return {Role::SYSTEM, std::move(content)};
    }
    static Message user(std::string content) {
        return {Role::USER, std::move(content)};
    }
    static Message assistant(std::string content) {
        return {Role::ASSISTANT, std::move(content)};
    }
};

using StreamCallback = std::function<bool(const std::string& chunk)>;

struct CompletionRequest {
    std::string system_prompt;
    std::vector<Message> messages;

    // Generation parameters
    int max_tokens = 256;
    float temperature = 0.3f;
    float top_p = 0.9f;
    int top_k = 40;
    float repeat_penalty = 1.1f;
    std::vector<std::string> stop_sequences;

    // Streaming
    StreamCallback on_chunk = nullptr;

    // Timeouts
    int timeout_ms = 30000;
};

struct CompletionResult {
    std::string content;
    std::string stop_reason;  // "eos", "max_tokens", "stop_sequence", "aborted"

    // Token counts
    int prompt_tokens = 0;
    int completion_tokens = 0;

    // Performance metrics
    int latency_ms = 0;
    float tokens_per_second = 0.0f;
};

struct ProviderInfo {
    std::string name;
    std::string model_id;
    bool is_local = false;
    bool supports_streaming = true;
    size_t context_length = 4096;
    size_t model_size_bytes = 0;
};

// ============================================================================
// Abstract Provider Interface
// ============================================================================

class LLMProvider {
public:
    virtual ~LLMProvider() = default;

    // Identification
    virtual ProviderInfo info() const = 0;

    // Convenience accessors
    std::string name() const { return info().name; }
    std::string model_id() const { return info().model_id; }
    bool is_local() const { return info().is_local; }

    // Lifecycle
    virtual Result<void> initialize() = 0;
    virtual void shutdown() = 0;
    virtual bool is_available() const = 0;

    // Core completion
    virtual Result<CompletionResult> complete(const CompletionRequest& request) = 0;

    // Abort support (thread-safe)
    virtual void abort() = 0;
    virtual bool is_aborted() const = 0;
    virtual void reset_abort() = 0;
};

using ProviderPtr = std::unique_ptr<LLMProvider>;

// ============================================================================
// System Prompts for Code Tasks
// ============================================================================

namespace prompts {

constexpr const char* CODE_COMPLETION = R"(You are an expert code completion assistant.
Given partial code, provide ONLY the completion - no explanations, no markdown, no code fences.
Complete naturally from exactly where the code ends. Match the existing style and indentation.
Provide concise, idiomatic completions. Stop at logical boundaries (end of statement/function).
Do not repeat any code that was already written.)";

constexpr const char* NL_TO_CODE = R"(You are an expert programmer. Convert natural language requests to code.
Output ONLY executable code - no explanations, no markdown code fences, no comments about what the code does.
Use the specified language if provided, otherwise infer the most appropriate language from context.
Write clean, idiomatic, production-quality code. Include necessary imports/headers.
Keep the code focused and minimal - only implement what was requested.)";

}  // namespace prompts

}  // namespace dam::llm
