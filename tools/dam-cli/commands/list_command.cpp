#include "list_command.hpp"
#include <iomanip>

namespace dam::cli {

void ListCommand::setup(CLI::App& /* app */) {
    // No options for list command
}

int ListCommand::execute(CommandContext& ctx) {
    auto snippets_result = ctx.store->list_all();
    if (!snippets_result.ok()) {
        std::cerr << "Error: " << snippets_result.error().to_string() << "\n";
        return DAM_EXIT_IO_ERROR;
    }

    auto& snippets = snippets_result.value();
    if (snippets.empty()) {
        std::cout << "No snippets found.\n";
        std::cout << "Use 'dam add' to create your first snippet.\n";
        return DAM_EXIT_SUCCESS;
    }

    // Print header
    std::cout << std::left
              << std::setw(6) << "ID"
              << std::setw(25) << "NAME"
              << std::setw(12) << "LANG"
              << std::setw(30) << "TAGS"
              << "SIZE\n";
    std::cout << std::string(80, '-') << "\n";

    // Print snippets
    for (const auto& s : snippets) {
        std::string tags_str;
        for (size_t i = 0; i < s.tags.size(); ++i) {
            if (i > 0) tags_str += ", ";
            tags_str += s.tags[i];
        }

        std::cout << std::left
                  << std::setw(6) << s.id
                  << std::setw(25) << truncate(s.name, 24)
                  << std::setw(12) << truncate(s.language, 11)
                  << std::setw(30) << truncate(tags_str, 29)
                  << s.content.size() << " bytes\n";
    }

    std::cout << "\n" << snippets.size() << " snippet(s)\n";
    return DAM_EXIT_SUCCESS;
}

}  // namespace dam::cli
