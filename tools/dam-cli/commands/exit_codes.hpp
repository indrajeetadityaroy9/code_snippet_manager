#pragma once

namespace dam::cli {

// Standard exit codes for CLI commands
// Named with DAM_ prefix to avoid conflict with system macros
constexpr int DAM_EXIT_SUCCESS = 0;
constexpr int DAM_EXIT_USER_ERROR = 1;     // Invalid arguments, usage errors
constexpr int DAM_EXIT_NOT_FOUND = 2;      // Snippet/resource not found
constexpr int DAM_EXIT_IO_ERROR = 3;       // File/network/storage errors
constexpr int DAM_EXIT_INTERNAL = 4;       // Internal/unexpected errors

}  // namespace dam::cli
