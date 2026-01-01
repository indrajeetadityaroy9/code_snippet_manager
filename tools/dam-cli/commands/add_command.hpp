#pragma once

#include "command.hpp"
#include "exit_codes.hpp"
#include <string>
#include <vector>

namespace dam::cli {

/**
 * Add a new code snippet to the store.
 *
 * Input modes (mutually exclusive):
 * - File path: Read content from file
 * - --stdin: Read content from stdin
 * - -i/--interactive: Use LLM-assisted interactive editor
 * - Default: Open $EDITOR to compose content
 */
class AddCommand : public Command {
public:
    void setup(CLI::App& app) override;
    int execute(CommandContext& ctx) override;

    std::string name() const override { return "add"; }
    std::string description() const override {
        return "Add a new code snippet";
    }

private:
    // CLI options
    std::string name_;
    std::string language_;
    std::string desc_;
    std::vector<std::string> tags_;
    std::string file_;
    bool from_stdin_ = false;
    bool interactive_ = false;

    // Helper methods
    int add_from_file(CommandContext& ctx);
    int add_from_stdin(CommandContext& ctx);
    int add_from_editor(CommandContext& ctx);
    int add_interactive(CommandContext& ctx);

    int save_snippet(CommandContext& ctx, const std::string& content,
                     const std::string& detected_lang = "");
};

// Helper: Get file extension for a language
std::string get_extension_for_lang(const std::string& lang);

// Helper: Open external editor and return content
std::pair<bool, std::string> open_editor(const std::string& initial_content = "",
                                          const std::string& extension = ".txt");

// Helper: Get editor command from environment
std::string get_editor();

// Helper: Safely run editor using fork/exec
int run_editor(const std::string& editor, const std::string& filepath);

}  // namespace dam::cli
