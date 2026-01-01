#pragma once

#include "command.hpp"
#include "exit_codes.hpp"

namespace dam::cli {

/**
 * Remove a snippet from the store.
 */
class RmCommand : public Command {
public:
    void setup(CLI::App& app) override;
    int execute(CommandContext& ctx) override;

    std::string name() const override { return "rm"; }
    std::string description() const override {
        return "Remove a snippet";
    }

private:
    std::string id_or_name_;
    bool force_ = false;
};

}  // namespace dam::cli
