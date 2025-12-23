#include "interactive_editor.hpp"

#include <dam/language_detector.hpp>

#include <algorithm>
#include <sstream>

namespace dam::cli {

InteractiveEditor::InteractiveEditor(ClaudeClient client,
                                     InteractiveEditorConfig config)
    : client_(std::move(client))
    , config_(std::move(config))
    , debouncer_(config_.debounce_ms) {
    lines_.push_back("");
}

bool InteractiveEditor::is_available() const {
    return Terminal::is_tty() && client_.is_available();
}

void InteractiveEditor::set_language(const std::string& language) {
    config_.language_hint = language;
}

EditorResult InteractiveEditor::run() {
    return run("");
}

EditorResult InteractiveEditor::run(const std::string& initial_content) {
    if (!is_available()) {
        return {initial_content, false, ""};
    }

    terminal_ = std::make_unique<Terminal>();

    // Get terminal size
    auto [cols, rows] = Terminal::get_size();
    editor_width_ = cols;
    editor_height_ = rows - 3;  // Reserve lines for header and status

    set_content(initial_content);
    render();

    EditorResult result;
    result.accepted = false;

    bool running = true;
    while (running) {
        // Calculate timeout based on debouncer state
        int timeout = 50;  // Base poll interval
        if (debouncer_.is_pending()) {
            timeout = std::min(timeout, debouncer_.remaining_ms() + 1);
        }

        auto key_event = terminal_->read_key(timeout);

        if (!key_event) {
            // Timeout - check if we should fetch suggestion
            if (debouncer_.ready() && !fetching_suggestion_ && !suggestion_visible_) {
                request_suggestion();
                render();
            }
            continue;
        }

        // Clear suggestion on most inputs (except Tab and navigation)
        bool should_clear_suggestion = suggestion_visible_ &&
            key_event->key != Key::TAB &&
            key_event->key != Key::ESCAPE;

        if (should_clear_suggestion) {
            dismiss_suggestion();
        }

        switch (key_event->key) {
            case Key::CHAR:
                if (key_event->ctrl) {
                    // Handle Ctrl+key combinations
                    if (key_event->ch == 'c') {
                        // Ctrl+C - cancel
                        running = false;
                        result.accepted = false;
                    } else if (key_event->ch == 'd') {
                        // Ctrl+D - submit if content exists, else cancel
                        if (!get_content().empty()) {
                            running = false;
                            result.accepted = true;
                        } else {
                            running = false;
                            result.accepted = false;
                        }
                    } else if (key_event->ch == 's') {
                        // Ctrl+S - submit
                        running = false;
                        result.accepted = true;
                    }
                } else {
                    insert_char(key_event->ch);
                    debouncer_.trigger();
                }
                break;

            case Key::ENTER:
                insert_newline();
                debouncer_.trigger();
                break;

            case Key::CTRL_ENTER:
                // Submit
                running = false;
                result.accepted = true;
                break;

            case Key::TAB:
                if (suggestion_visible_) {
                    accept_suggestion();
                } else {
                    // Insert spaces for tab
                    for (int i = 0; i < 4; ++i) {
                        insert_char(' ');
                    }
                    debouncer_.trigger();
                }
                break;

            case Key::BACKSPACE:
                delete_char_backward();
                debouncer_.trigger();
                break;

            case Key::DELETE_KEY:
                delete_char_forward();
                debouncer_.trigger();
                break;

            case Key::LEFT:
                if (key_event->ctrl) {
                    move_word_left();
                } else {
                    move_cursor_left();
                }
                break;

            case Key::RIGHT:
                if (key_event->ctrl) {
                    move_word_right();
                } else {
                    move_cursor_right();
                }
                break;

            case Key::UP:
                move_cursor_up();
                break;

            case Key::DOWN:
                move_cursor_down();
                break;

            case Key::HOME:
                move_to_line_start();
                break;

            case Key::END:
                move_to_line_end();
                break;

            case Key::ESCAPE:
                if (suggestion_visible_) {
                    dismiss_suggestion();
                }
                break;

            case Key::CTRL_C:
                running = false;
                result.accepted = false;
                break;

            case Key::CTRL_D:
                if (!get_content().empty()) {
                    running = false;
                    result.accepted = true;
                } else {
                    running = false;
                    result.accepted = false;
                }
                break;

            default:
                break;
        }

        render();
    }

    // Clean up terminal
    terminal_->show_cursor();
    terminal_->clear_screen();
    terminal_->flush();
    terminal_.reset();

    result.content = get_content();
    result.detected_language = detect_language();

    return result;
}

// ============================================================================
// Editing Operations
// ============================================================================

void InteractiveEditor::insert_char(char c) {
    lines_[cursor_row_].insert(cursor_col_, 1, c);
    cursor_col_++;
}

void InteractiveEditor::insert_newline() {
    // Split current line at cursor
    std::string current = lines_[cursor_row_];
    std::string before = current.substr(0, cursor_col_);
    std::string after = current.substr(cursor_col_);

    lines_[cursor_row_] = before;
    lines_.insert(lines_.begin() + cursor_row_ + 1, after);

    cursor_row_++;
    cursor_col_ = 0;

    ensure_cursor_visible();
}

void InteractiveEditor::delete_char_backward() {
    if (cursor_col_ > 0) {
        lines_[cursor_row_].erase(cursor_col_ - 1, 1);
        cursor_col_--;
    } else if (cursor_row_ > 0) {
        // Merge with previous line
        cursor_col_ = static_cast<int>(lines_[cursor_row_ - 1].length());
        lines_[cursor_row_ - 1] += lines_[cursor_row_];
        lines_.erase(lines_.begin() + cursor_row_);
        cursor_row_--;
    }
}

void InteractiveEditor::delete_char_forward() {
    if (cursor_col_ < static_cast<int>(lines_[cursor_row_].length())) {
        lines_[cursor_row_].erase(cursor_col_, 1);
    } else if (cursor_row_ < static_cast<int>(lines_.size()) - 1) {
        // Merge with next line
        lines_[cursor_row_] += lines_[cursor_row_ + 1];
        lines_.erase(lines_.begin() + cursor_row_ + 1);
    }
}

void InteractiveEditor::delete_line() {
    if (lines_.size() > 1) {
        lines_.erase(lines_.begin() + cursor_row_);
        if (cursor_row_ >= static_cast<int>(lines_.size())) {
            cursor_row_ = static_cast<int>(lines_.size()) - 1;
        }
        cursor_col_ = std::min(cursor_col_, static_cast<int>(lines_[cursor_row_].length()));
    } else {
        lines_[0].clear();
        cursor_col_ = 0;
    }
}

void InteractiveEditor::move_cursor_left() {
    if (cursor_col_ > 0) {
        cursor_col_--;
    } else if (cursor_row_ > 0) {
        cursor_row_--;
        cursor_col_ = static_cast<int>(lines_[cursor_row_].length());
    }
}

void InteractiveEditor::move_cursor_right() {
    if (cursor_col_ < static_cast<int>(lines_[cursor_row_].length())) {
        cursor_col_++;
    } else if (cursor_row_ < static_cast<int>(lines_.size()) - 1) {
        cursor_row_++;
        cursor_col_ = 0;
    }
}

void InteractiveEditor::move_cursor_up() {
    if (cursor_row_ > 0) {
        cursor_row_--;
        cursor_col_ = std::min(cursor_col_, static_cast<int>(lines_[cursor_row_].length()));
        ensure_cursor_visible();
    }
}

void InteractiveEditor::move_cursor_down() {
    if (cursor_row_ < static_cast<int>(lines_.size()) - 1) {
        cursor_row_++;
        cursor_col_ = std::min(cursor_col_, static_cast<int>(lines_[cursor_row_].length()));
        ensure_cursor_visible();
    }
}

void InteractiveEditor::move_to_line_start() {
    cursor_col_ = 0;
}

void InteractiveEditor::move_to_line_end() {
    cursor_col_ = static_cast<int>(lines_[cursor_row_].length());
}

void InteractiveEditor::move_word_left() {
    if (cursor_col_ == 0 && cursor_row_ > 0) {
        cursor_row_--;
        cursor_col_ = static_cast<int>(lines_[cursor_row_].length());
        return;
    }

    const std::string& line = lines_[cursor_row_];

    // Skip whitespace
    while (cursor_col_ > 0 && std::isspace(line[cursor_col_ - 1])) {
        cursor_col_--;
    }

    // Skip word characters
    while (cursor_col_ > 0 && !std::isspace(line[cursor_col_ - 1])) {
        cursor_col_--;
    }
}

void InteractiveEditor::move_word_right() {
    const std::string& line = lines_[cursor_row_];

    if (cursor_col_ >= static_cast<int>(line.length())) {
        if (cursor_row_ < static_cast<int>(lines_.size()) - 1) {
            cursor_row_++;
            cursor_col_ = 0;
        }
        return;
    }

    // Skip word characters
    while (cursor_col_ < static_cast<int>(line.length()) && !std::isspace(line[cursor_col_])) {
        cursor_col_++;
    }

    // Skip whitespace
    while (cursor_col_ < static_cast<int>(line.length()) && std::isspace(line[cursor_col_])) {
        cursor_col_++;
    }
}

// ============================================================================
// Suggestion Handling
// ============================================================================

void InteractiveEditor::request_suggestion() {
    if (fetching_suggestion_) return;

    std::string content = get_content_before_cursor();
    if (content.empty()) return;

    // Classify input
    last_classification_ = InputClassifier::classify(content, config_.language_hint);

    if (last_classification_ == InputType::EMPTY) {
        return;
    }

    fetching_suggestion_ = true;
    current_suggestion_.clear();

    // Show "thinking" status
    render_status_bar();
    terminal_->flush();

    auto callback = [this](const std::string& chunk) -> bool {
        current_suggestion_ += chunk;
        suggestion_visible_ = true;
        render();
        terminal_->flush();
        return !client_.is_aborted();
    };

    dam::Result<std::string> result = dam::Error(dam::ErrorCode::INTERNAL_ERROR, "");

    if (last_classification_ == InputType::NATURAL_LANG) {
        result = client_.generate_from_nl(content, config_.language_hint, callback);
    } else {
        result = client_.complete_code(content, config_.language_hint, callback);
    }

    fetching_suggestion_ = false;

    if (result.ok()) {
        current_suggestion_ = result.value();
        suggestion_visible_ = !current_suggestion_.empty();
    } else {
        // Silently fail - don't interrupt the user
        current_suggestion_.clear();
        suggestion_visible_ = false;
    }
}

void InteractiveEditor::accept_suggestion() {
    if (!suggestion_visible_ || current_suggestion_.empty()) return;

    // For natural language mode, replace the content entirely
    if (last_classification_ == InputType::NATURAL_LANG) {
        set_content(current_suggestion_);
    } else {
        // For code completion, insert at cursor
        for (char c : current_suggestion_) {
            if (c == '\n') {
                insert_newline();
            } else {
                insert_char(c);
            }
        }
    }

    dismiss_suggestion();
}

void InteractiveEditor::dismiss_suggestion() {
    current_suggestion_.clear();
    suggestion_visible_ = false;
    fetching_suggestion_ = false;
    client_.abort();
}

// ============================================================================
// Rendering
// ============================================================================

void InteractiveEditor::render() {
    terminal_->hide_cursor();

    render_header();
    render_content();
    if (suggestion_visible_) {
        render_suggestion_overlay();
    }
    render_status_bar();
    position_cursor();

    terminal_->show_cursor();
    terminal_->flush();
}

void InteractiveEditor::render_header() {
    terminal_->move_cursor(0, 0);
    terminal_->clear_line();

    std::string title = "Interactive Snippet Editor";
    if (!config_.language_hint.empty()) {
        title += " [" + config_.language_hint + "]";
    }

    terminal_->write_colored(title, colors::BOLD);
    terminal_->write("\n");

    terminal_->clear_line();
    terminal_->write_colored(std::string(editor_width_, '-'), colors::DIM);
}

void InteractiveEditor::render_content() {
    // Render visible lines
    for (int i = 0; i < editor_height_; ++i) {
        int line_idx = scroll_offset_ + i;
        terminal_->move_cursor(0, 2 + i);
        terminal_->clear_line();

        if (line_idx < static_cast<int>(lines_.size())) {
            const std::string& line = lines_[line_idx];

            // Truncate long lines
            if (static_cast<int>(line.length()) > editor_width_) {
                terminal_->write(line.substr(0, editor_width_ - 1));
                terminal_->write_colored(">", colors::DIM);
            } else {
                terminal_->write(line);
            }
        }
    }
}

void InteractiveEditor::render_suggestion_overlay() {
    if (current_suggestion_.empty()) return;

    // Save position, render suggestion in gray after cursor
    int display_row = cursor_row_ - scroll_offset_ + 2;
    int display_col = cursor_col_;

    terminal_->move_cursor(display_col, display_row);

    // Get just the first line of suggestion for inline display
    std::string first_line;
    size_t newline_pos = current_suggestion_.find('\n');
    if (newline_pos != std::string::npos) {
        first_line = current_suggestion_.substr(0, newline_pos);
        first_line += "...";
    } else {
        first_line = current_suggestion_;
    }

    // Truncate to fit screen
    int available = editor_width_ - display_col;
    if (static_cast<int>(first_line.length()) > available) {
        first_line = first_line.substr(0, available - 3) + "...";
    }

    terminal_->write_colored(first_line, colors::GRAY);
}

void InteractiveEditor::render_status_bar() {
    terminal_->move_cursor(0, editor_height_ + 2);
    terminal_->clear_line();

    std::ostringstream status;

    // Left side: mode/state info
    if (fetching_suggestion_) {
        terminal_->write_colored("[Thinking...]", colors::YELLOW);
    } else if (suggestion_visible_) {
        terminal_->write_colored("[Tab: Accept | Esc: Dismiss]", colors::GREEN);
    } else {
        std::string mode;
        switch (last_classification_) {
            case InputType::CODE: mode = "[Code]"; break;
            case InputType::NATURAL_LANG: mode = "[NL Command]"; break;
            default: mode = "[Ready]"; break;
        }
        terminal_->write_colored(mode, colors::DIM);
    }

    // Right side: position and help
    std::ostringstream right;
    right << "L" << (cursor_row_ + 1) << ":C" << (cursor_col_ + 1);
    right << " | Ctrl+S: Submit | Ctrl+C: Cancel";

    std::string right_str = right.str();
    int padding = editor_width_ - 30 - static_cast<int>(right_str.length());
    if (padding > 0) {
        terminal_->write(std::string(padding, ' '));
    }
    terminal_->write_colored(right_str, colors::DIM);
}

void InteractiveEditor::position_cursor() {
    int display_row = cursor_row_ - scroll_offset_ + 2;
    int display_col = cursor_col_;

    // Clamp to visible area
    display_row = std::max(2, std::min(display_row, editor_height_ + 1));
    display_col = std::max(0, std::min(display_col, editor_width_ - 1));

    terminal_->move_cursor(display_col, display_row);
}

// ============================================================================
// Helpers
// ============================================================================

std::string InteractiveEditor::get_content() const {
    std::ostringstream ss;
    for (size_t i = 0; i < lines_.size(); ++i) {
        ss << lines_[i];
        if (i < lines_.size() - 1) {
            ss << '\n';
        }
    }
    return ss.str();
}

std::string InteractiveEditor::get_content_before_cursor() const {
    std::ostringstream ss;
    for (int i = 0; i < cursor_row_; ++i) {
        ss << lines_[i] << '\n';
    }
    ss << lines_[cursor_row_].substr(0, cursor_col_);
    return ss.str();
}

void InteractiveEditor::set_content(const std::string& content) {
    lines_.clear();

    if (content.empty()) {
        lines_.push_back("");
        cursor_row_ = 0;
        cursor_col_ = 0;
        return;
    }

    std::istringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
        lines_.push_back(line);
    }

