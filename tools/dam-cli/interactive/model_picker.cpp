#include "model_picker.hpp"

#include <algorithm>
#include <sstream>
#include <iomanip>

namespace dam::cli {

ModelPicker::ModelPicker(const std::vector<dam::llm::ModelInfo>& models)
    : models_(models) {
    // Start with first code model selected, if any
    for (size_t i = 0; i < models_.size(); ++i) {
        if (models_[i].is_code_model) {
            selected_index_ = static_cast<int>(i);
            break;
        }
    }
}

std::optional<std::string> ModelPicker::pick() {
    if (models_.empty()) {
        return std::nullopt;
    }

    // Single model - auto-select
    if (models_.size() == 1) {
        return models_[0].name;
    }

    if (!Terminal::is_tty()) {
        // Non-interactive mode - auto-select best
        return auto_select(models_);
    }

    try {
        Terminal term;
        running_ = true;

        // Initial render
        term.hide_cursor();
        render(term);

        while (running_) {
            auto key = term.read_key(100);  // 100ms timeout for responsiveness
            if (!key) continue;

            switch (key->key) {
                case Key::UP:
                    move_up();
                    render(term);
                    break;

                case Key::DOWN:
                    move_down();
                    render(term);
                    break;

                case Key::ENTER:
                    running_ = false;
                    term.show_cursor();
                    term.write("\n");
                    return models_[selected_index_].name;

                case Key::ESCAPE:
                case Key::CTRL_C:
                case Key::CTRL_D:
                    running_ = false;
                    term.show_cursor();
                    term.write("\n");
                    return std::nullopt;

                default:
                    break;
            }
        }

        term.show_cursor();
        return std::nullopt;

    } catch (const std::exception&) {
        // Terminal error - fall back to auto-select
        return auto_select(models_);
    }
}

std::optional<std::string> ModelPicker::auto_select(
    const std::vector<dam::llm::ModelInfo>& models) {

    if (models.empty()) {
        return std::nullopt;
    }

    // Prefer code-optimized models
    for (const auto& model : models) {
        if (model.is_code_model) {
            return model.name;
        }
    }

    // Fall back to first model
    return models[0].name;
}

void ModelPicker::render(Terminal& term) {
    auto [cols, rows] = Terminal::get_size();

    // Calculate UI position (centered)
    int ui_height = std::min(static_cast<int>(models_.size()), MAX_VISIBLE_MODELS) + 4;
    int start_row = (rows - ui_height) / 2;
    int start_col = (cols - BOX_WIDTH) / 2;

    if (start_row < 0) start_row = 0;
    if (start_col < 0) start_col = 0;

    // Clear area and render
    term.save_cursor();

    render_header(term, start_row);
    render_models(term, start_row + 2);
    render_footer(term, start_row + ui_height - 1);

    term.restore_cursor();
    term.flush();
}

void ModelPicker::render_header(Terminal& term, int start_row) {
    auto [cols, _] = Terminal::get_size();
    int start_col = (cols - BOX_WIDTH) / 2;
    if (start_col < 0) start_col = 0;

    term.move_cursor(start_col, start_row);

    // Top border with title
    std::string title = " Select Ollama Model ";
    int left_pad = (BOX_WIDTH - 2 - static_cast<int>(title.size())) / 2;
    int right_pad = BOX_WIDTH - 2 - left_pad - static_cast<int>(title.size());

    std::ostringstream ss;
    ss << colors::CYAN << "\u250C";
    for (int i = 0; i < left_pad; ++i) ss << "\u2500";
    ss << colors::BOLD << title << colors::RESET << colors::CYAN;
    for (int i = 0; i < right_pad; ++i) ss << "\u2500";
    ss << "\u2510" << colors::RESET;
    term.write(ss.str());

    // Empty line
    term.move_cursor(start_col, start_row + 1);
    std::ostringstream empty;
    empty << colors::CYAN << "\u2502" << colors::RESET;
    empty << std::string(BOX_WIDTH - 2, ' ');
    empty << colors::CYAN << "\u2502" << colors::RESET;
    term.write(empty.str());
}

void ModelPicker::render_models(Terminal& term, int start_row) {
    auto [cols, _] = Terminal::get_size();
    int start_col = (cols - BOX_WIDTH) / 2;
    if (start_col < 0) start_col = 0;

    // Calculate visible range for scrolling
    int visible_count = std::min(static_cast<int>(models_.size()), MAX_VISIBLE_MODELS);
    int scroll_offset = 0;

    if (selected_index_ >= visible_count) {
        scroll_offset = selected_index_ - visible_count + 1;
    }

    for (int i = 0; i < visible_count; ++i) {
        int model_idx = i + scroll_offset;
        if (model_idx >= static_cast<int>(models_.size())) break;

        const auto& model = models_[model_idx];
        bool is_selected = (model_idx == selected_index_);

        term.move_cursor(start_col, start_row + i);

        std::ostringstream line;
        line << colors::CYAN << "\u2502" << colors::RESET << " ";

        // Selection indicator
        if (is_selected) {
            line << colors::GREEN << colors::BOLD << "\u25B8 " << colors::RESET;
        } else {
            line << "  ";
        }

        // Model name (truncated)
        std::string name = model.name;
        if (name.length() > 28) {
            name = name.substr(0, 25) + "...";
        }

        if (is_selected) {
            line << colors::GREEN << colors::BOLD;
        }
        line << std::left << std::setw(28) << name;
        if (is_selected) {
            line << colors::RESET;
        }

        // Size
        line << " " << colors::DIM << std::left << std::setw(10) << model.size << colors::RESET;

        // Recommended tag
        if (model.is_code_model) {
            line << colors::YELLOW << "(code)" << colors::RESET;
        } else {
            line << "      ";  // Padding
        }

        // Right border
        int content_len = 2 + 2 + 28 + 1 + 10 + 6;  // Approximate content length
        int padding = BOX_WIDTH - 2 - content_len;
        if (padding > 0) {
            line << std::string(padding, ' ');
        }
        line << colors::CYAN << "\u2502" << colors::RESET;

        term.write(line.str());
    }

    // Empty line after models
    term.move_cursor(start_col, start_row + visible_count);
    std::ostringstream empty;
    empty << colors::CYAN << "\u2502" << colors::RESET;
    empty << std::string(BOX_WIDTH - 2, ' ');
    empty << colors::CYAN << "\u2502" << colors::RESET;
    term.write(empty.str());
}

void ModelPicker::render_footer(Terminal& term, int row) {
    auto [cols, _] = Terminal::get_size();
    int start_col = (cols - BOX_WIDTH) / 2;
    if (start_col < 0) start_col = 0;

    term.move_cursor(start_col, row);

    // Instructions
    std::ostringstream help;
    help << colors::CYAN << "\u2502" << colors::RESET << " ";
    help << colors::DIM << "\u2191/\u2193 Navigate  ";
    help << colors::RESET << colors::GREEN << "Enter" << colors::RESET << colors::DIM << ": Select  ";
    help << colors::RESET << colors::RED << "Esc" << colors::RESET << colors::DIM << ": Cancel";
    help << colors::RESET;

    // Padding to fill the line
    int help_content_len = 48;  // Approximate
    int padding = BOX_WIDTH - 2 - help_content_len;
    if (padding > 0) {
        help << std::string(padding, ' ');
    }
    help << colors::CYAN << "\u2502" << colors::RESET;
    term.write(help.str());

    // Bottom border
    term.move_cursor(start_col, row + 1);
    std::ostringstream bottom;
    bottom << colors::CYAN << "\u2514";
    for (int i = 0; i < BOX_WIDTH - 2; ++i) bottom << "\u2500";
    bottom << "\u2518" << colors::RESET;
    term.write(bottom.str());
}

std::string ModelPicker::truncate_or_pad(const std::string& s, size_t width) const {
    if (s.length() > width) {
        return s.substr(0, width - 3) + "...";
    }
    return s + std::string(width - s.length(), ' ');
}

void ModelPicker::move_up() {
    if (selected_index_ > 0) {
        --selected_index_;
    }
}

void ModelPicker::move_down() {
    if (selected_index_ < static_cast<int>(models_.size()) - 1) {
        ++selected_index_;
    }
}

}  // namespace dam::cli
