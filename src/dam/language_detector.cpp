#include <dam/language_detector.hpp>
#include <algorithm>
#include <unordered_map>

namespace dam {

namespace {

// Extension to language mapping
const std::unordered_map<std::string, std::string> EXTENSION_MAP = {
    // Shell
    {".sh", "bash"},
    {".bash", "bash"},
    {".zsh", "zsh"},
    {".fish", "fish"},

    // Python
    {".py", "python"},
    {".pyw", "python"},
    {".pyx", "python"},

    // C/C++
    {".c", "c"},
    {".h", "c"},
    {".cpp", "cpp"},
    {".cc", "cpp"},
    {".cxx", "cpp"},
    {".hpp", "cpp"},
    {".hxx", "cpp"},

    // JavaScript/TypeScript
    {".js", "javascript"},
    {".mjs", "javascript"},
    {".jsx", "javascript"},
    {".ts", "typescript"},
    {".tsx", "typescript"},

    // Go
    {".go", "go"},

    // Rust
    {".rs", "rust"},

    // Ruby
    {".rb", "ruby"},
    {".rake", "ruby"},

    // Java/Kotlin
    {".java", "java"},
    {".kt", "kotlin"},
    {".kts", "kotlin"},

    // Data formats
    {".json", "json"},
    {".yml", "yaml"},
    {".yaml", "yaml"},
    {".toml", "toml"},
    {".xml", "xml"},

    // SQL
    {".sql", "sql"},

    // Docker/Config
    {".dockerfile", "dockerfile"},

    // Makefile
    {".mk", "makefile"},

    // Markdown
    {".md", "markdown"},
    {".markdown", "markdown"},

    // Lua
    {".lua", "lua"},

    // Perl
    {".pl", "perl"},
    {".pm", "perl"},

    // PHP
    {".php", "php"},

    // Swift
    {".swift", "swift"},

    // Scala
    {".scala", "scala"},
    {".sc", "scala"},

    // R
    {".r", "r"},
    {".R", "r"},

    // Haskell
    {".hs", "haskell"},

    // Elixir
    {".ex", "elixir"},
    {".exs", "elixir"},

    // CSS
    {".css", "css"},
    {".scss", "scss"},
    {".sass", "sass"},
    {".less", "less"},

    // HTML
    {".html", "html"},
    {".htm", "html"},

    // Vim
    {".vim", "vim"},
    {".vimrc", "vim"},
};

// Shebang interpreter to language mapping
const std::unordered_map<std::string, std::string> SHEBANG_MAP = {
    {"bash", "bash"},
    {"sh", "bash"},
    {"zsh", "zsh"},
    {"fish", "fish"},
    {"python", "python"},
    {"python3", "python"},
    {"python2", "python"},
    {"node", "javascript"},
    {"nodejs", "javascript"},
    {"ruby", "ruby"},
    {"perl", "perl"},
    {"php", "php"},
    {"lua", "lua"},
    {"awk", "awk"},
    {"sed", "sed"},
};

std::string to_lower(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return result;
}

}  // namespace

std::string LanguageDetector::detect(const std::string& content,
                                     const std::string& filename) {
    // Try shebang first (most reliable for scripts)
    std::string lang = from_shebang(content);
    if (!lang.empty()) {
        return lang;
    }

    // Try extension
    lang = from_extension(filename);
    if (!lang.empty()) {
        return lang;
    }

    // Check for special filenames
    std::string lower_filename = to_lower(filename);
    if (lower_filename == "dockerfile" || lower_filename.find("dockerfile") == 0) {
        return "dockerfile";
    }
    if (lower_filename == "makefile" || lower_filename == "gnumakefile") {
        return "makefile";
    }
    if (lower_filename == ".bashrc" || lower_filename == ".bash_profile" ||
        lower_filename == ".zshrc" || lower_filename == ".profile") {
        return "bash";
    }
    if (lower_filename == ".gitignore" || lower_filename == ".gitattributes") {
        return "gitconfig";
    }

    return "text";
}

std::string LanguageDetector::from_extension(const std::string& filename) {
    if (filename.empty()) {
        return "";
    }

    // Find the last dot
    size_t dot_pos = filename.rfind('.');
    if (dot_pos == std::string::npos || dot_pos == filename.length() - 1) {
        return "";
    }

    std::string ext = to_lower(filename.substr(dot_pos));
    auto it = EXTENSION_MAP.find(ext);
    if (it != EXTENSION_MAP.end()) {
        return it->second;
    }

    return "";
}

std::string LanguageDetector::from_shebang(const std::string& content) {
    if (content.size() < 2 || content[0] != '#' || content[1] != '!') {
        return "";
    }

    // Find end of first line
    size_t line_end = content.find('\n');
    if (line_end == std::string::npos) {
        line_end = content.length();
    }

    std::string shebang = content.substr(2, line_end - 2);

    // Handle /usr/bin/env <interpreter>
    size_t env_pos = shebang.find("env ");
    if (env_pos != std::string::npos) {
        size_t interp_start = env_pos + 4;
        // Skip whitespace
        while (interp_start < shebang.length() && shebang[interp_start] == ' ') {
            interp_start++;
        }
        size_t interp_end = shebang.find_first_of(" \t\n", interp_start);
        if (interp_end == std::string::npos) {
            interp_end = shebang.length();
        }
        std::string interpreter = shebang.substr(interp_start, interp_end - interp_start);

        auto it = SHEBANG_MAP.find(interpreter);
        if (it != SHEBANG_MAP.end()) {
            return it->second;
        }
        return interpreter;  // Return as-is if not in map
    }

    // Handle direct path like /bin/bash
    size_t last_slash = shebang.rfind('/');
    if (last_slash != std::string::npos) {
        size_t interp_start = last_slash + 1;
        size_t interp_end = shebang.find_first_of(" \t\n", interp_start);
        if (interp_end == std::string::npos) {
            interp_end = shebang.length();
        }
        std::string interpreter = shebang.substr(interp_start, interp_end - interp_start);

        auto it = SHEBANG_MAP.find(interpreter);
        if (it != SHEBANG_MAP.end()) {
            return it->second;
        }
        return interpreter;
    }

    return "";
}

}  // namespace dam
