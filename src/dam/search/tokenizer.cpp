#include <dam/search/tokenizer.hpp>

#include <algorithm>
#include <cctype>
#include <sstream>

namespace dam::search {

Tokenizer::Tokenizer(TokenizerConfig config)
    : config_(std::move(config)) {}

std::string Tokenizer::normalize(const std::string& token) const {
    std::string result = token;

    if (config_.lowercase) {
        std::transform(result.begin(), result.end(), result.begin(),
                       [](unsigned char c) { return std::tolower(c); });
    }

    return result;
}

bool Tokenizer::is_valid_token(const std::string& token) const {
    if (token.empty()) return false;

    int len = static_cast<int>(token.size());
    if (len < config_.min_token_length || len > config_.max_token_length) {
        return false;
    }

    // Check if it's just punctuation or whitespace
    bool has_alnum = false;
    for (char c : token) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            has_alnum = true;
            break;
        }
    }

    if (!has_alnum && !config_.keep_numbers) {
        return false;
    }

    return true;
}

bool Tokenizer::is_stop_word(const std::string& word) const {
    return config_.stop_words.count(word) > 0;
}

std::vector<std::string> Tokenizer::split_camel_case(const std::string& s) const {
    std::vector<std::string> result;
    std::string current;

    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];

        if (i > 0 && std::isupper(static_cast<unsigned char>(c))) {
            // Check if this is start of new word
            bool prev_lower = std::islower(static_cast<unsigned char>(s[i-1]));
            bool next_lower = (i + 1 < s.size()) &&
                              std::islower(static_cast<unsigned char>(s[i+1]));

            if (prev_lower || next_lower) {
                if (!current.empty()) {
                    result.push_back(current);
                    current.clear();
                }
            }
        }

        current += c;
    }

    if (!current.empty()) {
        result.push_back(current);
    }

    return result;
}

std::vector<std::string> Tokenizer::split_snake_case(const std::string& s) const {
    std::vector<std::string> result;
    std::istringstream ss(s);
    std::string token;

    while (std::getline(ss, token, '_')) {
        if (!token.empty()) {
            result.push_back(token);
        }
    }

    return result;
}

std::vector<std::string> Tokenizer::split_on_punctuation(const std::string& s) const {
    std::vector<std::string> result;
    std::string current;

    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            current += c;
        } else if (!current.empty()) {
            result.push_back(current);
            current.clear();
        }
    }

    if (!current.empty()) {
        result.push_back(current);
    }

    return result;
}

std::vector<std::string> Tokenizer::tokenize(const std::string& text) const {
    std::vector<std::string> tokens;

    // First split on whitespace and punctuation
    std::vector<std::string> words = split_on_punctuation(text);

    for (const auto& word : words) {
        std::vector<std::string> sub_tokens;

        // Apply snake_case splitting
        if (config_.split_snake_case && word.find('_') != std::string::npos) {
            sub_tokens = split_snake_case(word);
        } else {
            sub_tokens.push_back(word);
        }

        // Apply camelCase splitting to each sub-token
        if (config_.split_camel_case) {
            std::vector<std::string> final_tokens;
            for (const auto& sub : sub_tokens) {
                auto camel_parts = split_camel_case(sub);
                for (const auto& part : camel_parts) {
                    final_tokens.push_back(part);
                }
            }
            sub_tokens = std::move(final_tokens);
        }

        // Normalize and filter
        for (const auto& token : sub_tokens) {
            std::string normalized = normalize(token);

            if (is_valid_token(normalized) && !is_stop_word(normalized)) {
                tokens.push_back(normalized);
            }
        }
    }

    return tokens;
}

std::vector<std::pair<std::string, uint32_t>> Tokenizer::tokenize_with_positions(
    const std::string& text) const {

    std::vector<std::pair<std::string, uint32_t>> result;
    uint32_t position = 0;

    // Split on whitespace first to track positions
    std::istringstream ss(text);
    std::string word;

    while (ss >> word) {
        // Tokenize this word
        auto tokens = tokenize(word);

        for (const auto& token : tokens) {
            result.emplace_back(token, position);
        }

        position++;
    }

    return result;
}

std::vector<std::string> Tokenizer::tokenize_code(const std::string& code) const {
    std::vector<std::string> tokens;

    // Split on whitespace and common code delimiters
    std::string current;
    bool in_string = false;
    char string_char = 0;

    for (size_t i = 0; i < code.size(); ++i) {
        char c = code[i];

        // Handle string literals (skip content)
        if ((c == '"' || c == '\'') && (i == 0 || code[i-1] != '\\')) {
            if (!in_string) {
                in_string = true;
                string_char = c;
            } else if (c == string_char) {
                in_string = false;
            }
            continue;
        }

        if (in_string) continue;

        // Handle comments (simple // and # style)
        if (c == '/' && i + 1 < code.size() && code[i+1] == '/') {
            // Skip to end of line
            while (i < code.size() && code[i] != '\n') ++i;
            continue;
        }
        if (c == '#') {
            while (i < code.size() && code[i] != '\n') ++i;
            continue;
        }

        // Token boundaries
        if (std::isspace(static_cast<unsigned char>(c)) ||
            c == '(' || c == ')' || c == '{' || c == '}' ||
            c == '[' || c == ']' || c == ';' || c == ',' ||
            c == ':' || c == '.' || c == '=' || c == '+' ||
            c == '-' || c == '*' || c == '/' || c == '<' ||
            c == '>' || c == '!' || c == '&' || c == '|') {

            if (!current.empty()) {
                // Process current token
                auto sub_tokens = tokenize(current);
                for (const auto& t : sub_tokens) {
                    tokens.push_back(t);
                }
                current.clear();
            }
        } else {
            current += c;
        }
    }

    // Don't forget last token
    if (!current.empty()) {
        auto sub_tokens = tokenize(current);
        for (const auto& t : sub_tokens) {
            tokens.push_back(t);
        }
    }

    return tokens;
}

std::set<std::string> Tokenizer::unique_terms(const std::string& text) const {
    auto tokens = tokenize(text);
    return std::set<std::string>(tokens.begin(), tokens.end());
}

std::set<std::string> Tokenizer::default_code_stop_words() {
    return {
        // Common English articles/prepositions
        "a", "an", "the", "and", "or", "but", "in", "on", "at", "to", "for",
        "of", "with", "by", "from", "as", "is", "was", "are", "were", "been",
        "be", "have", "has", "had", "do", "does", "did", "will", "would",
        "could", "should", "may", "might", "must", "shall", "can",

        // Common code words that are too generic
        "var", "let", "const", "new", "this", "self", "true", "false", "null",
        "nil", "none", "void", "int", "string", "bool", "float", "double",

        // Very short common words
        "if", "it", "so", "no", "up", "go"
    };
}

}  // namespace dam::search
