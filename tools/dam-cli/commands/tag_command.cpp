#include "tag_command.hpp"
#include <algorithm>

namespace dam::cli {

void TagCommand::setup(CLI::App& app) {
    app.add_option("id_or_name", id_or_name_, "Snippet ID or name")
        ->type_name("<id|name>");

    app.add_option("operations", operations_, "Tag operations (+tag to add, -tag to remove)")
        ->type_name("<+tag|-tag>...");
}

int TagCommand::execute(CommandContext& ctx) {
    // No arguments: list all tags
    if (id_or_name_.empty()) {
        return list_all_tags(ctx);
    }

    // With ID but no operations: show usage
    if (operations_.empty()) {
        std::cerr << "Usage: dam tag              # list all tags\n";
        std::cerr << "       dam tag <id> +tag... # add/remove tags\n";
        std::cerr << "  +tag  Add tag\n";
        std::cerr << "  -tag  Remove tag\n";
        return DAM_EXIT_USER_ERROR;
    }

    return modify_tags(ctx);
}

int TagCommand::list_all_tags(CommandContext& ctx) {
    auto counts_result = ctx.store->get_tag_counts();
    if (!counts_result.ok()) {
        std::cerr << "Error: " << counts_result.error().to_string() << "\n";
        return DAM_EXIT_IO_ERROR;
    }

    auto& counts = counts_result.value();
    if (counts.empty()) {
        std::cout << "No tags found.\n";
        return DAM_EXIT_SUCCESS;
    }

    // Sort by count (descending)
    std::vector<std::pair<std::string, size_t>> sorted(counts.begin(), counts.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    std::cout << "Tags:\n";
    for (const auto& [tag, count] : sorted) {
        std::cout << "  " << tag << " (" << count << ")\n";
    }

    return DAM_EXIT_SUCCESS;
}

int TagCommand::modify_tags(CommandContext& ctx) {
    // Resolve snippet
    auto snippet_result = resolve_snippet(ctx.store, id_or_name_);

    if (!snippet_result.ok()) {
        std::cerr << "Error: Snippet not found: " << id_or_name_ << "\n";
        return DAM_EXIT_NOT_FOUND;
    }

    SnippetId id = snippet_result.value().id;

    // Process tag operations
    for (const auto& op : operations_) {
        if (op.empty()) continue;

        if (op[0] == '+') {
            std::string tag = op.substr(1);
            auto result = ctx.store->add_tag(id, tag);
            if (!result.ok()) {
                std::cerr << "Error adding tag: " << result.error().to_string() << "\n";
            } else {
                std::cout << "Added tag: " << tag << "\n";
            }
        } else if (op[0] == '-') {
            std::string tag = op.substr(1);
            auto result = ctx.store->remove_tag(id, tag);
            if (!result.ok()) {
                std::cerr << "Error removing tag: " << result.error().to_string() << "\n";
            } else {
                std::cout << "Removed tag: " << tag << "\n";
            }
        } else {
            std::cerr << "Warning: Ignoring '" << op << "' (use +tag or -tag)\n";
        }
    }

    return DAM_EXIT_SUCCESS;
}

}  // namespace dam::cli
