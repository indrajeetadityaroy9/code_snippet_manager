#include "claude_client.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cstring>
#include <sstream>

namespace dam::cli {

using json = nlohmann::json;

// Streaming response parser state
struct StreamingState {
    StreamCallback callback;
    std::string buffer;
    std::string accumulated_content;
    std::string stop_reason;
    int input_tokens = 0;
    int output_tokens = 0;
    std::atomic<bool>* abort_flag;
    bool error_occurred = false;
    std::string error_message;
};

// Non-streaming response accumulator
struct ResponseBuffer {
    std::string data;
};

// Write callback for non-streaming requests
static size_t buffer_write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buffer = static_cast<ResponseBuffer*>(userdata);
    size_t total_size = size * nmemb;
    buffer->data.append(ptr, total_size);
    return total_size;
}

std::optional<ClaudeClient> ClaudeClient::from_environment() {
    const char* api_key = std::getenv("ANTHROPIC_API_KEY");
    if (!api_key || api_key[0] == '\0') {
        return std::nullopt;
    }

    ClaudeClientConfig config;
    config.api_key = api_key;
    return ClaudeClient(std::move(config));
}

ClaudeClient::ClaudeClient(ClaudeClientConfig config)
    : config_(std::move(config)) {
    init_curl();
}

ClaudeClient::~ClaudeClient() {
    cleanup_curl();
}

ClaudeClient::ClaudeClient(ClaudeClient&& other) noexcept
    : config_(std::move(other.config_))
    , curl_handle_(other.curl_handle_)
    , abort_requested_(other.abort_requested_.load()) {
    other.curl_handle_ = nullptr;
}

ClaudeClient& ClaudeClient::operator=(ClaudeClient&& other) noexcept {
    if (this != &other) {
        cleanup_curl();
        config_ = std::move(other.config_);
        curl_handle_ = other.curl_handle_;
        abort_requested_ = other.abort_requested_.load();
        other.curl_handle_ = nullptr;
    }
    return *this;
}

bool ClaudeClient::is_available() const {
    return !config_.api_key.empty() && curl_handle_ != nullptr;
}

void ClaudeClient::init_curl() {
    static bool curl_initialized = false;
    if (!curl_initialized) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl_initialized = true;
    }
    curl_handle_ = curl_easy_init();
}

void ClaudeClient::cleanup_curl() {
    if (curl_handle_) {
        curl_easy_cleanup(static_cast<CURL*>(curl_handle_));
        curl_handle_ = nullptr;
    }
}

std::string ClaudeClient::test_connection() {
    if (!is_available()) {
        return "Client not initialized or API key missing";
    }

    // Make a minimal request to test connectivity
    std::vector<Message> messages{{Message::Role::USER, "Hi"}};
    auto result = complete("Reply with just 'ok'", messages);

    if (!result.ok()) {
        return result.error().message();
    }
    return "";  // Success
}

size_t ClaudeClient::write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* state = static_cast<StreamingState*>(userdata);
    size_t total_size = size * nmemb;

    // Check for abort
    if (state->abort_flag && state->abort_flag->load()) {
        return 0;  // Abort transfer
    }

    state->buffer.append(ptr, total_size);

    // Process complete SSE events (separated by double newlines)
    size_t pos;
    while ((pos = state->buffer.find("\n\n")) != std::string::npos) {
        std::string event = state->buffer.substr(0, pos);
        state->buffer.erase(0, pos + 2);

        // Skip empty events
        if (event.empty() || event == "\n") {
            continue;
        }

        // Parse SSE event - look for "data: " prefix
        size_t data_start = event.find("data: ");
        if (data_start == std::string::npos) {
            continue;
        }

        // Extract JSON data
        std::string data = event.substr(data_start + 6);

        // Handle end marker
        if (data == "[DONE]") {
            continue;
        }

        try {
            auto j = json::parse(data);
            std::string event_type = j.value("type", "");

            if (event_type == "content_block_delta") {
                if (j.contains("delta") && j["delta"].contains("text")) {
                    std::string text = j["delta"]["text"];
                    state->accumulated_content += text;
                    if (state->callback && !state->callback(text)) {
                        return 0;  // User aborted
                    }
                }
            } else if (event_type == "message_delta") {
                if (j.contains("usage")) {
                    state->output_tokens = j["usage"].value("output_tokens", 0);
                }
                if (j.contains("delta") && j["delta"].contains("stop_reason")) {
                    auto stop = j["delta"]["stop_reason"];
                    if (!stop.is_null()) {
                        state->stop_reason = stop.get<std::string>();
                    }
                }
            } else if (event_type == "message_start") {
                if (j.contains("message") && j["message"].contains("usage")) {
                    state->input_tokens = j["message"]["usage"].value("input_tokens", 0);
                }
            } else if (event_type == "error") {
                state->error_occurred = true;
                if (j.contains("error")) {
                    state->error_message = j["error"].value("message", "Unknown API error");
                } else {
                    state->error_message = "Unknown API error";
                }
                return 0;  // Stop processing
            }
        } catch (const json::exception& e) {
            // Ignore parse errors for malformed events
        }
    }

    return total_size;
}

std::string ClaudeClient::build_request_body(
    const std::string& system_prompt,
    const std::vector<Message>& messages,
    bool stream) {

    json request;
    request["model"] = config_.model;
    request["max_tokens"] = config_.max_tokens;
    request["temperature"] = config_.temperature;
    request["stream"] = stream;

    if (!system_prompt.empty()) {
        request["system"] = system_prompt;
    }

    if (!config_.stop_sequences.empty()) {
        request["stop_sequences"] = config_.stop_sequences;
    }

    json msgs = json::array();
    for (const auto& msg : messages) {
        json m;
        m["role"] = (msg.role == Message::Role::USER) ? "user" : "assistant";
        m["content"] = msg.content;
        msgs.push_back(m);
    }
    request["messages"] = msgs;

    return request.dump();
}

