#pragma once

#include <string>
#include <vector>

namespace dam::cli {

/**
 * Classification result for input text.
 */
enum class InputType {
    CODE,           // Programming code - use syntax completion
    NATURAL_LANG,   // Natural language command - generate code
    EMPTY,          // Empty or whitespace only
    AMBIGUOUS       // Could be either - default to code
};

/**
 * Classifies user input to determine completion mode.
 *
 * Uses heuristics to distinguish between:
 * - Code being typed (for syntax completion)
 * - Natural language requests (for code generation)
 *
 * The classifier is designed to be fast and run on every keystroke.
 *
 * Examples:
 *   "def binary_search("         -> CODE
 *   "write a binary search"      -> NATURAL_LANG
 *   "create a function that"     -> NATURAL_LANG
 *   "if x > 0:"                  -> CODE
 *   "x = "                       -> AMBIGUOUS (defaults to CODE)
 */
class InputClassifier {
public:
    /**
     * Classify the input text.
     *
     * @param input The current input buffer
     * @param language_hint Optional language hint from context
     * @return Classification result
     */
    static InputType classify(const std::string& input,
                              const std::string& language_hint = "");

    /**
     * Get confidence score for classification (0.0-1.0).
     * Higher values indicate more confident classification.
     */
    static float confidence(const std::string& input);

    /**
     * Check if input looks like a natural language request.
     */
    static bool is_natural_language(const std::string& input);

    /**
     * Check if input looks like code.
     */
    static bool is_code(const std::string& input);

    /**
     * Get the classification type as a string for debugging.
     */
    static const char* type_name(InputType type);

private:
    // Scoring helpers
    static float calculate_code_score(const std::string& input);
    static float calculate_nl_score(const std::string& input);

    // Pattern matching helpers
    static bool starts_with_nl_phrase(const std::string& lower_input);
    static bool has_code_syntax(const std::string& input);
    static bool has_shebang(const std::string& input);
};

}  // namespace dam::cli
