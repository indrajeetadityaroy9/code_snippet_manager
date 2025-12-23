#include "terminal.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>

#if defined(__unix__) || defined(__APPLE__) || defined(__MACH__)
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#include <signal.h>
#endif

namespace dam::cli {

// Global pointer for signal handler to restore terminal
static Terminal* g_active_terminal = nullptr;

Terminal::Terminal() {
    if (!is_tty()) {
        throw std::runtime_error("Not running in an interactive terminal");
    }

    setup_signal_handlers();
    enable_raw_mode();
    g_active_terminal = this;
}

Terminal::~Terminal() {
    if (raw_mode_active_) {
        disable_raw_mode();
    }
    if (g_active_terminal == this) {
        g_active_terminal = nullptr;
    }
    cleanup_signal_handlers();
}

bool Terminal::is_tty() {
#if defined(__unix__) || defined(__APPLE__) || defined(__MACH__)
    return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
#elif defined(_WIN32)
    return _isatty(_fileno(stdin)) && _isatty(_fileno(stdout));
#else
    return false;
#endif
}

std::pair<int, int> Terminal::get_size() {
    int cols = 80, rows = 24;  // Defaults

#if defined(__unix__) || defined(__APPLE__) || defined(__MACH__)
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        cols = ws.ws_col;
        rows = ws.ws_row;
    }
#elif defined(_WIN32)
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    }
#endif

    return {cols, rows};
}

void Terminal::enable_raw_mode() {
#if defined(__unix__) || defined(__APPLE__) || defined(__MACH__)
    if (tcgetattr(STDIN_FILENO, &original_termios_) == -1) {
        throw std::runtime_error("Failed to get terminal attributes");
    }

    struct termios raw = original_termios_;

    // Input modes: no break, no CR to NL, no parity check, no strip char, no start/stop
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);

    // Output modes: disable post processing
    raw.c_oflag &= ~(OPOST);

    // Control modes: set 8 bit chars
    raw.c_cflag |= (CS8);

    // Local modes: no echo, no canonical, no extended functions, no signal chars
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

    // Control chars: set minimum chars to 0 (non-blocking)
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        throw std::runtime_error("Failed to set terminal to raw mode");
    }

#elif defined(_WIN32)
    stdin_handle_ = GetStdHandle(STD_INPUT_HANDLE);
    stdout_handle_ = GetStdHandle(STD_OUTPUT_HANDLE);

    if (!GetConsoleMode(stdin_handle_, &original_console_mode_)) {
        throw std::runtime_error("Failed to get console mode");
    }

    DWORD raw_mode = original_console_mode_;
    raw_mode &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
    raw_mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;

    if (!SetConsoleMode(stdin_handle_, raw_mode)) {
        throw std::runtime_error("Failed to set raw mode");
    }

    // Enable ANSI escape sequences on Windows
    DWORD out_mode;
    GetConsoleMode(stdout_handle_, &out_mode);
    out_mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(stdout_handle_, out_mode);
#endif

    raw_mode_active_ = true;
}

void Terminal::disable_raw_mode() {
    if (!raw_mode_active_) return;

#if defined(__unix__) || defined(__APPLE__) || defined(__MACH__)
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_termios_);
#elif defined(_WIN32)
    SetConsoleMode(stdin_handle_, original_console_mode_);
#endif

    raw_mode_active_ = false;
}

