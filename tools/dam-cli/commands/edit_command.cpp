#include "edit_command.hpp"
#include "add_command.hpp"  // For open_editor, get_extension_for_lang

namespace dam::cli {

void EditCommand::setup(CLI::App& app) {
    app.add_option("id_or_name", id_or_name_, "Snippet ID or name")
        ->required()
        ->type_name("<id|name>");
}

int EditCommand::execute(CommandContext& ctx) {
    auto snippet_result = resolve_snippet(ctx.store, id_or_name_);

    if (!snippet_result.ok()) {
        std::cerr << "Error: Snippet not found: " << id_or_name_ << "\n";
        return DAM_EXIT_NOT_FOUND;
    }

    auto snippet = snippet_result.value();
    SnippetId id = snippet.id;

    // Open editor with current content
    std::string extension = get_extension_for_lang(snippet.language);
    auto [success, new_content] = open_editor(snippet.content, extension);

    if (!success) {
        return DAM_EXIT_SUCCESS;  // User cancelled
    }

    // Check if content actually changed
    if (new_content == snippet.content) {
        std::cout << "No changes made.\n";
        return DAM_EXIT_SUCCESS;
    }

    // Use atomic update to preserve snippet ID and ensure data safety
    auto result = ctx.store->update(id, new_content, snippet.name,
                                     snippet.tags, snippet.language,
                                     snippet.description);
    if (!result.ok()) {
        std::cerr << "Error: " << result.error().to_string() << "\n";
        return DAM_EXIT_IO_ERROR;
    }

    std::cout << "Updated snippet " << id << ": " << snippet.name << "\n";
    return DAM_EXIT_SUCCESS;
}

}  // namespace dam::cli
