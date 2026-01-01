#pragma once

#include "command.hpp"
#include "exit_codes.hpp"
#include <vector>

namespace dam::cli {

/**
 * Manage tags on snippets.
 * Without args: list all tags with counts.
 * With args: add/remove tags from a snippet.
 */
class TagCommand : public Command {
public:
    void setup(CLI::App& app) override;
    int execute(CommandContext& ctx) override;

    std::string name() const override { return "tag"; }
    std::string description() const override {
        return "Manage snippet tags";
    }

private:
    std::string id_or_name_;
    std::vector<std::string> operations_;  // +tag or -tag

    int list_all_tags(CommandContext& ctx);
    int modify_tags(CommandContext& ctx);
};

}  // namespace dam::cli
