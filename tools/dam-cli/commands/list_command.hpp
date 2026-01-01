#pragma once

#include "command.hpp"
#include "exit_codes.hpp"

namespace dam::cli {

/**
 * List all snippets in the store.
 */
class ListCommand : public Command {
public:
    void setup(CLI::App& app) override;
    int execute(CommandContext& ctx) override;

    std::string name() const override { return "list"; }
    std::string description() const override {
        return "List all snippets";
    }
};

}  // namespace dam::cli
