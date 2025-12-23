#pragma once

#include <dam/result.hpp>

#include <atomic>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dam::cli {

/**
 * Configuration for Claude API client.
 */
struct ClaudeClientConfig {
    std::string api_key;
    std::string model = "claude-sonnet-4-20250514";
    std::string api_base = "https://api.anthropic.com";
    int max_tokens = 1024;
    float temperature = 0.3f;           // Lower for code completion
    int timeout_ms = 30000;             // 30 second timeout
    std::vector<std::string> stop_sequences;
};

/**
 * Represents a message in the conversation.
 */
struct Message {
    enum class Role { USER, ASSISTANT };
    Role role;
    std::string content;
};

/**
 * Callback type for streaming responses.
 * Called with each text chunk as it arrives.
 * Return false to abort the stream.
 */
using StreamCallback = std::function<bool(const std::string& chunk)>;

/**
 * Result of a completion request.
 */
struct CompletionResult {
    std::string content;
    std::string stop_reason;    // "end_turn", "max_tokens", "stop_sequence"
    int input_tokens = 0;
    int output_tokens = 0;
};

/**
 * Claude API client with streaming support.
 *
 * Example usage:
 *   auto client = ClaudeClient::from_environment();
 *   if (!client) {
 *       std::cerr << "ANTHROPIC_API_KEY not set\n";
 *       return;
 *   }
 *
 *   auto result = client->complete_code(
 *       "def binary_search(arr, target):\n    ",
 *       "python",
 *       [](const std::string& chunk) {
 *           std::cout << chunk;
 *           return true;  // Continue streaming
 *       }
 *   );
 *
 * Thread safety: NOT thread-safe. Each thread should have its own client.
 */
class ClaudeClient {
public:
    /**
     * Create a client from environment variable ANTHROPIC_API_KEY.
     * @return Client if API key is available, nullopt otherwise
     */
    static std::optional<ClaudeClient> from_environment();

    /**
     * Create a client with explicit configuration.
     */
    explicit ClaudeClient(ClaudeClientConfig config);

    /**
     * Destructor - cleans up curl resources.
     */
    ~ClaudeClient();

    // Non-copyable
    ClaudeClient(const ClaudeClient&) = delete;
    ClaudeClient& operator=(const ClaudeClient&) = delete;

    // Movable
    ClaudeClient(ClaudeClient&& other) noexcept;
    ClaudeClient& operator=(ClaudeClient&& other) noexcept;

    /**
     * Check if the client is properly configured and can make requests.
     */
    bool is_available() const;

    /**
     * Test connectivity to the API (lightweight request).
     * @return Empty string on success, error message on failure
     */
    std::string test_connection();

    /**
     * Complete a prompt with streaming response.
     *
     * @param system_prompt System prompt for the completion
     * @param messages Conversation history
     * @param callback Called for each streaming chunk (return false to abort)
     * @return Final result or error
     */
    Result<CompletionResult> complete_streaming(
        const std::string& system_prompt,
        const std::vector<Message>& messages,
        StreamCallback callback = nullptr);

    /**
     * Complete a prompt and return the full response (no streaming).
     */
    Result<CompletionResult> complete(
        const std::string& system_prompt,
        const std::vector<Message>& messages);

    /**
     * Specialized method for code completion suggestions.
     * Uses optimized prompts for syntax completion.
     *
     * @param code_context Code before cursor
     * @param language Detected or specified language (empty for auto)
     * @param callback Streaming callback
     * @return Suggested completion
     */
    Result<std::string> complete_code(
        const std::string& code_context,
        const std::string& language = "",
        StreamCallback callback = nullptr);

    /**
     * Specialized method for natural language to code generation.
     * Generates complete, working code from a description.
     *
     * @param natural_language User's natural language request
     * @param language Target language (optional, inferred if empty)
     * @param callback Streaming callback
     * @return Generated code
     */
    Result<std::string> generate_from_nl(
        const std::string& natural_language,
        const std::string& language = "",
        StreamCallback callback = nullptr);

    /**
     * Abort any in-progress request.
     * Safe to call from another thread.
     */
    void abort();

    /**
     * Check if an abort has been requested.
     */
    bool is_aborted() const;

    /**
     * Reset abort flag for new requests.
     */
    void reset_abort();

    /**
     * Get current configuration.
     */
    const ClaudeClientConfig& config() const { return config_; }

private:
    ClaudeClientConfig config_;
    void* curl_handle_ = nullptr;
    std::atomic<bool> abort_requested_{false};

    void init_curl();
    void cleanup_curl();

    std::string build_request_body(
        const std::string& system_prompt,
        const std::vector<Message>& messages,
        bool stream);

    static size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata);
};

/**
 * System prompts for different completion modes.
 */
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

}  // namespace dam::cli
