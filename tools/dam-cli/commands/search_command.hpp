#pragma once

#include "command.hpp"
#include "exit_codes.hpp"

namespace dam::cli {

/**
 * Search snippets by content, tag, or language.
 */
class SearchCommand : public Command {
public:
    void setup(CLI::App& app) override;
    int execute(CommandContext& ctx) override;

    std::string name() const override { return "search"; }
    std::string description() const override {
        return "Search snippets";
    }

private:
    std::string query_;
    std::string filter_tag_;
    std::string filter_lang_;
    size_t max_results_ = 20;

    int search_by_content(CommandContext& ctx);
    int filter_by_metadata(CommandContext& ctx);
    void print_results(const std::vector<SnippetMetadata>& snippets);
};

}  // namespace dam::cli