std::optional<KeyEvent> Terminal::read_key(int timeout_ms) {
#if defined(__unix__) || defined(__APPLE__) || defined(__MACH__)
    // Use select() for timeout handling
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);

    struct timeval tv;
    struct timeval* tv_ptr = nullptr;

    if (timeout_ms >= 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        tv_ptr = &tv;
    }

    int ret = select(STDIN_FILENO + 1, &fds, nullptr, nullptr, tv_ptr);
    if (ret <= 0) {
        return std::nullopt;  // Timeout or error
    }

    char c;
    if (read(STDIN_FILENO, &c, 1) != 1) {
        return std::nullopt;
    }

    KeyEvent event;

    // Handle escape sequences
    if (c == '\x1b') {
        return parse_escape_sequence();
    }

    // Handle control characters
    if (c == '\r' || c == '\n') {
        event.key = Key::ENTER;
        return event;
    }
    if (c == '\t') {
        event.key = Key::TAB;
        return event;
    }
    if (c == 127 || c == '\b') {
        event.key = Key::BACKSPACE;
        return event;
    }
    if (c == 3) {  // Ctrl+C
        event.key = Key::CTRL_C;
        event.ctrl = true;
        return event;
    }
    if (c == 4) {  // Ctrl+D
        event.key = Key::CTRL_D;
        event.ctrl = true;
        return event;
    }
    if (c == 26) {  // Ctrl+Z
        event.key = Key::CTRL_Z;
        event.ctrl = true;
        return event;
    }

    // Check for other control characters (Ctrl+A through Ctrl+Z)
    if (c >= 1 && c <= 26) {
        event.key = Key::CHAR;
        event.ch = 'a' + c - 1;
        event.ctrl = true;
        return event;
    }

    // Regular character
    if (c >= 32 && c < 127) {
        event.key = Key::CHAR;
        event.ch = c;
        return event;
    }

    event.key = Key::UNKNOWN;
    return event;

#elif defined(_WIN32)
    DWORD events_read;
    INPUT_RECORD input_record;

    if (timeout_ms >= 0) {
        DWORD result = WaitForSingleObject(stdin_handle_, timeout_ms);
        if (result != WAIT_OBJECT_0) {
            return std::nullopt;
        }
    }

    if (!ReadConsoleInput(stdin_handle_, &input_record, 1, &events_read) || events_read == 0) {
        return std::nullopt;
    }

    if (input_record.EventType != KEY_EVENT || !input_record.Event.KeyEvent.bKeyDown) {
        return std::nullopt;
    }

    KeyEvent event;
    auto& key = input_record.Event.KeyEvent;

    event.ctrl = (key.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
    event.alt = (key.dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;
    event.shift = (key.dwControlKeyState & SHIFT_PRESSED) != 0;

    switch (key.wVirtualKeyCode) {
        case VK_RETURN: event.key = Key::ENTER; break;
        case VK_TAB: event.key = Key::TAB; break;
        case VK_BACK: event.key = Key::BACKSPACE; break;
        case VK_DELETE: event.key = Key::DELETE_KEY; break;
        case VK_ESCAPE: event.key = Key::ESCAPE; break;
        case VK_UP: event.key = Key::UP; break;
        case VK_DOWN: event.key = Key::DOWN; break;
        case VK_LEFT: event.key = Key::LEFT; break;
        case VK_RIGHT: event.key = Key::RIGHT; break;
        case VK_HOME: event.key = Key::HOME; break;
        case VK_END: event.key = Key::END; break;
        case VK_PRIOR: event.key = Key::PAGE_UP; break;
        case VK_NEXT: event.key = Key::PAGE_DOWN; break;
        default:
            if (key.uChar.AsciiChar >= 32) {
                event.key = Key::CHAR;
                event.ch = key.uChar.AsciiChar;
            } else if (event.ctrl && key.wVirtualKeyCode >= 'A' && key.wVirtualKeyCode <= 'Z') {
                event.key = Key::CHAR;
                event.ch = 'a' + (key.wVirtualKeyCode - 'A');
            } else {
                event.key = Key::UNKNOWN;
            }
            break;
    }

    return event;
#else
    return std::nullopt;
#endif
}

KeyEvent Terminal::parse_escape_sequence() {
    KeyEvent event;
    event.key = Key::ESCAPE;

#if defined(__unix__) || defined(__APPLE__) || defined(__MACH__)
    // Try to read more characters (escape sequence)
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);

    struct timeval tv = {0, 50000};  // 50ms timeout for escape sequence

    if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) <= 0) {
        return event;  // Just escape key
    }

    char seq[8] = {0};
    ssize_t len = read(STDIN_FILENO, seq, sizeof(seq) - 1);
    if (len <= 0) {
        return event;
    }

    // CSI sequences: ESC [
    if (seq[0] == '[') {
        if (len == 1) {
            // Wait for more
            if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0) {
                len += read(STDIN_FILENO, seq + 1, sizeof(seq) - 2);
            }
        }

        switch (seq[1]) {
            case 'A': event.key = Key::UP; break;
            case 'B': event.key = Key::DOWN; break;
            case 'C': event.key = Key::RIGHT; break;
            case 'D': event.key = Key::LEFT; break;
            case 'H': event.key = Key::HOME; break;
            case 'F': event.key = Key::END; break;
            case '1':
                if (seq[2] == '~') event.key = Key::HOME;
                else if (seq[2] == ';' && seq[3] == '5' && seq[4] == 'A') {
                    event.key = Key::UP; event.ctrl = true;
                }
                break;
            case '3':
                if (seq[2] == '~') event.key = Key::DELETE_KEY;
                break;
            case '4':
                if (seq[2] == '~') event.key = Key::END;
                break;
            case '5':
                if (seq[2] == '~') event.key = Key::PAGE_UP;
                break;
            case '6':
                if (seq[2] == '~') event.key = Key::PAGE_DOWN;
                break;
            default:
                break;
        }
    }
    // SS3 sequences: ESC O
    else if (seq[0] == 'O') {
        switch (seq[1]) {
            case 'H': event.key = Key::HOME; break;
            case 'F': event.key = Key::END; break;
            case 'P': break;  // F1
            case 'Q': break;  // F2
            case 'R': break;  // F3
            case 'S': break;  // F4
            default: break;
        }
    }
    // Alt+key: ESC followed by key
    else if (len == 1 && seq[0] >= 'a' && seq[0] <= 'z') {
        event.key = Key::CHAR;
        event.ch = seq[0];
        event.alt = true;
    }
