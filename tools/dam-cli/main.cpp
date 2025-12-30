#include <dam/dam.hpp>
#include <dam/util/crc32.hpp>

#ifdef DAM_ENABLE_LLM
#include "interactive/interactive_editor.hpp"
#include "interactive/model_picker.hpp"
#include <dam/llm/error_messages.hpp>
#include <dam/llm/router.hpp>
#include <dam/search/search_router.hpp>
#endif

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

namespace {

// Default store location
std::filesystem::path get_default_store_path() {
    const char* home = std::getenv("HOME");
    if (home) {
        return std::filesystem::path(home) / ".dam";
    }
    return ".dam";
}

// Open the store
std::unique_ptr<dam::SnippetStore> open_store(bool verbose = false) {
    dam::Config config;
    config.root_directory = get_default_store_path();
    config.verbose = verbose;

    auto result = dam::SnippetStore::open(config);
    if (!result.ok()) {
        std::cerr << "Error: " << result.error().to_string() << "\n";
        return nullptr;
    }
    return std::move(result.value());
}

// Read entire file content
std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return "";
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

// Read from stdin
std::string read_stdin() {
    std::ostringstream ss;
    ss << std::cin.rdbuf();
    return ss.str();
}

// Get editor from environment
std::string get_editor() {
    const char* editor = std::getenv("EDITOR");
    if (editor && editor[0] != '\0') return editor;

    editor = std::getenv("VISUAL");
    if (editor && editor[0] != '\0') return editor;

    // Fallback to common editors
    if (std::system("command -v vim >/dev/null 2>&1") == 0) return "vim";
    if (std::system("command -v vi >/dev/null 2>&1") == 0) return "vi";
    if (std::system("command -v nano >/dev/null 2>&1") == 0) return "nano";

    return "";
}

// Safely run editor using fork/exec (avoids shell injection)
int run_editor(const std::string& editor, const std::string& filepath) {
    pid_t pid = fork();
    if (pid == -1) {
        return -1;  // fork failed
    }

    if (pid == 0) {
        // Child process - exec the editor
        // Parse editor command (handle "vim" vs "/usr/bin/vim" vs "code --wait")
        std::vector<std::string> args;
        std::istringstream iss(editor);
        std::string token;
        while (iss >> token) {
            args.push_back(token);
        }
        args.push_back(filepath);

        // Convert to char* array for execvp
        // Store pointers to c_str() - safe because args vector outlives execvp call
        std::vector<const char*> argv_ptrs;
        argv_ptrs.reserve(args.size() + 1);
        for (const auto& arg : args) {
            argv_ptrs.push_back(arg.c_str());
        }
        argv_ptrs.push_back(nullptr);

        // execvp expects char* const[], cast away const (execvp doesn't modify args)
        execvp(argv_ptrs[0], const_cast<char* const*>(argv_ptrs.data()));
        // If execvp returns, it failed
        _exit(127);
    }

    // Parent process - wait for child
    int status;
    if (waitpid(pid, &status, 0) == -1) {
        return -1;
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}

// Open editor and return content
std::pair<bool, std::string> open_editor(const std::string& initial_content = "",
                                          const std::string& extension = ".txt") {
    std::string editor = get_editor();
    if (editor.empty()) {
        std::cerr << "Error: No editor found. Set $EDITOR environment variable.\n";
        return {false, ""};
    }

    // Create temp file
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::filesystem::path temp_file = temp_dir / ("dam_edit_XXXXXX" + extension);

    // Generate unique filename
    std::string temp_path = temp_file.string();
    char* temp_cstr = &temp_path[0];
    int fd = mkstemps(temp_cstr, static_cast<int>(extension.length()));
    if (fd == -1) {
        std::cerr << "Error: Could not create temporary file\n";
        return {false, ""};
    }
    close(fd);
    temp_file = temp_path;

    // Write initial content
    {
        std::ofstream out(temp_file);
        if (!out) {
            std::cerr << "Error: Could not write to temporary file\n";
            std::filesystem::remove(temp_file);
            return {false, ""};
        }
        out << initial_content;
    }

    // Compute hash of initial content for change detection (more reliable than mtime)
    uint32_t before_hash = dam::CRC32::compute(initial_content);

    // Open editor using safe fork/exec
    int result = run_editor(editor, temp_file.string());

    if (result != 0) {
        std::cerr << "Error: Editor exited with error\n";
        std::filesystem::remove(temp_file);
        return {false, ""};
    }

    // Read content
    std::string content = read_file(temp_file);

    // Clean up
    std::filesystem::remove(temp_file);

    // Check if content was modified using hash comparison
    uint32_t after_hash = dam::CRC32::compute(content);
    if (before_hash == after_hash && !initial_content.empty()) {
        std::cout << "No changes made.\n";
        return {false, ""};
    }

    // Check for empty content
    if (content.empty() || content.find_first_not_of(" \t\n\r") == std::string::npos) {
        std::cout << "Empty content, cancelled.\n";
        return {false, ""};
    }

    return {true, content};
}

// Get file extension for language
std::string get_extension_for_lang(const std::string& lang) {
    if (lang == "python") return ".py";
    if (lang == "bash" || lang == "shell") return ".sh";
    if (lang == "javascript") return ".js";
    if (lang == "typescript") return ".ts";
    if (lang == "cpp" || lang == "c++") return ".cpp";
    if (lang == "c") return ".c";
    if (lang == "go") return ".go";
    if (lang == "rust") return ".rs";
    if (lang == "ruby") return ".rb";
    if (lang == "java") return ".java";
    if (lang == "sql") return ".sql";
    if (lang == "yaml") return ".yaml";
    if (lang == "json") return ".json";
    if (lang == "markdown") return ".md";
    return ".txt";
}

// Parse command line for option value
const char* get_option(int argc, char* argv[], const char* short_opt, const char* long_opt) {
    for (int i = 0; i < argc - 1; ++i) {
        if ((short_opt && std::strcmp(argv[i], short_opt) == 0) ||
            (long_opt && std::strcmp(argv[i], long_opt) == 0)) {
            return argv[i + 1];
        }
    }
    return nullptr;
}

// Check if flag is present
bool has_flag(int argc, char* argv[], const char* short_opt, const char* long_opt) {
    for (int i = 0; i < argc; ++i) {
        if ((short_opt && std::strcmp(argv[i], short_opt) == 0) ||
            (long_opt && std::strcmp(argv[i], long_opt) == 0)) {
            return true;
        }
    }
    return false;
}

// Collect all values for repeatable option
std::vector<std::string> get_all_options(int argc, char* argv[], const char* short_opt, const char* long_opt) {
    std::vector<std::string> result;
    for (int i = 0; i < argc - 1; ++i) {
        if ((short_opt && std::strcmp(argv[i], short_opt) == 0) ||
            (long_opt && std::strcmp(argv[i], long_opt) == 0)) {
            result.push_back(argv[i + 1]);
        }
    }
    return result;
}

// Find first non-option argument
const char* get_positional(int argc, char* argv[]) {
    for (int i = 0; i < argc; ++i) {
        if (argv[i][0] != '-') {
            return argv[i];
        }
        // Skip option value
        if (std::strcmp(argv[i], "-n") == 0 || std::strcmp(argv[i], "--name") == 0 ||
            std::strcmp(argv[i], "-t") == 0 || std::strcmp(argv[i], "--tag") == 0 ||
            std::strcmp(argv[i], "-l") == 0 || std::strcmp(argv[i], "--lang") == 0 ||
            std::strcmp(argv[i], "-d") == 0 || std::strcmp(argv[i], "--desc") == 0) {
            ++i;
        }
    }
    return nullptr;
}

// Truncate string for display (Bug #36 fix: prevent underflow)
std::string truncate(const std::string& s, size_t max_len) {
    if (max_len <= 3) return s.substr(0, max_len);  // Prevent underflow
    if (s.length() <= max_len) return s;
    return s.substr(0, max_len - 3) + "...";
}

// Safely parse a snippet ID from string
std::optional<dam::SnippetId> parse_snippet_id(const char* str) {
    if (!str || !*str) return std::nullopt;

    char* end;
    errno = 0;
    unsigned long long val = std::strtoull(str, &end, 10);

    // Check for parse errors
    if (end == str || *end != '\0') {
        return std::nullopt;  // Not a valid number
    }

    // Check for overflow or zero (IDs start at 1)
    if (errno == ERANGE || val == 0) {
        return std::nullopt;
    }

    return static_cast<dam::SnippetId>(val);
}

// ============================================================================
// Commands
// ============================================================================

int cmd_add(int argc, char* argv[]) {
    bool from_stdin = has_flag(argc, argv, nullptr, "--stdin");
    bool interactive = has_flag(argc, argv, "-i", "--interactive");
    const char* name = get_option(argc, argv, "-n", "--name");
    const char* lang = get_option(argc, argv, "-l", "--lang");
    const char* desc = get_option(argc, argv, "-d", "--desc");
    auto tags = get_all_options(argc, argv, "-t", "--tag");
    const char* file = get_positional(argc, argv);

    std::string content;
    std::string snippet_name;
    std::string detected_lang;

    if (from_stdin) {
        content = read_stdin();
        if (!name) {
            std::cerr << "Error: --name is required when reading from stdin\n";
            return 1;
        }
        snippet_name = name;
    } else if (file) {
        std::filesystem::path path(file);
        if (!std::filesystem::exists(path)) {
            std::cerr << "Error: File not found: " << file << "\n";
            return 1;
        }
        content = read_file(path);
        snippet_name = name ? name : path.filename().string();
    } else if (interactive) {
#ifdef DAM_ENABLE_LLM
        // Try interactive mode with LLM assistance
        if (!dam::cli::Terminal::is_tty()) {
            std::cerr << "Error: Interactive mode requires a terminal.\n";
            return 1;
        }

        if (!name) {
            std::cerr << "Error: --name is required\n";
            std::cerr << "Usage: dam add -i -n <name> [-t tag]... [-l lang]\n";
            return 1;
        }
        snippet_name = name;

        // Try to create LLM router with discovery
        auto discovery_result = dam::llm::LLMFactory::create_with_discovery();

        std::unique_ptr<dam::llm::LLMRouter> router;

        if (!discovery_result.ok()) {
            auto error_code = discovery_result.error_code();

            if (error_code == dam::ErrorCode::OLLAMA_NOT_RUNNING) {
                std::cerr << dam::llm::ErrorMessages::ollama_not_running() << "\n";
                return 1;
            }

            if (error_code == dam::ErrorCode::MODEL_NOT_FOUND) {
                std::cerr << dam::llm::ErrorMessages::no_models_installed() << "\n";
                return 1;
            }

            // Generic error
            std::cerr << "Error: " << discovery_result.error().message() << "\n";
            std::cerr << dam::llm::ErrorMessages::setup_help() << "\n";
            return 1;
        }

        auto& create_result = discovery_result.value();

        // If multiple models and no router yet, show model picker
        if (create_result.requires_model_selection && !create_result.router) {
            dam::cli::ModelPicker picker(create_result.discovery.available_models);
            auto selected = picker.pick();

            if (!selected) {
                std::cout << "Cancelled.\n";
                return 0;
            }

            // Create router with selected model
            auto router_result = dam::llm::LLMFactory::create_with_ollama_model(*selected);
            if (!router_result.ok()) {
                std::cerr << "Error: " << router_result.error().message() << "\n";
                return 1;
            }
            router = std::move(router_result.value());
            std::cout << "Using model: " << *selected << "\n";
        } else if (create_result.router) {
            // Auto-selected model (single or recommended)
            router = std::move(create_result.router);
        } else {
            std::cerr << "Error: No LLM model available.\n";
            return 1;
        }

        dam::cli::InteractiveEditorConfig config;
        if (lang) {
            config.language_hint = lang;
        }

        dam::cli::InteractiveEditor editor(std::move(router), config);
        auto result = editor.run();

        if (!result.accepted) {
            std::cout << "Cancelled.\n";
            return 0;
        }

        content = result.content;
        detected_lang = result.detected_language;
#else
        std::cerr << "Error: Interactive mode not available (compiled without LLM support).\n";
        std::cerr << "Rebuild with -DDAM_ENABLE_LLM=ON to enable.\n";
        return 1;
#endif
    } else {
        // Default: open editor to create snippet
        if (!name) {
            std::cerr << "Error: --name is required\n";
            std::cerr << "Usage: dam add -n <name> [-t tag]... [-l lang]\n";
            return 1;
        }
        snippet_name = name;

        std::string extension = lang ? get_extension_for_lang(lang) : ".txt";
        auto [success, editor_content] = open_editor("", extension);
        if (!success) {
            return 1;
        }
        content = editor_content;
    }

    auto store = open_store();
    if (!store) return 1;

    // Use detected language from interactive mode if not explicitly set
    std::string final_lang = lang ? lang : detected_lang;

    auto result = store->add(
        content,
        snippet_name,
        tags,
        final_lang,
        desc ? desc : ""
    );

    if (!result.ok()) {
        std::cerr << "Error: " << result.error().to_string() << "\n";
        return 1;
    }

    std::cout << "Added snippet " << result.value() << ": " << snippet_name << "\n";
    return 0;
}

int cmd_get(int argc, char* argv[]) {
    if (argc < 1) {
        std::cerr << "Usage: dam get <id|name> [--raw]\n";
        return 1;
    }

    bool raw = has_flag(argc, argv, nullptr, "--raw");
    const char* id_or_name = argv[0];

    auto store = open_store();
    if (!store) return 1;

    dam::Result<dam::SnippetMetadata> snippet_result = dam::Error(dam::ErrorCode::NOT_FOUND);

    // Try as ID first
    auto parsed_id = parse_snippet_id(id_or_name);
    if (parsed_id.has_value()) {
        snippet_result = store->get(*parsed_id);
    }

    // Try as name
    if (!snippet_result.ok()) {
        auto found_id = store->find_by_name(id_or_name);
        if (found_id.ok()) {
            snippet_result = store->get(found_id.value());
        }
    }

    if (!snippet_result.ok()) {
        std::cerr << "Error: Snippet not found: " << id_or_name << "\n";
        return 1;
    }

    auto& snippet = snippet_result.value();
    if (raw) {
        std::cout << snippet.content;
    } else {
        std::cout << "# " << snippet.name << " [" << snippet.language << "]\n";
        if (!snippet.description.empty()) {
            std::cout << "# " << snippet.description << "\n";
        }
        if (!snippet.tags.empty()) {
            std::cout << "# Tags: ";
            for (size_t i = 0; i < snippet.tags.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << snippet.tags[i];
            }
            std::cout << "\n";
        }
        std::cout << "\n" << snippet.content;
        if (!snippet.content.empty() && snippet.content.back() != '\n') {
            std::cout << "\n";
        }
    }

    return 0;
}

int cmd_edit(int argc, char* argv[]) {
    if (argc < 1) {
        std::cerr << "Usage: dam edit <id|name>\n";
        return 1;
    }

    const char* id_or_name = argv[0];

    auto store = open_store();
    if (!store) return 1;

    dam::Result<dam::SnippetMetadata> snippet_result = dam::Error(dam::ErrorCode::NOT_FOUND);
    dam::SnippetId id = dam::INVALID_SNIPPET_ID;

    // Try as ID first
    auto parsed_id = parse_snippet_id(id_or_name);
    if (parsed_id.has_value()) {
        id = *parsed_id;
        snippet_result = store->get(id);
    }

    // Try as name
    if (!snippet_result.ok()) {
        auto found_id = store->find_by_name(id_or_name);
        if (found_id.ok()) {
            id = found_id.value();
            snippet_result = store->get(id);
        }
    }

    if (!snippet_result.ok()) {
        std::cerr << "Error: Snippet not found: " << id_or_name << "\n";
        return 1;
    }

    auto& snippet = snippet_result.value();

    // Open editor with current content
    std::string extension = get_extension_for_lang(snippet.language);
    auto [success, new_content] = open_editor(snippet.content, extension);

    if (!success) {
        return 0;  // User cancelled
    }

    // Check if content actually changed
    if (new_content == snippet.content) {
        std::cout << "No changes made.\n";
        return 0;
    }

    // Update snippet - need to remove and re-add since we store content inline
    auto tags = snippet.tags;
    auto name = snippet.name;
    auto lang = snippet.language;
    auto desc = snippet.description;

    auto remove_result = store->remove(id);
    if (!remove_result.ok()) {
        std::cerr << "Error: " << remove_result.error().to_string() << "\n";
        return 1;
    }

    auto add_result = store->add(new_content, name, tags, lang, desc);
    if (!add_result.ok()) {
        std::cerr << "Error: " << add_result.error().to_string() << "\n";
        return 1;
    }

    std::cout << "Updated snippet " << add_result.value() << ": " << name << "\n";
    return 0;
}

int cmd_list(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    auto store = open_store();
    if (!store) return 1;

    auto snippets_result = store->list_all();
    if (!snippets_result.ok()) {
        std::cerr << "Error: " << snippets_result.error().to_string() << "\n";
        return 1;
    }

    auto& snippets = snippets_result.value();
    if (snippets.empty()) {
        std::cout << "No snippets found.\n";
        std::cout << "Use 'dam add' to create your first snippet.\n";
        return 0;
    }

    // Print header
    std::cout << std::left
              << std::setw(6) << "ID"
              << std::setw(25) << "NAME"
              << std::setw(12) << "LANG"
              << std::setw(30) << "TAGS"
              << "SIZE\n";
    std::cout << std::string(80, '-') << "\n";

    // Print snippets
    for (const auto& s : snippets) {
        std::string tags_str;
        for (size_t i = 0; i < s.tags.size(); ++i) {
            if (i > 0) tags_str += ", ";
            tags_str += s.tags[i];
        }

        std::cout << std::left
                  << std::setw(6) << s.id
                  << std::setw(25) << truncate(s.name, 24)
                  << std::setw(12) << truncate(s.language, 11)
                  << std::setw(30) << truncate(tags_str, 29)
                  << s.content.size() << " bytes\n";
    }

    std::cout << "\n" << snippets.size() << " snippet(s)\n";
    return 0;
}

int cmd_rm(int argc, char* argv[]) {
    if (argc < 1) {
        std::cerr << "Usage: dam rm <id|name> [-f]\n";
        return 1;
    }

    bool force = has_flag(argc, argv, "-f", "--force");
    const char* id_or_name = argv[0];

    auto store = open_store();
    if (!store) return 1;

    dam::SnippetId id = dam::INVALID_SNIPPET_ID;

    // Try as ID first
    auto parsed_id = parse_snippet_id(id_or_name);
    if (parsed_id.has_value()) {
        id = *parsed_id;
    } else {
        auto found_id = store->find_by_name(id_or_name);
        if (found_id.ok()) {
            id = found_id.value();
        }
    }

    if (id == dam::INVALID_SNIPPET_ID) {
        std::cerr << "Error: Snippet not found: " << id_or_name << "\n";
        return 1;
    }

    auto snippet_result = store->get(id);
    if (!snippet_result.ok()) {
        std::cerr << "Error: Snippet not found\n";
        return 1;
    }

    auto& snippet = snippet_result.value();
    if (!force) {
        std::cout << "Remove snippet " << id << " (" << snippet.name << ")? [y/N] ";
        std::string response;
        std::getline(std::cin, response);
        if (response != "y" && response != "Y") {
            std::cout << "Cancelled.\n";
            return 0;
        }
    }

    auto result = store->remove(id);
    if (!result.ok()) {
        std::cerr << "Error: " << result.error().to_string() << "\n";
        return 1;
    }

    std::cout << "Removed snippet " << id << "\n";
    return 0;
}

int cmd_tag(int argc, char* argv[]) {
    auto store = open_store();
    if (!store) return 1;

    // No arguments: list all tags with counts
    if (argc < 1) {
        auto counts_result = store->get_tag_counts();
        if (!counts_result.ok()) {
            std::cerr << "Error: " << counts_result.error().to_string() << "\n";
            return 1;
        }

        auto& counts = counts_result.value();
        if (counts.empty()) {
            std::cout << "No tags found.\n";
            return 0;
        }

        // Sort by count (descending)
        std::vector<std::pair<std::string, size_t>> sorted(counts.begin(), counts.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });

        std::cout << "Tags:\n";
        for (const auto& [tag, count] : sorted) {
            std::cout << "  " << tag << " (" << count << ")\n";
        }
        return 0;
    }

    // With arguments: modify tags on a snippet
    if (argc < 2) {
        std::cerr << "Usage: dam tag              # list all tags\n";
        std::cerr << "       dam tag <id> +tag... # add/remove tags\n";
        std::cerr << "  +tag  Add tag\n";
        std::cerr << "  -tag  Remove tag\n";
        return 1;
    }

    const char* id_str = argv[0];

    // Parse ID or name
    dam::SnippetId id = dam::INVALID_SNIPPET_ID;
    auto parsed_id = parse_snippet_id(id_str);
    if (parsed_id.has_value()) {
        id = *parsed_id;
    } else {
        // Try as name
        auto found_id = store->find_by_name(id_str);
        if (found_id.ok()) {
            id = found_id.value();
        }
    }

    if (id == dam::INVALID_SNIPPET_ID || !store->get(id).ok()) {
        std::cerr << "Error: Snippet not found: " << id_str << "\n";
        return 1;
    }

    // Process tag modifications
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (arg[0] == '+') {
            auto result = store->add_tag(id, arg + 1);
            if (!result.ok()) {
                std::cerr << "Error adding tag: " << result.error().to_string() << "\n";
            } else {
                std::cout << "Added tag: " << (arg + 1) << "\n";
            }
        } else if (arg[0] == '-') {
            auto result = store->remove_tag(id, arg + 1);
            if (!result.ok()) {
                std::cerr << "Error removing tag: " << result.error().to_string() << "\n";
            } else {
                std::cout << "Removed tag: " << (arg + 1) << "\n";
            }
        } else {
            std::cerr << "Warning: Ignoring '" << arg << "' (use +tag or -tag)\n";
        }
    }

