#pragma once

#include <set>
#include <string>
#include <utility>
#include <vector>

namespace dam::search {

struct TokenizerConfig {
    bool lowercase = true;
    bool remove_punctuation = true;
    bool stem_words = false;  // Porter stemming (not implemented yet)
    int min_token_length = 2;
    int max_token_length = 64;
    std::set<std::string> stop_words;  // Words to ignore

    // Code-specific options
    bool split_camel_case = true;
    bool split_snake_case = true;
    bool keep_numbers = true;
};

class Tokenizer {
public:
    explicit Tokenizer(TokenizerConfig config = {});

    // Tokenize text into terms
    std::vector<std::string> tokenize(const std::string& text) const;

    // Tokenize with positions (for phrase queries)
    std::vector<std::pair<std::string, uint32_t>> tokenize_with_positions(
        const std::string& text) const;

    // Code-aware tokenization (handles camelCase, snake_case, special symbols)
    std::vector<std::string> tokenize_code(const std::string& code) const;

    // Get unique terms (for indexing)
    std::set<std::string> unique_terms(const std::string& text) const;

    // Configuration access
    const TokenizerConfig& config() const { return config_; }
    void set_config(TokenizerConfig config) { config_ = std::move(config); }

    // Default stop words for code
    static std::set<std::string> default_code_stop_words();

private:
    TokenizerConfig config_;

    std::string normalize(const std::string& token) const;
    bool is_valid_token(const std::string& token) const;
    bool is_stop_word(const std::string& word) const;

    // Code tokenization helpers
    std::vector<std::string> split_camel_case(const std::string& s) const;
    std::vector<std::string> split_snake_case(const std::string& s) const;
    std::vector<std::string> split_on_punctuation(const std::string& s) const;
};

}  // namespace dam::search
