#include <dam/llm/model_discovery.hpp>

#include <nlohmann/json.hpp>
#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <iomanip>

namespace dam::llm {

namespace {

// CURL write callback
size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* response = static_cast<std::string*>(userdata);
    response->append(ptr, size * nmemb);
    return size * nmemb;
}

// Convert string to lowercase
std::string to_lower(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return std::tolower(c); });
    return result;
}

}  // namespace

DiscoveryResult ModelDiscovery::discover_ollama(
    const std::string& host,
    int timeout_ms) {

    DiscoveryResult result;
    result.ollama_running = false;

    // Initialize CURL
    CURL* curl = curl_easy_init();
    if (!curl) {
        result.error_message = "Failed to initialize HTTP client";
        return result;
    }

    std::string response;
    std::string url = host + "/api/tags";

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeout_ms));

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        curl_easy_cleanup(curl);
        if (res == CURLE_OPERATION_TIMEDOUT || res == CURLE_COULDNT_CONNECT) {
            result.error_message = "Cannot connect to Ollama at " + host;
        } else {
            result.error_message = "Network error: " + std::string(curl_easy_strerror(res));
        }
        return result;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (http_code != 200) {
        result.error_message = "Ollama returned error code " + std::to_string(http_code);
        return result;
    }

    // Parse JSON response
    try {
        auto json = nlohmann::json::parse(response);
        result.ollama_running = true;

        if (json.contains("models") && json["models"].is_array()) {
            for (const auto& model : json["models"]) {
                ModelInfo info;

                if (model.contains("name")) {
                    info.name = model["name"].get<std::string>();
                }

                if (model.contains("size")) {
                    uint64_t bytes = model["size"].get<uint64_t>();
                    info.size = format_size(bytes);
                }

                if (model.contains("details") && model["details"].contains("family")) {
                    info.family = model["details"]["family"].get<std::string>();
                } else {
                    info.family = extract_family(info.name);
                }

                if (model.contains("modified_at")) {
                    info.modified_at = model["modified_at"].get<std::string>();
                }

                info.is_code_model = is_code_model(info.name);

                result.available_models.push_back(info);
            }
        }

        // Sort: code models first, then by name
        std::sort(result.available_models.begin(), result.available_models.end(),
            [](const ModelInfo& a, const ModelInfo& b) {
                if (a.is_code_model != b.is_code_model) {
                    return a.is_code_model > b.is_code_model;
                }
                return a.name < b.name;
            });

        // Set recommended model (first code model, or first model)
        for (const auto& model : result.available_models) {
            if (model.is_code_model) {
                result.recommended_model = model.name;
                break;
            }
        }
        if (result.recommended_model.empty() && !result.available_models.empty()) {
            result.recommended_model = result.available_models[0].name;
        }

    } catch (const nlohmann::json::exception& e) {
        result.ollama_running = true;  // Server responded, just couldn't parse
        result.error_message = "Failed to parse model list: " + std::string(e.what());
    }

    return result;
}

bool ModelDiscovery::is_code_model(const std::string& model_name) {
    std::string lower = to_lower(model_name);

    // Exact family matches
    static const std::vector<std::string> code_families = {
        "codellama",
        "starcoder",
        "codegemma",
        "codestral",
        "deepseek-coder",
        "deepseek-v2.5",  // DeepSeek V2.5 is optimized for code
        "granite-code",
        "stable-code",
    };

    for (const auto& family : code_families) {
        if (lower.find(family) != std::string::npos) {
            return true;
        }
    }

    // Pattern matches
    if (lower.find("coder") != std::string::npos) return true;
    if (lower.find("-code") != std::string::npos) return true;
    if (lower.find("code-") != std::string::npos) return true;

    // Qwen coder variants
    if (lower.find("qwen") != std::string::npos &&
        lower.find("coder") != std::string::npos) {
        return true;
    }

    return false;
}

std::vector<std::string> ModelDiscovery::recommended_code_models() {
    return {
        "codellama:7b-instruct",
        "deepseek-coder:6.7b",
        "qwen2.5-coder:7b",
        "codellama:13b-instruct",
        "starcoder2:7b",
        "codegemma:7b",
    };
}

std::string ModelDiscovery::format_size(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit_index = 0;
    double size = static_cast<double>(bytes);

    while (size >= 1024.0 && unit_index < 4) {
        size /= 1024.0;
        unit_index++;
    }

    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1) << size << " " << units[unit_index];
    return ss.str();
}

std::string ModelDiscovery::extract_family(const std::string& model_name) {
    // Extract base name before colon (e.g., "codellama" from "codellama:7b-instruct")
    auto colon_pos = model_name.find(':');
    std::string base = (colon_pos != std::string::npos)
        ? model_name.substr(0, colon_pos)
        : model_name;

    // Remove version/size suffixes
    auto dash_pos = base.rfind('-');
    if (dash_pos != std::string::npos) {
        std::string suffix = base.substr(dash_pos + 1);
        // Check if suffix is a version number
        bool is_version = !suffix.empty() &&
            (std::isdigit(suffix[0]) || suffix == "latest");
        if (is_version) {
            base = base.substr(0, dash_pos);
        }
    }

    return base;
}

}  // namespace dam::llm
