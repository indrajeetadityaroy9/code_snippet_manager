#pragma once

#include "terminal.hpp"
#include <dam/llm/model_discovery.hpp>

#include <optional>
#include <string>
#include <vector>

namespace dam::cli {

/**
 * Interactive terminal UI for selecting an Ollama model.
 *
 * Displays available models with arrow key navigation,
 * highlighting code-optimized models as recommended.
 *
 * UI Example:
 * ┌─ Select Ollama Model ──────────────────────────────┐
 * │                                                    │
 * │ ▸ codellama:7b-instruct   4.7 GB (recommended)    │
 * │   deepseek-coder:6.7b     4.1 GB                  │
 * │   qwen2.5:1.5b-instruct   1.2 GB                  │
 * │                                                    │
 * │ ↑/↓ Navigate  Enter: Select  Esc: Cancel          │
 * └────────────────────────────────────────────────────┘
 */
class ModelPicker {
public:
    /**
     * Create picker with list of available models.
     */
    explicit ModelPicker(const std::vector<dam::llm::ModelInfo>& models);

    /**
     * Show the picker UI and wait for user selection.
     *
     * @return Selected model name, or nullopt if cancelled
     */
    std::optional<std::string> pick();

    /**
     * Auto-select the best model without showing UI.
     *
     * Selection priority:
     * 1. First code-optimized model
     * 2. First available model
     *
     * @param models Available models
     * @return Recommended model name, or nullopt if no models
     */
    static std::optional<std::string> auto_select(
        const std::vector<dam::llm::ModelInfo>& models);

private:
    std::vector<dam::llm::ModelInfo> models_;
    int selected_index_ = 0;
    bool running_ = false;

    // UI dimensions
    static constexpr int BOX_WIDTH = 60;
    static constexpr int MIN_HEIGHT = 5;
    static constexpr int MAX_VISIBLE_MODELS = 8;

    // Rendering
    void render(Terminal& term);
    void render_header(Terminal& term, int start_row);
    void render_models(Terminal& term, int start_row);
    void render_footer(Terminal& term, int row);
    std::string truncate_or_pad(const std::string& s, size_t width) const;

    // Navigation
    void move_up();
    void move_down();
};

}  // namespace dam::cli