    return 0;
}

int cmd_search(int argc, char* argv[]) {
    // Parse options
    const char* max_results_str = get_option(argc, argv, "-n", "--max");
    const char* filter_tag = get_option(argc, argv, "-t", "--tag");
    const char* filter_lang = get_option(argc, argv, "-l", "--lang");
    size_t max_results = max_results_str ? std::stoul(max_results_str) : 20;

    // Collect query from all non-option arguments
    std::string query;
    for (int i = 0; i < argc; ++i) {
        const char* arg = argv[i];
        // Skip flags and options
        if (arg[0] == '-') {
            // Skip option values (like -n 10 or -t bash)
            if ((std::strcmp(arg, "-n") == 0 || std::strcmp(arg, "--max") == 0 ||
                 std::strcmp(arg, "-t") == 0 || std::strcmp(arg, "--tag") == 0 ||
                 std::strcmp(arg, "-l") == 0 || std::strcmp(arg, "--lang") == 0) &&
                i + 1 < argc) {
                ++i;  // Skip the value
            }
            continue;
        }
        if (!query.empty()) query += " ";
        query += arg;
    }

    // If no query but filters provided, list all and filter
    bool query_empty = query.empty();
    if (query_empty && !filter_tag && !filter_lang) {
        std::cerr << "Usage: dam search <query> [-t tag] [-l lang] [-n max]\n";
        std::cerr << "       dam search -t <tag>     # filter by tag\n";
        std::cerr << "       dam search -l <lang>    # filter by language\n";
        return 1;
    }

    auto store = open_store();
    if (!store) return 1;

    // Use store's built-in search for content queries
    if (!query_empty) {
        auto search_result = store->search(query, max_results);
        if (search_result.ok()) {
            auto results = search_result.value();

            // Apply metadata filters
            if (filter_tag || filter_lang) {
                results.erase(
                    std::remove_if(results.begin(), results.end(),
                        [&store, filter_tag, filter_lang](const dam::SearchResult& r) {
                            auto snippet_result = store->get(r.id);
                            if (!snippet_result.ok()) return true;

                            auto& snippet = snippet_result.value();
                            if (filter_tag) {
                                bool has_tag = std::find(snippet.tags.begin(), snippet.tags.end(),
                                                         filter_tag) != snippet.tags.end();
                                if (!has_tag) return true;
                            }
                            if (filter_lang) {
                                if (snippet.language != filter_lang) return true;
                            }
                            return false;
                        }),
                    results.end());
            }

            if (results.empty()) {
                std::cout << "No matches found";
                std::cout << " for: " << query;
                if (filter_tag) std::cout << " [tag:" << filter_tag << "]";
                if (filter_lang) std::cout << " [lang:" << filter_lang << "]";
                std::cout << "\n";
                return 0;
            }

            // Print results header
            std::cout << "Search results for: " << query;
            if (filter_tag) std::cout << " [tag:" << filter_tag << "]";
            if (filter_lang) std::cout << " [lang:" << filter_lang << "]";
            std::cout << "\n";
            std::cout << std::string(70, '-') << "\n";
            std::cout << std::left
                      << std::setw(6) << "ID"
                      << std::setw(25) << "NAME"
                      << std::setw(12) << "LANG"
                      << "TAGS\n";
            std::cout << std::string(70, '-') << "\n";

            // Print results
            size_t count = 0;
            for (const auto& r : results) {
                if (count >= max_results) break;
                auto snippet_result = store->get(r.id);
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
            return 0;
        }
    }

    // Filter-only queries (no text search, just list by filter)
    std::vector<dam::SnippetMetadata> snippets;

    if (filter_tag) {
        auto result = store->find_by_tag(filter_tag);
        if (result.ok()) snippets = result.value();
    } else if (filter_lang) {
        auto result = store->find_by_language(filter_lang);
        if (result.ok()) snippets = result.value();
    } else {
        // No query and no filters - list all
        auto result = store->list_all();
        if (result.ok()) snippets = result.value();
    }

    if (snippets.empty()) {
        std::cout << "No snippets found";
        if (filter_tag) std::cout << " [tag:" << filter_tag << "]";
        if (filter_lang) std::cout << " [lang:" << filter_lang << "]";
        std::cout << ".\n";
        return 0;
    }

    // Limit results
    if (snippets.size() > max_results) {
        snippets.resize(max_results);
    }

    std::cout << "Snippets";
    if (filter_tag) std::cout << " [tag:" << filter_tag << "]";
    if (filter_lang) std::cout << " [lang:" << filter_lang << "]";
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
    return 0;
}

int show_help(const char* command) {
    if (command == nullptr || command[0] == '\0') {
        std::cout << R"(DAM - Developer Asset Manager

Usage: dam <command> [options]

Commands:
  add      Add a new snippet
  get      Retrieve a snippet
  edit     Edit an existing snippet
  list     List all snippets
  search   Search and filter snippets
  rm       Remove a snippet
  tag      List tags or modify snippet tags
  help     Show help

Use 'dam help <command>' for more information about a command.

Store location: )" << get_default_store_path() << "\n";
        return 0;
    }

    if (std::strcmp(command, "add") == 0) {
        std::cout << R"(Add a new snippet

Usage:
  dam add -n <name> [-t tag]... [-l lang] [-d desc]
  dam add -i -n <name> [-t tag]... [-l lang] [-d desc]
  dam add <file> [-n name] [-t tag]... [-l lang] [-d desc]
  dam add --stdin -n <name> [-t tag]...

Options:
  -n, --name NAME      Snippet name (required, or defaults to filename)
  -t, --tag TAG        Add tag (repeatable)
  -l, --lang LANG      Language (auto-detected if omitted)
  -d, --desc DESC      Description
  -i, --interactive    LLM-assisted interactive editor
  --stdin              Read content from stdin instead of editor

Interactive Mode (-i):
  Requires Ollama running locally with at least one model installed.

  Setup:
    1. Install Ollama: brew install ollama (macOS)
    2. Start server: ollama serve
    3. Pull a model: ollama pull codellama:7b-instruct

  Features:
  - Type code to get syntax completion suggestions (gray ghost text)
  - Type natural language like "write a binary search" for code generation
  - Press Tab to accept suggestions
  - Press Ctrl+S or Ctrl+D to submit
  - Press Ctrl+C to cancel

  If multiple models are installed, you'll be prompted to select one.
  Override with: DAM_OLLAMA_MODEL=<model> dam add -i ...

By default, opens $EDITOR to write snippet content.

Examples:
  dam add -n "deploy script" -l bash -t devops
  dam add -i -n "sort algo" -l python -t algorithms
  dam add script.sh -t bash -t utils
  dam add config.yaml -n "k8s config" -t k8s
  echo 'ls -la' | dam add --stdin -n "list files" -t bash
)";
    } else if (std::strcmp(command, "get") == 0) {
        std::cout << R"(Retrieve a snippet

Usage: dam get <id|name> [--raw]

Options:
  --raw    Output only the content (no metadata header)

Examples:
  dam get 1
  dam get "my script"
  dam get 1 --raw > script.sh
)";
    } else if (std::strcmp(command, "edit") == 0) {
        std::cout << R"(Edit an existing snippet

Usage: dam edit <id|name>

Opens the snippet content in $EDITOR. On save, the snippet is updated
with the new content. All metadata (name, tags, language) is preserved.

Environment:
  $EDITOR    Preferred editor (falls back to $VISUAL, then vim/vi/nano)

Examples:
  dam edit 1
  dam edit "my script"
)";
    } else if (std::strcmp(command, "list") == 0) {
        std::cout << R"(List all snippets

Usage: dam list

Shows all snippets in a table with ID, name, language, tags, and size.
Use 'dam search' with filters to find specific snippets.

Examples:
  dam list
)";
    } else if (std::strcmp(command, "rm") == 0) {
        std::cout << R"(Remove a snippet

Usage: dam rm <id|name> [-f]

Options:
  -f, --force    Don't prompt for confirmation

Examples:
  dam rm 1
  dam rm "old script" -f
)";
    } else if (std::strcmp(command, "tag") == 0) {
        std::cout << R"(List tags or modify snippet tags

Usage:
  dam tag                       # list all tags with counts
  dam tag <id|name> +tag -tag   # add/remove tags from snippet

Arguments:
  +tag    Add tag to snippet
  -tag    Remove tag from snippet

Examples:
  dam tag                       # list all tags
  dam tag 1 +production -draft  # modify tags on snippet 1
  dam tag "my script" +urgent   # add tag by snippet name
)";
    } else if (std::strcmp(command, "search") == 0) {
        std::cout << R"(Search and filter snippets

Usage:
  dam search <query>             # search by content
  dam search -t <tag>            # filter by tag
  dam search -l <lang>           # filter by language
  dam search <query> -t <tag>    # combined search and filter

Options:
  -t, --tag TAG    Filter results by tag
  -l, --lang LANG  Filter results by language
  -n, --max N      Maximum number of results (default: 20)

Search uses intelligent auto-routing to find the best matches using all
available indexes (keyword, fuzzy, semantic) automatically.

Examples:
  dam search "binary search"         # search content
  dam search -t bash                 # filter by tag
  dam search -l python               # filter by language
  dam search "sort" -t algorithms    # search + filter
  dam search "how to parse json"     # natural language query
)";
    } else {
        std::cerr << "Unknown command: " << command << "\n";
        return 1;
    }

    return 0;
}

int show_usage() {
    std::cerr << "Usage: dam <command> [options]\n";
    std::cerr << "Run 'dam help' for more information.\n";
    return 1;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        return show_usage();
    }

    std::string cmd = argv[1];

    if (cmd == "add")    return cmd_add(argc - 2, argv + 2);
    if (cmd == "get")    return cmd_get(argc - 2, argv + 2);
    if (cmd == "edit")   return cmd_edit(argc - 2, argv + 2);
    if (cmd == "list")   return cmd_list(argc - 2, argv + 2);
    if (cmd == "search") return cmd_search(argc - 2, argv + 2);
    if (cmd == "rm")     return cmd_rm(argc - 2, argv + 2);
    if (cmd == "tag")    return cmd_tag(argc - 2, argv + 2);
    if (cmd == "help")   return show_help(argc > 2 ? argv[2] : nullptr);

    std::cerr << "Unknown command: " << cmd << "\n";
    std::cerr << "Run 'dam help' for available commands.\n";
    return 1;
}
