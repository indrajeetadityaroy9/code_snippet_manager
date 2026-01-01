#pragma once

#include "command.hpp"
#include "exit_codes.hpp"

namespace dam::cli {

/**
 * Edit an existing snippet's content.
 * Opens $EDITOR with current content.
 */
class EditCommand : public Command {
public:
    void setup(CLI::App& app) override;
    int execute(CommandContext& ctx) override;

    std::string name() const override { return "edit"; }
    std::string description() const override {
        return "Edit a snippet's content";
    }

private:
    std::string id_or_name_;
};

}  // namespace dam::cli
