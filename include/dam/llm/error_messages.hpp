#pragma once

#include <dam/llm/model_discovery.hpp>

#include <string>
#include <vector>

namespace dam::llm {

// ============================================================================
// Error Message Builder
// ============================================================================

/**
 * Provides user-friendly error messages with setup instructions
 * for common LLM configuration issues.
 */
class ErrorMessages {
public:
    /**
     * Get error message when Ollama is not running or unreachable.
     *
     * @param host The Ollama host URL that was attempted
     * @return Formatted error message with setup instructions
     */
    static std::string ollama_not_running(const std::string& host = "http://localhost:11434");

    /**
     * Get error message when Ollama is running but no models are installed.
     *
     * @return Formatted error message with model installation instructions
     */
    static std::string no_models_installed();

    /**
     * Get error message when the specified model is not found.
     *
     * @param model_name The model that was not found
     * @param available_models List of models that are available
     * @return Formatted error message with suggestions
     */
    static std::string model_not_found(
        const std::string& model_name,
        const std::vector<ModelInfo>& available_models);

    /**
     * Get error message for generic LLM connection errors.
     *
     * @param error_detail Technical error details
     * @return Formatted error message
     */
    static std::string connection_error(const std::string& error_detail);

    /**
     * Get general setup help for LLM features.
     *
     * @return Formatted help message with Ollama setup instructions
     */
    static std::string setup_help();

    /**
     * Get list of recommended models for code completion.
     *
     * @return Formatted list of recommended models
     */
    static std::string recommended_models_list();

    /**
     * Format available models as a displayable list.
     *
     * @param models List of available models
     * @param max_display Maximum number of models to show
     * @return Formatted model list
     */
    static std::string format_model_list(
        const std::vector<ModelInfo>& models,
        size_t max_display = 5);
};

}  // namespace dam::llm