#endif

    return event;
}

void Terminal::write(const std::string& s) {
    std::cout << s;
}

void Terminal::write_colored(const std::string& s, const char* color) {
    std::cout << color << s << colors::RESET;
}

void Terminal::move_cursor(int col, int row) {
    std::cout << "\033[" << (row + 1) << ";" << (col + 1) << "H";
}

void Terminal::move_cursor_relative(int dcol, int drow) {
    if (drow > 0) {
        std::cout << "\033[" << drow << "B";
    } else if (drow < 0) {
        std::cout << "\033[" << (-drow) << "A";
    }
    if (dcol > 0) {
        std::cout << "\033[" << dcol << "C";
    } else if (dcol < 0) {
        std::cout << "\033[" << (-dcol) << "D";
    }
}

void Terminal::clear_line() {
    std::cout << "\033[2K\r";
}

void Terminal::clear_to_end_of_line() {
    std::cout << "\033[K";
}

void Terminal::clear_screen() {
    std::cout << "\033[2J\033[H";
}

void Terminal::save_cursor() {
    std::cout << "\033[s";
}

void Terminal::restore_cursor() {
    std::cout << "\033[u";
}

void Terminal::hide_cursor() {
    std::cout << "\033[?25l";
}

void Terminal::show_cursor() {
    std::cout << "\033[?25h";
}

void Terminal::flush() {
    std::cout.flush();
}

void Terminal::bell() {
    std::cout << '\a';
    flush();
}

void Terminal::setup_signal_handlers() {
#if defined(__unix__) || defined(__APPLE__) || defined(__MACH__)
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGQUIT, &sa, nullptr);
#endif
}

void Terminal::cleanup_signal_handlers() {
#if defined(__unix__) || defined(__APPLE__) || defined(__MACH__)
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
#endif
}

void Terminal::signal_handler(int sig) {
    // Restore terminal before exiting
    if (g_active_terminal) {
        g_active_terminal->disable_raw_mode();
        g_active_terminal->show_cursor();
        g_active_terminal->flush();
    }

    // Re-raise signal with default handler
    signal(sig, SIG_DFL);
    raise(sig);
}

}  // namespace dam::cli