Result<CompletionResult> ClaudeClient::complete_streaming(
    const std::string& system_prompt,
    const std::vector<Message>& messages,
    StreamCallback callback) {

    if (!is_available()) {
        return Error(ErrorCode::INVALID_ARGUMENT, "Claude client not available");
    }

    reset_abort();

    StreamingState state;
    state.callback = callback;
    state.abort_flag = &abort_requested_;

    std::string request_body = build_request_body(system_prompt, messages, true);

    CURL* curl = static_cast<CURL*>(curl_handle_);
    curl_easy_reset(curl);

    // Set up headers
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("x-api-key: " + config_.api_key).c_str());
    headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");

    std::string url = config_.api_base + "/v1/messages";

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(config_.timeout_ms));

    // Disable signal handling in curl (for thread safety)
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);

    if (abort_requested_.load()) {
        return Error(ErrorCode::TRANSACTION_ABORTED, "Request aborted");
    }

    if (res != CURLE_OK) {
        return Error(ErrorCode::IO_ERROR, std::string("Network error: ") + curl_easy_strerror(res));
    }

    // Check HTTP status code
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (http_code >= 400) {
        if (http_code == 401) {
            return Error(ErrorCode::PERMISSION_DENIED, "Invalid API key");
        } else if (http_code == 429) {
            return Error(ErrorCode::OUT_OF_SPACE, "Rate limit exceeded");
        } else if (http_code >= 500) {
            return Error(ErrorCode::IO_ERROR, "API server error");
        }
        return Error(ErrorCode::IO_ERROR, "HTTP error " + std::to_string(http_code));
    }

    if (state.error_occurred) {
        return Error(ErrorCode::INTERNAL_ERROR, state.error_message);
    }

    CompletionResult result;
    result.content = state.accumulated_content;
    result.stop_reason = state.stop_reason;
    result.input_tokens = state.input_tokens;
    result.output_tokens = state.output_tokens;

    return result;
}

Result<CompletionResult> ClaudeClient::complete(
    const std::string& system_prompt,
    const std::vector<Message>& messages) {

    if (!is_available()) {
        return Error(ErrorCode::INVALID_ARGUMENT, "Claude client not available");
    }

    reset_abort();

    std::string request_body = build_request_body(system_prompt, messages, false);

    CURL* curl = static_cast<CURL*>(curl_handle_);
    curl_easy_reset(curl);

    ResponseBuffer response;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("x-api-key: " + config_.api_key).c_str());
    headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");

    std::string url = config_.api_base + "/v1/messages";

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, buffer_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(config_.timeout_ms));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        return Error(ErrorCode::IO_ERROR, std::string("Network error: ") + curl_easy_strerror(res));
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (http_code >= 400) {
        // Try to extract error message from response
        try {
            auto j = json::parse(response.data);
            if (j.contains("error") && j["error"].contains("message")) {
                return Error(ErrorCode::IO_ERROR, j["error"]["message"].get<std::string>());
            }
        } catch (...) {}
        return Error(ErrorCode::IO_ERROR, "HTTP error " + std::to_string(http_code));
    }

    try {
        auto j = json::parse(response.data);

        CompletionResult result;

        if (j.contains("content") && j["content"].is_array() && !j["content"].empty()) {
            result.content = j["content"][0].value("text", "");
        }
        result.stop_reason = j.value("stop_reason", "");

        if (j.contains("usage")) {
            result.input_tokens = j["usage"].value("input_tokens", 0);
            result.output_tokens = j["usage"].value("output_tokens", 0);
        }

        return result;
    } catch (const json::exception& e) {
        return Error(ErrorCode::CORRUPTION, std::string("Failed to parse response: ") + e.what());
    }
}

Result<std::string> ClaudeClient::complete_code(
    const std::string& code_context,
    const std::string& language,
    StreamCallback callback) {

    std::ostringstream user_message;

    if (!language.empty()) {
        user_message << "Language: " << language << "\n\n";
    }

    user_message << "Complete this code (output ONLY the continuation, not what's already written):\n\n";
    user_message << code_context;

    std::vector<Message> messages{{Message::Role::USER, user_message.str()}};

    // Use shorter max_tokens for completions
    auto original_max_tokens = config_.max_tokens;
    config_.max_tokens = 256;

    auto result = complete_streaming(prompts::CODE_COMPLETION, messages, callback);

    config_.max_tokens = original_max_tokens;

    if (!result.ok()) {
        return result.error();
    }

    return result->content;
}

Result<std::string> ClaudeClient::generate_from_nl(
    const std::string& natural_language,
    const std::string& language,
    StreamCallback callback) {

    std::ostringstream user_message;

    if (!language.empty()) {
        user_message << "Write this in " << language << ":\n\n";
    }

    user_message << natural_language;

    std::vector<Message> messages{{Message::Role::USER, user_message.str()}};

    auto result = complete_streaming(prompts::NL_TO_CODE, messages, callback);

    if (!result.ok()) {
        return result.error();
    }

    return result->content;
}

void ClaudeClient::abort() {
    abort_requested_.store(true);
}

bool ClaudeClient::is_aborted() const {
    return abort_requested_.load();
}

void ClaudeClient::reset_abort() {
    abort_requested_.store(false);
}

}  // namespace dam::cli
