#include <dam/dam.hpp>
#include <dam/util/crc32.hpp>

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

// Safely parse a snippet ID from string (Bug #19 fix: check for overflow)
std::optional<dam::SnippetId> parse_snippet_id(const char* str) {
    if (!str || !*str) return std::nullopt;

    char* end;
    errno = 0;
    long long val = std::strtoll(str, &end, 10);

    // Check for parse errors
    if (end == str || *end != '\0') {
        return std::nullopt;  // Not a valid number
    }

    // Check for overflow
    if (errno == ERANGE || val <= 0 || val > static_cast<long long>(UINT64_MAX)) {
        return std::nullopt;
    }

    return static_cast<dam::SnippetId>(val);
}

// Format timestamp
std::string format_time(std::chrono::system_clock::time_point tp) {
    auto time = std::chrono::system_clock::to_time_t(tp);
    std::tm tm = *std::localtime(&time);
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M");
    return ss.str();
}

// ============================================================================
// Commands
// ============================================================================

int cmd_add(int argc, char* argv[]) {
    bool from_stdin = has_flag(argc, argv, nullptr, "--stdin");
    const char* name = get_option(argc, argv, "-n", "--name");
    const char* lang = get_option(argc, argv, "-l", "--lang");
    const char* desc = get_option(argc, argv, "-d", "--desc");
    auto tags = get_all_options(argc, argv, "-t", "--tag");
    const char* file = get_positional(argc, argv);

    std::string content;
    std::string snippet_name;

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

    auto result = store->add(
        content,
        snippet_name,
        tags,
        lang ? lang : "",
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

    std::optional<dam::SnippetMetadata> snippet;

    // Try as ID first
    auto parsed_id = parse_snippet_id(id_or_name);
    if (parsed_id.has_value()) {
        snippet = store->get(*parsed_id);
    }

    // Try as name
    if (!snippet.has_value()) {
        auto found_id = store->find_by_name(id_or_name);
        if (found_id.has_value()) {
            snippet = store->get(*found_id);
        }
    }

    if (!snippet.has_value()) {
        std::cerr << "Error: Snippet not found: " << id_or_name << "\n";
        return 1;
    }

    if (raw) {
        std::cout << snippet->content;
    } else {
        std::cout << "# " << snippet->name << " [" << snippet->language << "]\n";
        if (!snippet->description.empty()) {
            std::cout << "# " << snippet->description << "\n";
        }
        if (!snippet->tags.empty()) {
            std::cout << "# Tags: ";
            for (size_t i = 0; i < snippet->tags.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << snippet->tags[i];
            }
            std::cout << "\n";
        }
        std::cout << "\n" << snippet->content;
        if (!snippet->content.empty() && snippet->content.back() != '\n') {
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

    std::optional<dam::SnippetMetadata> snippet;
    dam::SnippetId id = dam::INVALID_SNIPPET_ID;

    // Try as ID first
    auto parsed_id = parse_snippet_id(id_or_name);
    if (parsed_id.has_value()) {
        id = *parsed_id;
        snippet = store->get(id);
    }

    // Try as name
    if (!snippet.has_value()) {
        auto found_id = store->find_by_name(id_or_name);
        if (found_id.has_value()) {
            id = *found_id;
            snippet = store->get(id);
        }
    }

    if (!snippet.has_value()) {
        std::cerr << "Error: Snippet not found: " << id_or_name << "\n";
        return 1;
    }

    // Open editor with current content
    std::string extension = get_extension_for_lang(snippet->language);
    auto [success, new_content] = open_editor(snippet->content, extension);

    if (!success) {
        return 0;  // User cancelled
    }

    // Check if content actually changed
    if (new_content == snippet->content) {
        std::cout << "No changes made.\n";
        return 0;
    }

    // Update snippet - need to remove and re-add since we store content inline
    auto tags = snippet->tags;
    auto name = snippet->name;
    auto lang = snippet->language;
    auto desc = snippet->description;

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
    const char* tag = get_option(argc, argv, "-t", "--tag");
    const char* lang = get_option(argc, argv, "-l", "--lang");

    auto store = open_store();
    if (!store) return 1;

    std::vector<dam::SnippetMetadata> snippets;

    if (tag) {
        snippets = store->find_by_tag(tag);
    } else if (lang) {
        snippets = store->find_by_language(lang);
    } else {
        snippets = store->list_all();
    }

    if (snippets.empty()) {
        std::cout << "No snippets found.\n";
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
        if (found_id.has_value()) {
            id = *found_id;
        }
    }

    if (id == dam::INVALID_SNIPPET_ID) {
        std::cerr << "Error: Snippet not found: " << id_or_name << "\n";
        return 1;
    }

    auto snippet = store->get(id);
    if (!snippet.has_value()) {
        std::cerr << "Error: Snippet not found\n";
        return 1;
    }

    if (!force) {
        std::cout << "Remove snippet " << id << " (" << snippet->name << ")? [y/N] ";
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

int cmd_tags(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    auto store = open_store();
    if (!store) return 1;

    auto counts = store->get_tag_counts();

    if (counts.empty()) {
        std::cout << "No tags found.\n";
        return 0;
    }

    // Sort by count (descending)
    std::vector<std::pair<std::string, size_t>> sorted(counts.begin(), counts.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    for (const auto& [tag, count] : sorted) {
        std::cout << tag << " (" << count << ")\n";
    }

    return 0;
}

int cmd_tag(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: dam tag <id> <+tag|-tag>...\n";
        std::cerr << "  +tag  Add tag\n";
        std::cerr << "  -tag  Remove tag\n";
        return 1;
    }

    const char* id_str = argv[0];

    auto store = open_store();
    if (!store) return 1;

    // Parse ID
    auto parsed_id = parse_snippet_id(id_str);
    if (!parsed_id.has_value()) {
        std::cerr << "Error: Invalid snippet ID: " << id_str << "\n";
        return 1;
    }
    dam::SnippetId id = *parsed_id;

    if (!store->get(id).has_value()) {
        std::cerr << "Error: Snippet not found: " << id << "\n";
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

int show_help(const char* command) {
    if (command == nullptr || command[0] == '\0') {
        std::cout << R"(DAM - Developer Asset Manager

Usage: dam <command> [options]

Commands:
  add      Add a new snippet
  get      Retrieve a snippet
  edit     Edit an existing snippet
  list     List snippets
  rm       Remove a snippet
  tags     List all tags
  tag      Add/remove tags from a snippet
  help     Show help

Use 'dam help <command>' for more information about a command.

Store location: )" << get_default_store_path() << "\n";
        return 0;
    }

    if (std::strcmp(command, "add") == 0) {
        std::cout << R"(Add a new snippet

Usage:
  dam add -n <name> [-t tag]... [-l lang] [-d desc]
  dam add <file> [-n name] [-t tag]... [-l lang] [-d desc]
  dam add --stdin -n <name> [-t tag]...

Options:
  -n, --name NAME    Snippet name (required, or defaults to filename)
  -t, --tag TAG      Add tag (repeatable)
  -l, --lang LANG    Language (auto-detected if omitted)
  -d, --desc DESC    Description
  --stdin            Read content from stdin instead of editor

By default, opens $EDITOR to write snippet content.

Examples:
  dam add -n "deploy script" -l bash -t devops
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
        std::cout << R"(List snippets

Usage: dam list [-t tag] [-l lang]

Options:
  -t, --tag TAG     Filter by tag
  -l, --lang LANG   Filter by language

Examples:
  dam list
  dam list -t bash
  dam list -l python
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
    } else if (std::strcmp(command, "tags") == 0) {
        std::cout << R"(List all tags with counts

Usage: dam tags

Shows all unique tags and the number of snippets with each tag.
)";
    } else if (std::strcmp(command, "tag") == 0) {
        std::cout << R"(Add or remove tags from a snippet

Usage: dam tag <id> <+tag|-tag>...

Arguments:
  +tag    Add tag
  -tag    Remove tag

Examples:
  dam tag 1 +production -draft
  dam tag 1 +important +urgent
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

    if (cmd == "add")   return cmd_add(argc - 2, argv + 2);
    if (cmd == "get")   return cmd_get(argc - 2, argv + 2);
    if (cmd == "edit")  return cmd_edit(argc - 2, argv + 2);
    if (cmd == "list")  return cmd_list(argc - 2, argv + 2);
    if (cmd == "rm")    return cmd_rm(argc - 2, argv + 2);
    if (cmd == "tags")  return cmd_tags(argc - 2, argv + 2);
    if (cmd == "tag")   return cmd_tag(argc - 2, argv + 2);
    if (cmd == "help")  return show_help(argc > 2 ? argv[2] : nullptr);

    std::cerr << "Unknown command: " << cmd << "\n";
    std::cerr << "Run 'dam help' for available commands.\n";
    return 1;
}
