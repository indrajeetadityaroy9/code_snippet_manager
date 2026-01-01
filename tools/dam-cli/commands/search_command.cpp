#include "search_command.hpp"
#include <algorithm>
#include <iomanip>

namespace dam::cli {

void SearchCommand::setup(CLI::App& app) {
    app.add_option("query", query_, "Search query")
        ->type_name("<query>");

    app.add_option("-t,--tag", filter_tag_, "Filter by tag")
        ->type_name("<tag>");

    app.add_option("-l,--lang", filter_lang_, "Filter by language")
        ->type_name("<lang>");

    app.add_option("-n,--max", max_results_, "Maximum results (default: 20)")
        ->type_name("<num>");
}

int SearchCommand::execute(CommandContext& ctx) {
    // Require at least a query or a filter
    if (query_.empty() && filter_tag_.empty() && filter_lang_.empty()) {
        std::cerr << "Usage: dam search <query> [-t tag] [-l lang] [-n max]\n";
        std::cerr << "       dam search -t <tag>     # filter by tag\n";
        std::cerr << "       dam search -l <lang>    # filter by language\n";
        return DAM_EXIT_USER_ERROR;
    }

    // Content search
    if (!query_.empty()) {
        return search_by_content(ctx);
    }

    // Filter-only (no text query)
    return filter_by_metadata(ctx);
}

int SearchCommand::search_by_content(CommandContext& ctx) {
    auto search_result = ctx.store->search(query_, max_results_);
    if (!search_result.ok()) {
        std::cerr << "Error: " << search_result.error().to_string() << "\n";
        return DAM_EXIT_IO_ERROR;
    }

    auto results = search_result.value();

    // Apply metadata filters
    if (!filter_tag_.empty() || !filter_lang_.empty()) {
        results.erase(
            std::remove_if(results.begin(), results.end(),
                [this, &ctx](const SearchResult& r) {
                    auto snippet_result = ctx.store->get(r.id);
                    if (!snippet_result.ok()) return true;

                    auto& snippet = snippet_result.value();
                    if (!filter_tag_.empty()) {
                        bool has_tag = std::find(snippet.tags.begin(), snippet.tags.end(),
                                                  filter_tag_) != snippet.tags.end();
                        if (!has_tag) return true;
                    }
                    if (!filter_lang_.empty()) {
                        if (snippet.language != filter_lang_) return true;
                    }
                    return false;
                }),
            results.end());
    }

    if (results.empty()) {
        std::cout << "No matches found for: " << query_;
        if (!filter_tag_.empty()) std::cout << " [tag:" << filter_tag_ << "]";
        if (!filter_lang_.empty()) std::cout << " [lang:" << filter_lang_ << "]";
        std::cout << "\n";
        return DAM_EXIT_SUCCESS;
    }

    // Print results header
    std::cout << "Search results for: " << query_;
    if (!filter_tag_.empty()) std::cout << " [tag:" << filter_tag_ << "]";
    if (!filter_lang_.empty()) std::cout << " [lang:" << filter_lang_ << "]";
    std::cout << "\n";
    std::cout << std::string(70, '-') << "\n";
    std::cout << std::left
              << std::setw(6) << "ID"
              << std::setw(25) << "NAME"
              << std::setw(12) << "LANG"
              << "TAGS\n";
    std::cout << std::string(70, '-') << "\n";

    size_t count = 0;
    for (const auto& r : results) {
        if (count >= max_results_) break;
        auto snippet_result = ctx.store->get(r.id);
        if (!snippet_result.ok()) continue;

        auto& snippet = snippet_result.value();
        std::string tags_str;
        for (size_t i = 0; i < snippet.tags.size() && i < 3; ++i) {
            if (i > 0) tags_str += ", ";
            tags_str += snippet.tags[i];
        }

        std::cout << std::left
                  << std::setw(6) << r.id
                  << std::setw(25) << truncate(snippet.name, 24)
                  << std::setw(12) << truncate(snippet.language, 11)
                  << tags_str << "\n";
        ++count;
    }

    std::cout << "\n" << count << " result(s)\n";
    return DAM_EXIT_SUCCESS;
}

int SearchCommand::filter_by_metadata(CommandContext& ctx) {
    std::vector<SnippetMetadata> snippets;

    if (!filter_tag_.empty()) {
        auto result = ctx.store->find_by_tag(filter_tag_);
        if (result.ok()) snippets = result.value();
    } else if (!filter_lang_.empty()) {
        auto result = ctx.store->find_by_language(filter_lang_);
        if (result.ok()) snippets = result.value();
    }

    if (snippets.empty()) {
        std::cout << "No snippets found";
        if (!filter_tag_.empty()) std::cout << " [tag:" << filter_tag_ << "]";
        if (!filter_lang_.empty()) std::cout << " [lang:" << filter_lang_ << "]";
        std::cout << ".\n";
        return DAM_EXIT_SUCCESS;
    }

    // Limit results
    if (snippets.size() > max_results_) {
        snippets.resize(max_results_);
    }

    print_results(snippets);
    return DAM_EXIT_SUCCESS;
}

void SearchCommand::print_results(const std::vector<SnippetMetadata>& snippets) {
    std::cout << "Snippets";
    if (!filter_tag_.empty()) std::cout << " [tag:" << filter_tag_ << "]";
    if (!filter_lang_.empty()) std::cout << " [lang:" << filter_lang_ << "]";
    std::cout << "\n";
    std::cout << std::string(70, '-') << "\n";
    std::cout << std::left
              << std::setw(6) << "ID"
              << std::setw(25) << "NAME"
              << std::setw(12) << "LANG"
              << "TAGS\n";
    std::cout << std::string(70, '-') << "\n";

    for (const auto& s : snippets) {
        std::string tags_str;
        for (size_t i = 0; i < s.tags.size() && i < 3; ++i) {
            if (i > 0) tags_str += ", ";
            tags_str += s.tags[i];
        }

        std::cout << std::left
                  << std::setw(6) << s.id
                  << std::setw(25) << truncate(s.name, 24)
                  << std::setw(12) << truncate(s.language, 11)
                  << tags_str << "\n";
    }

    std::cout << "\n" << snippets.size() << " snippet(s)\n";
}

}  // namespace dam::cli
