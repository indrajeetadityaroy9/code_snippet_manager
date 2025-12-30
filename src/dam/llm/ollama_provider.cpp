#include <dam/llm/ollama_provider.hpp>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <sstream>

namespace dam::llm {

using json = nlohmann::json;

// Streaming response state
struct OllamaStreamState {
    StreamCallback callback;
    std::string buffer;
    std::string accumulated_content;
    std::atomic<bool>* abort_flag;
    bool error_occurred = false;
    std::string error_message;
    int prompt_tokens = 0;
    int completion_tokens = 0;
};

// Non-streaming response buffer
struct ResponseBuffer {
    std::string data;
};

static size_t buffer_write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buffer = static_cast<ResponseBuffer*>(userdata);
    size_t total_size = size * nmemb;
    buffer->data.append(ptr, total_size);
    return total_size;
}

OllamaProvider::OllamaProvider(OllamaConfig config)
    : config_(std::move(config)) {}

OllamaProvider::~OllamaProvider() {
    shutdown();
}

OllamaProvider::OllamaProvider(OllamaProvider&& other) noexcept
    : config_(std::move(other.config_))
    , curl_handle_(other.curl_handle_)
    , initialized_(other.initialized_.load())
    , abort_requested_(other.abort_requested_.load()) {
    other.curl_handle_ = nullptr;
    other.initialized_ = false;
}

OllamaProvider& OllamaProvider::operator=(OllamaProvider&& other) noexcept {
    if (this != &other) {
        shutdown();
        config_ = std::move(other.config_);
        curl_handle_ = other.curl_handle_;
        initialized_ = other.initialized_.load();
        abort_requested_ = other.abort_requested_.load();
        other.curl_handle_ = nullptr;
        other.initialized_ = false;
    }
    return *this;
}

ProviderInfo OllamaProvider::info() const {
    ProviderInfo pinfo;
    pinfo.name = "ollama";
    pinfo.model_id = config_.model;
    pinfo.is_local = true;
    pinfo.supports_streaming = true;
    pinfo.context_length = config_.context_length;
    return pinfo;
}

void OllamaProvider::init_curl() {
    static std::once_flag curl_init_flag;
    std::call_once(curl_init_flag, []() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    });
    curl_handle_ = curl_easy_init();
}

void OllamaProvider::cleanup_curl() {
    if (curl_handle_) {
        curl_easy_cleanup(static_cast<CURL*>(curl_handle_));
        curl_handle_ = nullptr;
    }
}

Result<void> OllamaProvider::initialize() {
    if (initialized_) {
        return {};
    }

    init_curl();

    if (!curl_handle_) {
        return Error(ErrorCode::INTERNAL_ERROR, "Failed to initialize CURL");
    }

    // Check if Ollama is running by hitting the API
    CURL* curl = static_cast<CURL*>(curl_handle_);
    curl_easy_reset(curl);

    std::string url = config_.host + "/api/tags";
    ResponseBuffer response;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, buffer_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        cleanup_curl();
        if (res == CURLE_OPERATION_TIMEDOUT) {
            return Error(ErrorCode::TIMEOUT,
                "Connection to Ollama timed out at " + config_.host);
        }
        return Error(ErrorCode::NETWORK_ERROR,
            "Cannot connect to Ollama at " + config_.host +
            ": " + curl_easy_strerror(res));
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (http_code != 200) {
        cleanup_curl();
        if (http_code == 404) {
            return Error(ErrorCode::MODEL_NOT_FOUND,
                "Ollama model not found");
        }
        return Error(ErrorCode::IO_ERROR,
            "Ollama returned HTTP " + std::to_string(http_code));
    }

    initialized_ = true;
    return {};
}

void OllamaProvider::shutdown() {
    cleanup_curl();
    initialized_ = false;
}

bool OllamaProvider::is_available() const {
    return initialized_ && curl_handle_ != nullptr;
}

