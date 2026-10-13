// Raw-terminal handling and frame composition, using nothing but termios and
// ANSI escapes so the program builds with a bare compiler on any machine.
#pragma once

#include <csignal>
#include <cstdint>
#include <string>
#include <termios.h>

namespace btfind {

// Set from signal handlers; consumed by the main loop.
extern volatile sig_atomic_t g_resized;
extern volatile sig_atomic_t g_should_quit;

enum class Key {
    None,
    Char,
    Up,
    Down,
    Left,
    Right,
    Enter,
    Escape,
    Backspace,
    PageUp,
    PageDown,
    Home,
    End,
};

struct KeyEvent {
    Key key = Key::None;
    char ch = 0;  // valid when key == Key::Char
};

// Puts the terminal in raw mode on construction and restores it on destruction,
// including when unwinding through a signal-driven exit. Restoration is also
// wired to atexit, because a terminal left in raw mode with the alternate screen
// active is an unusable shell.
class Terminal {
public:
    explicit Terminal(bool use_color);
    ~Terminal();

    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    void refresh_size();
    int width() const { return width_; }
    int height() const { return height_; }

    bool color() const { return color_; }

    // Reads one key if available. Non-blocking; returns Key::None when nothing
    // is pending.
    KeyEvent read_key();

    // Writes a fully composed frame in a single syscall.
    void present(const std::string& frame);

    // Emits a terminal bell, used by the hunt mode's geiger pacing.
    void bell();

private:
    void restore();

    struct termios saved_ {};
    bool raw_active_ = false;
    bool color_ = true;
    int width_ = 80;
    int height_ = 24;
};

// Installs SIGWINCH/SIGINT/SIGTERM handlers that only set the flags above.
void install_signal_handlers();

namespace ansi {
inline constexpr const char* kReset = "\x1b[0m";
inline constexpr const char* kBold = "\x1b[1m";
inline constexpr const char* kDim = "\x1b[2m";
inline constexpr const char* kReverse = "\x1b[7m";
inline constexpr const char* kClearLine = "\x1b[K";
inline constexpr const char* kHome = "\x1b[H";
inline constexpr const char* kClearScreen = "\x1b[2J";

std::string fg256(int n);
std::string bg256(int n);
std::string move_to(int row, int col);
}  // namespace ansi

// --- text measurement -------------------------------------------------------
//
// Device names come off the air as arbitrary bytes and often contain UTF-8, so
// byte counts cannot be used for column layout without the table drifting.

// Display columns occupied by a UTF-8 string (CJK and emoji count as 2).
size_t display_width(const std::string& s);

// Truncates to `max_cols` display columns, appending an ellipsis if it cut.
std::string truncate_to(const std::string& s, size_t max_cols);

// Truncates then right-pads with spaces to exactly `cols` columns.
std::string fit(const std::string& s, size_t cols);

}  // namespace btfind
