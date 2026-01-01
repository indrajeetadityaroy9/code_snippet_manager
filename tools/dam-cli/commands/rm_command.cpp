#include "rm_command.hpp"

namespace dam::cli {

void RmCommand::setup(CLI::App& app) {
    app.add_option("id_or_name", id_or_name_, "Snippet ID or name")
        ->required()
        ->type_name("<id|name>");

    app.add_flag("-f,--force", force_, "Skip confirmation prompt");
}

int RmCommand::execute(CommandContext& ctx) {
    // Resolve snippet
    auto snippet_result = resolve_snippet(ctx.store, id_or_name_);

    if (!snippet_result.ok()) {
        std::cerr << "Error: Snippet not found: " << id_or_name_ << "\n";
        return DAM_EXIT_NOT_FOUND;
    }

    auto& snippet = snippet_result.value();
    SnippetId id = snippet.id;

    // Confirm deletion unless --force
    if (!force_) {
        std::cout << "Remove snippet " << id << " (" << snippet.name << ")? [y/N] ";
        std::string response;
        std::getline(std::cin, response);
        if (response != "y" && response != "Y") {
            std::cout << "Cancelled.\n";
            return DAM_EXIT_SUCCESS;
        }
    }

    auto result = ctx.store->remove(id);
    if (!result.ok()) {
        std::cerr << "Error: " << result.error().to_string() << "\n";
        return DAM_EXIT_IO_ERROR;
    }

    std::cout << "Removed snippet " << id << "\n";
    return DAM_EXIT_SUCCESS;
}

}  // namespace dam::cli
