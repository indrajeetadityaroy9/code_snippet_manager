#pragma once

#include <string>

namespace dam {

/**
 * Detects programming language from file content and/or filename.
 *
 * Detection priority:
 * 1. Shebang line (#!/bin/bash, #!/usr/bin/env python)
 * 2. File extension mapping
 * 3. Returns "text" if unknown
 */
class LanguageDetector {
public:
    /**
     * Detect language from content and optional filename.
     *
     * @param content The file content (checks shebang)
     * @param filename Optional filename (checks extension)
     * @return Detected language name (lowercase)
     */
    static std::string detect(const std::string& content,
                             const std::string& filename = "");

private:
    static std::string from_extension(const std::string& filename);
    static std::string from_shebang(const std::string& content);
};

}  // namespace dam
