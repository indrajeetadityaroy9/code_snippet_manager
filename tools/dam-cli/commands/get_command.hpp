#pragma once

#include "command.hpp"
#include "exit_codes.hpp"

namespace dam::cli {

/**
 * Retrieve and display a snippet by ID or name.
 */
class GetCommand : public Command {
public:
    void setup(CLI::App& app) override;
    int execute(CommandContext& ctx) override;

    std::string name() const override { return "get"; }
    std::string description() const override {
        return "Get a snippet by ID or name";
    }

private:
    std::string id_or_name_;
    bool raw_ = false;
};

}  // namespace dam::cli
