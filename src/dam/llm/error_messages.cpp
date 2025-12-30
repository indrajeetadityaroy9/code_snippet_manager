#include <dam/llm/error_messages.hpp>

#include <sstream>
#include <algorithm>

namespace dam::llm {

std::string ErrorMessages::ollama_not_running(const std::string& host) {
    std::ostringstream ss;
    ss << "LLM Error: Cannot connect to Ollama at " << host << "\n"
       << "\n"
       << "To get started with LLM-powered features:\n"
       << "\n"
       << "  1. Install Ollama:\n"
       << "     macOS:  brew install ollama\n"
       << "     Linux:  curl -fsSL https://ollama.com/install.sh | sh\n"
       << "\n"
       << "  2. Start the Ollama server:\n"
       << "     ollama serve\n"
       << "\n"
       << "  3. Pull a code completion model:\n"
       << "     ollama pull codellama:7b-instruct\n"
       << "\n"
       << "  4. Use DAM with interactive mode:\n"
       << "     dam add -i -n \"my-snippet\"\n"
       << "\n"
       << recommended_models_list();
    return ss.str();
}

std::string ErrorMessages::no_models_installed() {
    std::ostringstream ss;
    ss << "LLM Error: No models installed in Ollama\n"
       << "\n"
       << "Pull a model to enable LLM features:\n"
       << "\n"
       << "  ollama pull codellama:7b-instruct\n"
       << "\n"
       << recommended_models_list();
    return ss.str();
}

std::string ErrorMessages::model_not_found(
    const std::string& model_name,
    const std::vector<ModelInfo>& available_models) {

    std::ostringstream ss;
    ss << "LLM Error: Model '" << model_name << "' not found\n"
       << "\n"
       << "To install this model:\n"
       << "  ollama pull " << model_name << "\n";

    if (!available_models.empty()) {
        ss << "\n"
           << "Available models:\n"
           << format_model_list(available_models);
    }

    return ss.str();
}

std::string ErrorMessages::connection_error(const std::string& error_detail) {
    std::ostringstream ss;
    ss << "LLM Error: " << error_detail << "\n"
       << "\n"
       << "Troubleshooting:\n"
       << "  - Check if Ollama is running: ollama list\n"
       << "  - Restart Ollama: ollama serve\n"
       << "  - Check firewall settings\n";
    return ss.str();
}

std::string ErrorMessages::setup_help() {
    std::ostringstream ss;
    ss << "DAM LLM Setup Guide\n"
       << "==================\n"
       << "\n"
       << "DAM uses Ollama for local LLM-powered code completion.\n"
       << "\n"
       << "Quick Start:\n"
       << "  1. Install: brew install ollama (macOS)\n"
       << "  2. Start:   ollama serve\n"
       << "  3. Pull:    ollama pull codellama:7b-instruct\n"
       << "  4. Use:     dam add -i -n \"snippet-name\"\n"
       << "\n"
       << "Environment Variables:\n"
       << "  DAM_OLLAMA_MODEL  - Override default model selection\n"
       << "  DAM_OLLAMA_HOST   - Override Ollama server URL\n"
       << "\n"
       << "Example:\n"
       << "  DAM_OLLAMA_MODEL=deepseek-coder:6.7b dam add -i\n"
       << "\n"
       << recommended_models_list();
    return ss.str();
}

std::string ErrorMessages::recommended_models_list() {
    std::ostringstream ss;
    ss << "Recommended models for code completion:\n"
       << "  codellama:7b-instruct   - 4GB, fast, good for general code\n"
       << "  deepseek-coder:6.7b     - 4GB, excellent code understanding\n"
       << "  qwen2.5-coder:7b        - 4GB, multilingual support\n"
       << "  starcoder2:7b           - 4GB, trained on code repositories\n"
       << "  codegemma:7b            - 5GB, Google's code model\n";
    return ss.str();
}

std::string ErrorMessages::format_model_list(
    const std::vector<ModelInfo>& models,
    size_t max_display) {

    std::ostringstream ss;

    size_t count = std::min(models.size(), max_display);
    for (size_t i = 0; i < count; ++i) {
        const auto& model = models[i];
        ss << "  " << model.name;
        if (!model.size.empty()) {
            ss << " (" << model.size << ")";
        }
        if (model.is_code_model) {
            ss << " [code]";
        }
        ss << "\n";
    }

    if (models.size() > max_display) {
        ss << "  ... and " << (models.size() - max_display) << " more\n";
    }

    return ss.str();
}

}  // namespace dam::llm
