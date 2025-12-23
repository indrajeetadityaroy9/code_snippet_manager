#pragma once

#include "terminal.hpp"
#include "debouncer.hpp"
#include "../llm/claude_client.hpp"
#include "../llm/input_classifier.hpp"

#include <memory>
#include <string>
#include <vector>

namespace dam::cli {

/**
 * Configuration for interactive editor.
 */
struct InteractiveEditorConfig {
    int debounce_ms = 450;              // Debounce delay before API calls
    int suggestion_timeout_ms = 10000;  // Timeout for waiting on suggestions
    bool show_status_bar = true;        // Show status bar with hints
    bool show_line_numbers = false;     // Show line numbers
    std::string language_hint;          // Pre-set language for completions
};

/**
 * Result of an interactive editing session.
 */
struct EditorResult {
    std::string content;               // Final content
    bool accepted = false;             // User accepted (Ctrl+Enter) vs cancelled
    std::string detected_language;     // Auto-detected language
};

/**
 * Interactive code editor with LLM-assisted autocomplete.
 *
 * Provides:
 * - Raw terminal editing with arrow keys, backspace, etc.
 * - Syntax autocomplete (gray ghost text, Tab to accept)
 * - Natural language to code generation
 * - Input debouncing to avoid excessive API calls
 *
 * Usage:
 *   auto client = ClaudeClient::from_environment();
 *   if (!client) {
 *       // Fall back to external editor
 *       return;
 *   }
 *
 *   InteractiveEditor editor(std::move(*client));
 *   auto result = editor.run();
 *   if (result.accepted) {
 *       // Use result.content
 *   }
 */
class InteractiveEditor {
public:
    /**
     * Create editor with Claude client.
     */
    explicit InteractiveEditor(ClaudeClient client,
                               InteractiveEditorConfig config = {});

    /**
     * Check if interactive mode is available.
     */
    bool is_available() const;

    /**
     * Run the interactive editor.
     * Blocks until user completes or cancels.
     * @return Editor result with content and acceptance status
     */
    EditorResult run();

    /**
     * Run with initial content.
     */
    EditorResult run(const std::string& initial_content);

    /**
     * Set language hint for completions.
     */
    void set_language(const std::string& language);

    /**
     * Get current configuration.
     */
    const InteractiveEditorConfig& config() const { return config_; }

private:
    ClaudeClient client_;
    InteractiveEditorConfig config_;
    std::unique_ptr<Terminal> terminal_;
    Debouncer debouncer_;

    // Editor state
    std::vector<std::string> lines_;
    int cursor_row_ = 0;
    int cursor_col_ = 0;
    int scroll_offset_ = 0;       // Vertical scroll for long content

    // Suggestion state
    std::string current_suggestion_;
    bool suggestion_visible_ = false;
    bool fetching_suggestion_ = false;

    // Display state
    int editor_height_ = 0;       // Available lines for editor
    int editor_width_ = 0;        // Available columns

    // Input classification
    InputType last_classification_ = InputType::EMPTY;

    // Core editing operations
    void insert_char(char c);
    void insert_newline();
    void delete_char_backward();      // Backspace
    void delete_char_forward();       // Delete key
    void delete_line();
    void move_cursor_left();
    void move_cursor_right();
    void move_cursor_up();
    void move_cursor_down();
    void move_to_line_start();
    void move_to_line_end();
    void move_word_left();
    void move_word_right();

    // Suggestion handling
    void request_suggestion();
    void accept_suggestion();
    void dismiss_suggestion();

    // Rendering
    void render();
    void render_header();
    void render_content();
    void render_suggestion_overlay();
    void render_status_bar();
    void position_cursor();

    // Helpers
    std::string get_content() const;
    std::string get_content_before_cursor() const;
    void set_content(const std::string& content);
    std::string detect_language() const;
    void ensure_cursor_visible();

    // Scroll management
    void scroll_up();
    void scroll_down();
    void scroll_to_cursor();
};

}  // namespace dam::cli
