#include "add_command.hpp"
#include <dam/util/crc32.hpp>

#ifdef DAM_ENABLE_LLM
#include "../interactive/interactive_editor.hpp"
#include "../interactive/model_picker.hpp"
#include <dam/llm/error_messages.hpp>
#include <dam/llm/router.hpp>
#endif

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

namespace dam::cli {

// ============================================================================
// Helper Functions
// ============================================================================

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

int run_editor(const std::string& editor, const std::string& filepath) {
    pid_t pid = fork();
    if (pid == -1) {
        return -1;
    }

    if (pid == 0) {
        // Child process - exec the editor
        std::vector<std::string> args;
        std::istringstream iss(editor);
        std::string token;
        while (iss >> token) {
            args.push_back(token);
        }
        args.push_back(filepath);

        // Convert to char* array for execvp
        std::vector<char*> argv;
        for (auto& arg : args) {
            argv.push_back(&arg[0]);
        }
        argv.push_back(nullptr);

        execvp(argv[0], argv.data());
        _exit(127);
    }

    // Parent process - wait for editor
    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}

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

std::pair<bool, std::string> open_editor(const std::string& initial_content,
                                          const std::string& extension) {
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

    // Compute hash of initial content for change detection
    uint32_t before_hash = CRC32::compute(initial_content);

    // Open editor
    int result = run_editor(editor, temp_file.string());

    if (result != 0) {
        std::cerr << "Error: Editor exited with error\n";
        std::filesystem::remove(temp_file);
        return {false, ""};
    }

    // Read content
    auto content_opt = read_file(temp_file);

    // Clean up
    std::filesystem::remove(temp_file);

    if (!content_opt.has_value()) {
        std::cerr << "Error: Could not read temporary file\n";
        return {false, ""};
    }

    std::string content = std::move(*content_opt);

    // Check if content was modified
    uint32_t after_hash = CRC32::compute(content);
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

// ============================================================================
// AddCommand Implementation
// ============================================================================

void AddCommand::setup(CLI::App& app) {
    app.add_option("-n,--name", name_, "Snippet name (required for stdin/interactive)")
        ->type_name("<name>");

    app.add_option("-t,--tag", tags_, "Add tag (repeatable)")
        ->type_name("<tag>");

    app.add_option("-l,--lang", language_, "Language (auto-detected if omitted)")
        ->type_name("<lang>");

    app.add_option("-d,--desc", desc_, "Description")
        ->type_name("<text>");

    app.add_option("file", file_, "File to add as snippet")
        ->type_name("<file>");

    app.add_flag("--stdin", from_stdin_, "Read content from stdin");

    app.add_flag("-i,--interactive", interactive_, "Use interactive LLM-assisted editor");
}

int AddCommand::execute(CommandContext& ctx) {
    // Determine input mode (priority: stdin > file > interactive > editor)
    if (from_stdin_) {
        return add_from_stdin(ctx);
    } else if (!file_.empty()) {
        return add_from_file(ctx);
    } else if (interactive_) {
        return add_interactive(ctx);
    } else {
        return add_from_editor(ctx);
    }
}

int AddCommand::add_from_file(CommandContext& ctx) {
    std::filesystem::path path(file_);
    if (!std::filesystem::exists(path)) {
        std::cerr << "Error: File not found: " << file_ << "\n";
        return DAM_EXIT_IO_ERROR;
    }

    auto content_opt = read_file(path);
    if (!content_opt.has_value()) {
        std::cerr << "Error: Could not read file: " << file_ << "\n";
        return DAM_EXIT_IO_ERROR;
    }

    std::string content = std::move(*content_opt);

    // Use filename as name if not specified
    if (name_.empty()) {
        name_ = path.filename().string();
    }

    return save_snippet(ctx, content);
}

int AddCommand::add_from_stdin(CommandContext& ctx) {
    if (name_.empty()) {
        std::cerr << "Error: --name is required when reading from stdin\n";
        return DAM_EXIT_USER_ERROR;
    }

    std::string content = read_stdin();
    return save_snippet(ctx, content);
}

int AddCommand::add_from_editor(CommandContext& ctx) {
    if (name_.empty()) {
        std::cerr << "Error: --name is required\n";
        std::cerr << "Usage: dam add -n <name> [-t tag]... [-l lang]\n";
        return DAM_EXIT_USER_ERROR;
    }

    std::string extension = language_.empty() ? ".txt" : get_extension_for_lang(language_);
    auto [success, content] = open_editor("", extension);

    if (!success) {
        return DAM_EXIT_SUCCESS;  // User cancelled
    }

    return save_snippet(ctx, content);
}

int AddCommand::add_interactive(CommandContext& ctx) {
#ifdef DAM_ENABLE_LLM
    if (!Terminal::is_tty()) {
        std::cerr << "Error: Interactive mode requires a terminal.\n";
        return DAM_EXIT_USER_ERROR;
    }

    if (name_.empty()) {
        std::cerr << "Error: --name is required\n";
        std::cerr << "Usage: dam add -i -n <name> [-t tag]... [-l lang]\n";
        return DAM_EXIT_USER_ERROR;
    }

    // Try to create LLM router with discovery
    auto discovery_result = llm::LLMFactory::create_with_discovery();

    std::unique_ptr<llm::LLMRouter> router;

    if (!discovery_result.ok()) {
        auto error_code = discovery_result.error_code();

        if (error_code == ErrorCode::OLLAMA_NOT_RUNNING) {
            std::cerr << llm::ErrorMessages::ollama_not_running() << "\n";
            return DAM_EXIT_IO_ERROR;
        }

        if (error_code == ErrorCode::MODEL_NOT_FOUND) {
            std::cerr << llm::ErrorMessages::no_models_installed() << "\n";
            return DAM_EXIT_IO_ERROR;
        }

        std::cerr << "Error: " << discovery_result.error().message() << "\n";
        std::cerr << llm::ErrorMessages::setup_help() << "\n";
        return DAM_EXIT_IO_ERROR;
    }

    auto& create_result = discovery_result.value();

    // If multiple models and no router yet, show model picker
    if (create_result.requires_model_selection && !create_result.router) {
        ModelPicker picker(create_result.discovery.available_models);
        auto selected = picker.pick();

        if (!selected) {
            std::cout << "Cancelled.\n";
            return DAM_EXIT_SUCCESS;
        }

        auto router_result = llm::LLMFactory::create_with_ollama_model(*selected);
        if (!router_result.ok()) {
            std::cerr << "Error: " << router_result.error().message() << "\n";
            return DAM_EXIT_IO_ERROR;
        }
        router = std::move(router_result.value());
        std::cout << "Using model: " << *selected << "\n";
    } else if (create_result.router) {
        router = std::move(create_result.router);
    } else {
        std::cerr << "Error: No LLM model available.\n";
        return DAM_EXIT_IO_ERROR;
    }

    InteractiveEditorConfig config;
    if (!language_.empty()) {
        config.language_hint = language_;
    }

    InteractiveEditor editor(std::move(router), config);
    auto result = editor.run();

    if (!result.accepted) {
        std::cout << "Cancelled.\n";
        return DAM_EXIT_SUCCESS;
    }

    return save_snippet(ctx, result.content, result.detected_language);
#else
    std::cerr << "Error: Interactive mode not available (compiled without LLM support).\n";
    std::cerr << "Rebuild with -DDAM_ENABLE_LLM=ON to enable.\n";
    return DAM_EXIT_USER_ERROR;
#endif
}

int AddCommand::save_snippet(CommandContext& ctx, const std::string& content,
                              const std::string& detected_lang) {
    // Use detected language if not explicitly set
    std::string final_lang = language_.empty() ? detected_lang : language_;

    auto result = ctx.store->add(content, name_, tags_, final_lang, desc_);

    if (!result.ok()) {
        std::cerr << "Error: " << result.error().to_string() << "\n";
        return DAM_EXIT_IO_ERROR;
    }

    std::cout << "Added snippet " << result.value() << ": " << name_ << "\n";
    return DAM_EXIT_SUCCESS;
}

}  // namespace dam::cli
