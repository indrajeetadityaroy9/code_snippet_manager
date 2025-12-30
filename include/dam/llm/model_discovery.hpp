#pragma once

#include <dam/result.hpp>

#include <string>
#include <vector>

namespace dam::llm {

// ============================================================================
// Model Information
// ============================================================================

struct ModelInfo {
    std::string name;           // e.g., "codellama:7b-instruct"
    std::string size;           // e.g., "4.7 GB"
    std::string family;         // e.g., "codellama", "llama", "qwen"
    std::string modified_at;    // ISO timestamp
    bool is_code_model = false; // Inferred from name patterns
};

// ============================================================================
// Discovery Result
// ============================================================================

struct DiscoveryResult {
    bool ollama_running = false;
    std::vector<ModelInfo> available_models;
    std::string recommended_model;  // Best model for code completion
    std::string error_message;      // If ollama_running is false
};

// ============================================================================
// Model Discovery
// ============================================================================

class ModelDiscovery {
public:
    /**
     * Discover Ollama availability and list installed models.
     *
     * @param host Ollama server URL (default: http://localhost:11434)
     * @param timeout_ms Connection timeout in milliseconds
     * @return Discovery result with available models or error
     */
    static DiscoveryResult discover_ollama(
        const std::string& host = "http://localhost:11434",
        int timeout_ms = 5000);

    /**
     * Check if a model name suggests it's optimized for code.
     *
     * Matches patterns like:
     * - codellama, deepseek-coder, qwen*-coder, starcoder
     * - *-code, *code*, coder*
     *
     * @param model_name The model name to check
     * @return true if the model appears to be code-focused
     */
    static bool is_code_model(const std::string& model_name);

    /**
     * Get a list of recommended code completion models.
     *
     * These are models known to work well for code completion tasks.
     */
    static std::vector<std::string> recommended_code_models();

    /**
     * Format model size from bytes to human-readable string.
     *
     * @param bytes Size in bytes
     * @return Formatted string like "4.7 GB"
     */
    static std::string format_size(uint64_t bytes);

    /**
     * Extract model family from model name.
     *
     * @param model_name Full model name (e.g., "codellama:7b-instruct")
     * @return Family name (e.g., "codellama")
     */
    static std::string extract_family(const std::string& model_name);
};

}  // namespace dam::llm