    // Handle trailing newline
    if (!content.empty() && content.back() == '\n') {
        lines_.push_back("");
    }

    if (lines_.empty()) {
        lines_.push_back("");
    }

    // Move cursor to end
    cursor_row_ = static_cast<int>(lines_.size()) - 1;
    cursor_col_ = static_cast<int>(lines_[cursor_row_].length());

    ensure_cursor_visible();
}

std::string InteractiveEditor::detect_language() const {
    if (!config_.language_hint.empty()) {
        return config_.language_hint;
    }
    return dam::LanguageDetector::detect(get_content(), "");
}

void InteractiveEditor::ensure_cursor_visible() {
    scroll_to_cursor();
}

void InteractiveEditor::scroll_up() {
    if (scroll_offset_ > 0) {
        scroll_offset_--;
    }
}

void InteractiveEditor::scroll_down() {
    int max_scroll = std::max(0, static_cast<int>(lines_.size()) - editor_height_);
    if (scroll_offset_ < max_scroll) {
        scroll_offset_++;
    }
}

void InteractiveEditor::scroll_to_cursor() {
    // Ensure cursor row is visible
    if (cursor_row_ < scroll_offset_) {
        scroll_offset_ = cursor_row_;
    } else if (cursor_row_ >= scroll_offset_ + editor_height_) {
        scroll_offset_ = cursor_row_ - editor_height_ + 1;
    }
}

}  // namespace dam::cli
