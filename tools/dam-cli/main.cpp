#include <CLI/CLI.hpp>
#include <dam/dam.hpp>

#include "commands/command.hpp"
#include "commands/exit_codes.hpp"
#include "commands/add_command.hpp"
#include "commands/get_command.hpp"
#include "commands/edit_command.hpp"
#include "commands/list_command.hpp"
#include "commands/rm_command.hpp"
#include "commands/tag_command.hpp"
#include "commands/search_command.hpp"

#include <iostream>
#include <memory>
#include <vector>

using namespace dam::cli;

int main(int argc, char* argv[]) {
    // Create main CLI app
    CLI::App app{"DAM - Developer Asset Manager"};
    app.require_subcommand(1);

    // Global options
    std::string store_path;
    bool verbose = false;

    app.add_option("--store", store_path, "Store directory (default: ~/.dam)")
        ->type_name("<path>");
    app.add_flag("-v,--verbose", verbose, "Enable verbose output");

    // Create command instances
    std::vector<std::unique_ptr<Command>> commands;
    commands.push_back(std::make_unique<AddCommand>());
    commands.push_back(std::make_unique<GetCommand>());
    commands.push_back(std::make_unique<EditCommand>());
    commands.push_back(std::make_unique<ListCommand>());
    commands.push_back(std::make_unique<RmCommand>());
    commands.push_back(std::make_unique<TagCommand>());
    commands.push_back(std::make_unique<SearchCommand>());

    // Track which command was selected
    Command* selected_command = nullptr;

    // Register each command as a subcommand
    for (auto& cmd : commands) {
        auto* subapp = app.add_subcommand(cmd->name(), cmd->description());
        cmd->setup(*subapp);

        // Capture the command when its subcommand is parsed
        subapp->callback([&selected_command, &cmd]() {
            selected_command = cmd.get();
        });
    }

    // Parse command line
    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }

    // Execute selected command
    if (selected_command) {
        // Determine store path
        std::filesystem::path path = store_path.empty()
            ? get_default_store_path()
            : std::filesystem::path(store_path);

        // Open store
        auto store = open_store(path, verbose);
        if (!store) {
            return DAM_EXIT_IO_ERROR;
        }

        // Create context
        CommandContext ctx;
        ctx.store = store.get();
        ctx.verbose = verbose;
        ctx.store_path = path;

        // Execute
        return selected_command->execute(ctx);
    }

    return DAM_EXIT_SUCCESS;
}
