#include "input_classifier.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace dam::cli {

namespace {

// Natural language command starters (lowercase)
const std::vector<std::string> NL_STARTERS = {
    "write ", "create ", "generate ", "make ", "build ", "implement ",
    "add ", "define ", "show ", "give ", "help ", "how ", "what ",
    "can you ", "please ", "i need ", "i want ", "could you ",
    "design ", "develop ", "construct ", "produce ", "compose ",
    "explain ", "describe ", "convert ", "transform ", "translate "
};

// Code syntax indicators
const std::vector<std::string> CODE_KEYWORDS = {
    "def ", "class ", "function ", "func ", "fn ", "pub ", "private ",
    "const ", "let ", "var ", "int ", "void ", "struct ", "enum ",
    "import ", "from ", "include ", "using ", "package ", "module ",
    "if ", "else ", "elif ", "for ", "while ", "switch ", "match ",
    "return ", "yield ", "async ", "await ", "try ", "catch ", "throw ",
    "public ", "protected ", "static ", "final ", "abstract ",
    "interface ", "trait ", "impl ", "type ", "typedef ",
    "namespace ", "template ", "typename ", "auto ", "register ",
    "extern ", "inline ", "virtual ", "override ", "new ", "delete "
};

// Strong code indicators (patterns that almost certainly indicate code)
const std::vector<std::string> STRONG_CODE_PATTERNS = {
    "#!/", "#include", "#define", "#pragma", "#ifdef", "#ifndef",
    "->", "=>", "::", "<<", ">>", "&&", "||",
    "++", "--", "+=", "-=", "*=", "/=", "==", "!=", "<=", ">=",
    "[]", "{}", "()", "/**", "/*", "*/", "//",
    "std::", "self.", "this.", "super.",
    "lambda", "@property", "@staticmethod", "@classmethod",
    "console.log", "System.out", "fmt.Print"
};

// Conversational words that suggest natural language
const std::vector<std::string> CONVERSATIONAL_WORDS = {
    " the ", " a ", " an ", " that ", " which ", " this ",
    " with ", " from ", " into ", " onto ", " about ",
    " please ", " thanks ", " would ", " could ", " should ",
    " me ", " my ", " your ", " our ", " their "
};

}  // namespace

InputType InputClassifier::classify(const std::string& input,
                                     const std::string& language_hint) {
    // Trim leading whitespace
    size_t start = input.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) {
        return InputType::EMPTY;
    }

    std::string trimmed = input.substr(start);

    // Very short inputs are ambiguous
    if (trimmed.length() < 3) {
        return InputType::AMBIGUOUS;
    }

    // Check for shebang (definitely code)
    if (has_shebang(trimmed)) {
        return InputType::CODE;
    }

    float code_score = calculate_code_score(trimmed);
    float nl_score = calculate_nl_score(trimmed);

    // Clear code indicators
    if (code_score > 0.6f) {
        return InputType::CODE;
    }

    // Clear natural language (and low code score)
    if (nl_score > 0.5f && code_score < 0.3f) {
        return InputType::NATURAL_LANG;
    }

    // High confidence NL
    if (nl_score > 0.7f) {
        return InputType::NATURAL_LANG;
    }

    // Default to ambiguous (which the editor treats as code)
    return InputType::AMBIGUOUS;
}

