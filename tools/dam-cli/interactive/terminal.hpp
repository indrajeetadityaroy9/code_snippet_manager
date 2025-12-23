#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>

#if defined(__unix__) || defined(__APPLE__) || defined(__MACH__)
#include <termios.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace dam::cli {

/**
 * Special key codes for terminal input.
 */
enum class Key {
    CHAR,           // Regular character (check ch field)
    ENTER,
    TAB,
    BACKSPACE,
    DELETE_KEY,
    ESCAPE,
    UP,
    DOWN,
    LEFT,
    RIGHT,
    HOME,
    END,
    PAGE_UP,
    PAGE_DOWN,
    CTRL_C,
    CTRL_D,
    CTRL_Z,
    CTRL_ENTER,
    UNKNOWN
};

/**
 * Represents a key press event.
 */
struct KeyEvent {
    Key key = Key::UNKNOWN;
    char ch = '\0';      // Valid when key == CHAR
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
};

/**
 * ANSI color codes for terminal output.
 */
namespace colors {
    constexpr const char* RESET = "\033[0m";
    constexpr const char* GRAY = "\033[90m";
    constexpr const char* RED = "\033[91m";
    constexpr const char* GREEN = "\033[92m";
    constexpr const char* YELLOW = "\033[93m";
    constexpr const char* BLUE = "\033[94m";
    constexpr const char* MAGENTA = "\033[95m";
    constexpr const char* CYAN = "\033[96m";
    constexpr const char* DIM = "\033[2m";
    constexpr const char* BOLD = "\033[1m";
    constexpr const char* ITALIC = "\033[3m";
    constexpr const char* UNDERLINE = "\033[4m";
}

/**
 * Raw terminal mode wrapper using termios (Unix) or Console API (Windows).
 *
 * Provides:
 * - Raw mode (no line buffering, no echo)
 * - Non-blocking input with timeout
 * - ANSI escape sequence handling
 * - Automatic cleanup on destruction (RAII)
 *
 * Thread safety: NOT thread-safe. Use one Terminal per thread.
 */
class Terminal {
public:
    /**
     * Enter raw mode. Throws std::runtime_error on failure.
     */
    Terminal();

    /**
     * Restore original terminal settings.
     */
    ~Terminal();

    // Non-copyable, non-movable (due to signal handler state)
    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;
    Terminal(Terminal&&) = delete;
    Terminal& operator=(Terminal&&) = delete;

    /**
     * Check if stdin is connected to an interactive terminal.
     */
    static bool is_tty();

    /**
     * Get terminal dimensions (columns, rows).
     * Returns (80, 24) as default if unable to determine.
     */
    static std::pair<int, int> get_size();

    /**
     * Read a single key event.
     * @param timeout_ms Timeout in milliseconds (-1 for blocking, 0 for non-blocking)
     * @return Key event, or nullopt on timeout/error
     */
    std::optional<KeyEvent> read_key(int timeout_ms = -1);

    /**
     * Write string to terminal output.
     */
    void write(const std::string& s);

    /**
     * Write string with ANSI color code.
     * @param s Text to write
     * @param color ANSI color code (e.g., colors::GRAY)
     */
    void write_colored(const std::string& s, const char* color);

    /**
     * Move cursor to absolute position (0-indexed).
     */
    void move_cursor(int col, int row);

    /**
     * Move cursor relative to current position.
     */
    void move_cursor_relative(int dcol, int drow);

    /**
     * Clear the entire current line.
     */
    void clear_line();

    /**
     * Clear from cursor to end of line.
     */
    void clear_to_end_of_line();

    /**
     * Clear the entire screen.
     */
    void clear_screen();

    /**
     * Save current cursor position.
     */
    void save_cursor();

    /**
     * Restore previously saved cursor position.
     */
    void restore_cursor();

    /**
     * Hide the cursor.
     */
    void hide_cursor();

    /**
     * Show the cursor.
     */
    void show_cursor();

    /**
     * Flush output buffer to terminal.
     */
    void flush();

    /**
     * Ring the terminal bell.
     */
    void bell();

private:
#if defined(__unix__) || defined(__APPLE__) || defined(__MACH__)
    struct termios original_termios_;
#elif defined(_WIN32)
    DWORD original_console_mode_;
    HANDLE stdin_handle_;
    HANDLE stdout_handle_;
#endif
    bool raw_mode_active_ = false;

    void enable_raw_mode();
    void disable_raw_mode();
    KeyEvent parse_escape_sequence();

    // Signal handling for clean terminal restoration
    static void setup_signal_handlers();
    static void cleanup_signal_handlers();
    static void signal_handler(int sig);
};

}  // namespace dam::cli
