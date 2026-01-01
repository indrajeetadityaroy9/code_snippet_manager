#include "get_command.hpp"

namespace dam::cli {

void GetCommand::setup(CLI::App& app) {
    app.add_option("id_or_name", id_or_name_, "Snippet ID or name")
        ->required()
        ->type_name("<id|name>");

    app.add_flag("--raw", raw_, "Output content only, no headers");
}

int GetCommand::execute(CommandContext& ctx) {
    auto snippet_result = resolve_snippet(ctx.store, id_or_name_);

    if (!snippet_result.ok()) {
        std::cerr << "Error: Snippet not found: " << id_or_name_ << "\n";
        return DAM_EXIT_NOT_FOUND;
    }

    auto& snippet = snippet_result.value();

    if (raw_) {
        std::cout << snippet.content;
    } else {
        std::cout << "# " << snippet.name << " [" << snippet.language << "]\n";

        if (!snippet.description.empty()) {
            std::cout << "# " << snippet.description << "\n";
        }

        if (!snippet.tags.empty()) {
            std::cout << "# Tags: ";
            for (size_t i = 0; i < snippet.tags.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << snippet.tags[i];
            }
            std::cout << "\n";
        }

        std::cout << "\n" << snippet.content;

        // Ensure trailing newline
        if (!snippet.content.empty() && snippet.content.back() != '\n') {
            std::cout << "\n";
        }
    }

    return DAM_EXIT_SUCCESS;
}

}  // namespace dam::cli