size_t OllamaProvider::write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* state = static_cast<OllamaStreamState*>(userdata);
    size_t total_size = size * nmemb;

    // Check for abort
    if (state->abort_flag && state->abort_flag->load()) {
        return 0;  // Abort transfer
    }

    state->buffer.append(ptr, total_size);

    // Process complete JSON lines (Ollama sends NDJSON)
    size_t pos;
    while ((pos = state->buffer.find('\n')) != std::string::npos) {
        std::string line = state->buffer.substr(0, pos);
        state->buffer.erase(0, pos + 1);

        if (line.empty()) continue;

        try {
            auto j = json::parse(line);

            if (j.contains("error")) {
                state->error_occurred = true;
                state->error_message = j["error"].get<std::string>();
                return 0;
            }

            if (j.contains("response")) {
                std::string text = j["response"].get<std::string>();
                state->accumulated_content += text;

                if (state->callback && !state->callback(text)) {
                    return 0;  // User aborted
                }
            }

            // Token counts from final response
            if (j.contains("done") && j["done"].get<bool>()) {
                if (j.contains("prompt_eval_count")) {
                    state->prompt_tokens = j["prompt_eval_count"].get<int>();
                }
                if (j.contains("eval_count")) {
                    state->completion_tokens = j["eval_count"].get<int>();
                }
            }
        } catch (const json::exception&) {
            // Ignore parse errors
        }
    }

    return total_size;
}

std::string OllamaProvider::build_request_body(const CompletionRequest& request, bool stream) {
    json body;
    body["model"] = config_.model;
    body["stream"] = stream;

    json options;
    options["temperature"] = request.temperature;
    options["top_p"] = request.top_p;
    options["top_k"] = request.top_k;
    options["num_predict"] = request.max_tokens;
    options["repeat_penalty"] = request.repeat_penalty;

    if (!request.stop_sequences.empty()) {
        options["stop"] = request.stop_sequences;
    }

    body["options"] = options;

    if (config_.keep_alive) {
        body["keep_alive"] = "5m";
    }

    // Build messages array for chat endpoint
    json messages = json::array();

    if (!request.system_prompt.empty()) {
        messages.push_back({{"role", "system"}, {"content", request.system_prompt}});
    }

    for (const auto& msg : request.messages) {
        std::string role;
        switch (msg.role) {
            case Message::Role::SYSTEM:
                role = "system";
                break;
            case Message::Role::USER:
                role = "user";
                break;
            case Message::Role::ASSISTANT:
                role = "assistant";
                break;
        }
        messages.push_back({{"role", role}, {"content", msg.content}});
    }

    body["messages"] = messages;

    return body.dump();
}

Result<CompletionResult> OllamaProvider::complete(const CompletionRequest& request) {
    if (!is_available()) {
        return Error(ErrorCode::INVALID_ARGUMENT, "Provider not available");
    }

    reset_abort();
    auto start_time = std::chrono::steady_clock::now();

    bool use_streaming = (request.on_chunk != nullptr);
    std::string request_body = build_request_body(request, use_streaming);

    CURL* curl = static_cast<CURL*>(curl_handle_);
    curl_easy_reset(curl);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    std::string url = config_.host + "/api/chat";

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(config_.timeout_ms));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CompletionResult result;

    if (use_streaming) {
        OllamaStreamState state;
        state.callback = request.on_chunk;
        state.abort_flag = &abort_requested_;

        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);

        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);

        if (abort_requested_.load()) {
            result.stop_reason = "aborted";
            result.content = state.accumulated_content;
            return result;
        }

        if (res != CURLE_OK) {
            if (res == CURLE_OPERATION_TIMEDOUT) {
                return Error(ErrorCode::TIMEOUT, "Request timed out");
            }
            return Error(ErrorCode::NETWORK_ERROR,
                std::string("Network error: ") + curl_easy_strerror(res));
        }

        if (state.error_occurred) {
            return Error(ErrorCode::INTERNAL_ERROR, state.error_message);
        }

        result.content = state.accumulated_content;
        result.prompt_tokens = state.prompt_tokens;
        result.completion_tokens = state.completion_tokens;
        result.stop_reason = "eos";
    } else {
        ResponseBuffer response;
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, buffer_write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);

        if (res != CURLE_OK) {
            if (res == CURLE_OPERATION_TIMEDOUT) {
                return Error(ErrorCode::TIMEOUT, "Request timed out");
            }
            return Error(ErrorCode::NETWORK_ERROR,
                std::string("Network error: ") + curl_easy_strerror(res));
        }

        try {
            auto j = json::parse(response.data);

            if (j.contains("error")) {
                return Error(ErrorCode::INTERNAL_ERROR,
                    j["error"].get<std::string>());
            }

            if (j.contains("message") && j["message"].contains("content")) {
                result.content = j["message"]["content"].get<std::string>();
            }

            if (j.contains("prompt_eval_count")) {
                result.prompt_tokens = j["prompt_eval_count"].get<int>();
            }
            if (j.contains("eval_count")) {
                result.completion_tokens = j["eval_count"].get<int>();
            }

            result.stop_reason = "eos";
        } catch (const json::exception& e) {
            return Error(ErrorCode::CORRUPTION,
                std::string("Failed to parse response: ") + e.what());
        }
    }

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