float InputClassifier::calculate_code_score(const std::string& input) {
    float score = 0.0f;

    // Check for strong code patterns (highest weight)
    for (const auto& pattern : STRONG_CODE_PATTERNS) {
        if (input.find(pattern) != std::string::npos) {
            score += 0.4f;
            break;  // One strong pattern is enough
        }
    }

    // Check for code keywords
    std::string lower = input;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    int keyword_count = 0;
    for (const auto& keyword : CODE_KEYWORDS) {
        if (lower.find(keyword) != std::string::npos) {
            keyword_count++;
            if (keyword_count >= 2) break;  // Cap the contribution
        }
    }
    score += keyword_count * 0.2f;

    // Check for programming punctuation patterns
    int brackets = 0;
    int braces = 0;
    int semicolons = 0;
    int operators = 0;
    int colons = 0;

    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        if (c == '(' || c == ')') brackets++;
        if (c == '{' || c == '}') braces++;
        if (c == ';') semicolons++;
        if (c == ':' && (i + 1 >= input.size() || input[i + 1] != ':')) colons++;
        if (c == '=' || c == '+' || c == '-' || c == '*' || c == '/' || c == '%') operators++;
    }

    if (brackets >= 2) score += 0.15f;
    if (braces >= 2) score += 0.2f;
    if (semicolons >= 1) score += 0.15f;
    if (operators >= 2) score += 0.1f;

    // Indentation suggests code structure
    if (input.find("    ") != std::string::npos ||
        input.find("\t") != std::string::npos) {
        score += 0.1f;
    }

    // Multiple lines with consistent indentation
    size_t newlines = std::count(input.begin(), input.end(), '\n');
    if (newlines >= 2) {
        score += 0.1f;
    }

    return std::min(1.0f, score);
}

float InputClassifier::calculate_nl_score(const std::string& input) {
    float score = 0.0f;

    // Convert to lowercase for comparison
    std::string lower = input;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // Check for natural language starters (highest weight)
    if (starts_with_nl_phrase(lower)) {
        score += 0.5f;
    }

    // Check for question patterns
    if (lower.back() == '?' ||
        lower.find("how to") != std::string::npos ||
        lower.find("what is") != std::string::npos ||
        lower.find("how do") != std::string::npos ||
        lower.find("can i") != std::string::npos) {
        score += 0.25f;
    }

    // Check for conversational words
    int conversational_count = 0;
    for (const auto& word : CONVERSATIONAL_WORDS) {
        if (lower.find(word) != std::string::npos) {
            conversational_count++;
            if (conversational_count >= 3) break;
        }
    }
    score += conversational_count * 0.1f;

    // Low special character ratio suggests prose
    int alphanumeric = 0;
    int special = 0;
    for (char c : input) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ') {
            alphanumeric++;
        } else if (c != '\n' && c != '\r' && c != '\t') {
            special++;
        }
    }

    if (alphanumeric > 0) {
        float special_ratio = static_cast<float>(special) / alphanumeric;
        if (special_ratio < 0.05f) {
            score += 0.2f;  // Very few special chars
        } else if (special_ratio < 0.1f) {
            score += 0.1f;
        }
    }

    // Sentence-like structure (starts with capital, has spaces)
    if (!input.empty() && std::isupper(static_cast<unsigned char>(input[0]))) {
        int spaces = std::count(input.begin(), input.end(), ' ');
        if (spaces >= 3) {
            score += 0.1f;
        }
    }

    return std::min(1.0f, score);
}

bool InputClassifier::starts_with_nl_phrase(const std::string& lower_input) {
    for (const auto& starter : NL_STARTERS) {
        if (lower_input.find(starter) == 0) {
            return true;
        }
    }
    return false;
}

bool InputClassifier::has_code_syntax(const std::string& input) {
    return calculate_code_score(input) > 0.3f;
}

bool InputClassifier::has_shebang(const std::string& input) {
    return input.size() >= 2 && input[0] == '#' && input[1] == '!';
}

float InputClassifier::confidence(const std::string& input) {
    float code_score = calculate_code_score(input);
    float nl_score = calculate_nl_score(input);
    return std::abs(code_score - nl_score);
}

bool InputClassifier::is_natural_language(const std::string& input) {
    InputType type = classify(input);
    return type == InputType::NATURAL_LANG;
}

bool InputClassifier::is_code(const std::string& input) {
    InputType type = classify(input);
    return type == InputType::CODE || type == InputType::AMBIGUOUS;
}

const char* InputClassifier::type_name(InputType type) {
    switch (type) {
        case InputType::CODE: return "CODE";
        case InputType::NATURAL_LANG: return "NATURAL_LANG";
        case InputType::EMPTY: return "EMPTY";
        case InputType::AMBIGUOUS: return "AMBIGUOUS";
        default: return "UNKNOWN";
    }
}

}  // namespace dam::cli