void OllamaProvider::abort() {
    abort_requested_ = true;
}

bool OllamaProvider::is_aborted() const {
    return abort_requested_;
}

void OllamaProvider::reset_abort() {
    abort_requested_ = false;
}

Result<std::vector<std::string>> OllamaProvider::list_models() {
    if (!is_available()) {
        return Error(ErrorCode::INVALID_ARGUMENT, "Provider not available");
    }

    CURL* curl = static_cast<CURL*>(curl_handle_);
    curl_easy_reset(curl);

    std::string url = config_.host + "/api/tags";
    ResponseBuffer response;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, buffer_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        if (res == CURLE_OPERATION_TIMEDOUT) {
            return Error(ErrorCode::TIMEOUT, "Request timed out");
        }
        return Error(ErrorCode::NETWORK_ERROR,
            std::string("Network error: ") + curl_easy_strerror(res));
    }

    try {
        auto j = json::parse(response.data);
        std::vector<std::string> models;

        if (j.contains("models")) {
            for (const auto& model : j["models"]) {
                if (model.contains("name")) {
                    models.push_back(model["name"].get<std::string>());
                }
            }
        }

        return models;
    } catch (const json::exception& e) {
        return Error(ErrorCode::CORRUPTION,
            std::string("Failed to parse response: ") + e.what());
    }
}

Result<void> OllamaProvider::pull_model(const std::string& model_name) {
    if (!curl_handle_) {
        init_curl();
    }

    CURL* curl = static_cast<CURL*>(curl_handle_);
    curl_easy_reset(curl);

    json body;
    body["name"] = model_name;

    std::string request_body = body.dump();
    std::string url = config_.host + "/api/pull";

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    ResponseBuffer response;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, buffer_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 0L);  // No timeout for pulls
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        if (res == CURLE_OPERATION_TIMEDOUT) {
            return Error(ErrorCode::TIMEOUT, "Model pull timed out");
        }
        return Error(ErrorCode::NETWORK_ERROR,
            std::string("Network error: ") + curl_easy_strerror(res));
    }

    return {};
}

Result<void> OllamaProvider::preload_model() {
    // Send a minimal request to load model into memory
    CompletionRequest req;
    req.messages = {Message::user("Hi")};
    req.max_tokens = 1;

    auto result = complete(req);
    if (!result.ok()) {
        return result.error();
    }

    return {};
}

Result<std::unique_ptr<OllamaProvider>> OllamaProvider::create(OllamaConfig config) {
    auto provider = std::make_unique<OllamaProvider>(std::move(config));
    auto init_result = provider->initialize();
    if (!init_result.ok()) {
        return init_result.error();
    }
    return provider;
}

Result<std::unique_ptr<OllamaProvider>> OllamaProvider::create_from_env() {
    OllamaConfig config;

    if (const char* host = std::getenv("DAM_OLLAMA_HOST")) {
        config.host = host;
    }

    if (const char* model = std::getenv("DAM_OLLAMA_MODEL")) {
        config.model = model;
    }

    return create(std::move(config));
}

}  // namespace dam::llm
