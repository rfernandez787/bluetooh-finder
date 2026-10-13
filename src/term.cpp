#include "term.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstdio>

namespace btfind {

volatile sig_atomic_t g_resized = 0;
volatile sig_atomic_t g_should_quit = 0;

namespace {

// The one live Terminal, so the atexit hook can restore it. A second instance
// would be a programming error, not a supported mode.
Terminal* g_terminal_instance = nullptr;
struct termios g_saved_termios;
bool g_termios_saved = false;
bool g_screen_entered = false;

// Restore autowrap, show the cursor, leave the alternate screen. Written with a
// bare write() because iostreams are not safe to touch from an exit handler.
const char* kLeaveScreen = "\x1b[0m\x1b[?7h\x1b[?25h\x1b[?1049l";

void emergency_restore() {
    if (g_termios_saved) {
        ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_termios);
        g_termios_saved = false;
    }
    if (g_screen_entered) {
        ssize_t r = ::write(STDOUT_FILENO, kLeaveScreen, strlen(kLeaveScreen));
        (void)r;
        g_screen_entered = false;
    }
}

void on_winch(int) { g_resized = 1; }
void on_quit(int) { g_should_quit = 1; }

}  // namespace

void install_signal_handlers() {
    struct sigaction sa {};
    sa.sa_handler = on_winch;
    sigemptyset(&sa.sa_mask);
    // No SA_RESTART: a resize should break poll() so the frame is redrawn at once.
    sa.sa_flags = 0;
    ::sigaction(SIGWINCH, &sa, nullptr);

    sa.sa_handler = on_quit;
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);

    // A dead pipe must not kill us mid-frame; the write path handles the error.
    struct sigaction ign {};
    ign.sa_handler = SIG_IGN;
    ::sigaction(SIGPIPE, &ign, nullptr);
}

Terminal::Terminal(bool use_color) : color_(use_color) {
    g_terminal_instance = this;

    if (::tcgetattr(STDIN_FILENO, &saved_) == 0) {
        g_saved_termios = saved_;
        g_termios_saved = true;
        std::atexit(emergency_restore);

        struct termios raw = saved_;
        // Character-at-a-time input with no echo, and no signal/flow-control
        // interception so keys like Ctrl-S reach us instead of freezing output.
        raw.c_lflag &= ~(ECHO | ICANON | IEXTEN);
        raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
        raw.c_oflag &= ~(OPOST);
        raw.c_cflag |= CS8;
        // ISIG is deliberately kept so Ctrl-C still raises SIGINT and our handler
        // performs an orderly shutdown (scan disabled, terminal restored).
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        if (::tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0) raw_active_ = true;
    }

    // Autowrap off: every line this program draws is sized to the window, but a
    // single miscounted column would otherwise wrap, and a wrap on the bottom
    // row scrolls the entire screen out from under the layout.
    const char* enter = "\x1b[?1049h\x1b[?7l\x1b[?25l\x1b[2J";
    ssize_t r = ::write(STDOUT_FILENO, enter, strlen(enter));
    (void)r;
    g_screen_entered = true;

    refresh_size();
}

Terminal::~Terminal() {
    restore();
    g_terminal_instance = nullptr;
}

void Terminal::restore() {
    if (g_screen_entered) {
        ssize_t r = ::write(STDOUT_FILENO, kLeaveScreen, strlen(kLeaveScreen));
        (void)r;
        g_screen_entered = false;
    }
    if (raw_active_) {
        ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_);
        raw_active_ = false;
        g_termios_saved = false;
    }
}

void Terminal::refresh_size() {
    struct winsize ws {};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        width_ = ws.ws_col;
        height_ = ws.ws_row;
    } else {
        width_ = 80;
        height_ = 24;
    }
    // Guard the layout maths against absurdly small windows.
    if (width_ < 20) width_ = 20;
    if (height_ < 6) height_ = 6;
}

KeyEvent Terminal::read_key() {
    unsigned char c = 0;
    ssize_t n = ::read(STDIN_FILENO, &c, 1);
    if (n != 1) return {};

    if (c == '\r' || c == '\n') return {Key::Enter, 0};
    if (c == 127 || c == 8) return {Key::Backspace, 0};

    if (c != 0x1b) return {Key::Char, static_cast<char>(c)};

    // Escape, or the start of a sequence. VMIN=0/VTIME=0 means a bare Escape
    // reads back nothing more, which is exactly how we tell them apart.
    unsigned char seq[2];
    if (::read(STDIN_FILENO, &seq[0], 1) != 1) return {Key::Escape, 0};
    if (::read(STDIN_FILENO, &seq[1], 1) != 1) return {Key::Escape, 0};

    if (seq[0] == '[') {
        if (seq[1] >= '0' && seq[1] <= '9') {
            unsigned char tilde = 0;
            if (::read(STDIN_FILENO, &tilde, 1) != 1) return {Key::Escape, 0};
            if (tilde == '~') {
                switch (seq[1]) {
                    case '1': case '7': return {Key::Home, 0};
                    case '4': case '8': return {Key::End, 0};
                    case '5': return {Key::PageUp, 0};
                    case '6': return {Key::PageDown, 0};
                    default: return {};
                }
            }
            return {};
        }
        switch (seq[1]) {
            case 'A': return {Key::Up, 0};
            case 'B': return {Key::Down, 0};
            case 'C': return {Key::Right, 0};
            case 'D': return {Key::Left, 0};
            case 'H': return {Key::Home, 0};
            case 'F': return {Key::End, 0};
            default: return {};
        }
    }
    if (seq[0] == 'O') {
        switch (seq[1]) {
            case 'A': return {Key::Up, 0};
            case 'B': return {Key::Down, 0};
            case 'C': return {Key::Right, 0};
            case 'D': return {Key::Left, 0};
            case 'H': return {Key::Home, 0};
            case 'F': return {Key::End, 0};
            default: return {};
        }
    }
    return {Key::Escape, 0};
}

void Terminal::present(const std::string& frame) {
    size_t off = 0;
    while (off < frame.size()) {
        ssize_t n = ::write(STDOUT_FILENO, frame.data() + off, frame.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;  // a resize landed mid-write
            return;
        }
        off += static_cast<size_t>(n);
    }
}

void Terminal::bell() {
    ssize_t r = ::write(STDOUT_FILENO, "\a", 1);
    (void)r;
}

namespace ansi {

std::string fg256(int n) { return "\x1b[38;5;" + std::to_string(n) + "m"; }
std::string bg256(int n) { return "\x1b[48;5;" + std::to_string(n) + "m"; }

std::string move_to(int row, int col) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "\x1b[%d;%dH", row, col);
    return std::string(buf);
}

}  // namespace ansi

// --- UTF-8 aware measurement ------------------------------------------------

namespace {

// Rough east-asian-wide test. Enough to keep the columns aligned for the CJK and
// emoji that show up in device names; not a full Unicode width table.
bool is_wide(uint32_t cp) {
    return (cp >= 0x1100 && cp <= 0x115F) ||    // Hangul Jamo
           (cp >= 0x2E80 && cp <= 0xA4CF) ||    // CJK radicals .. Yi
           (cp >= 0xAC00 && cp <= 0xD7A3) ||    // Hangul syllables
           (cp >= 0xF900 && cp <= 0xFAFF) ||    // CJK compatibility
           (cp >= 0xFF00 && cp <= 0xFF60) ||    // fullwidth forms
           (cp >= 0xFFE0 && cp <= 0xFFE6) ||
           (cp >= 0x1F300 && cp <= 0x1F9FF);    // emoji
}

// Decodes one UTF-8 sequence at `i`, advancing it. Invalid bytes are consumed
// one at a time and reported as width 1, so malformed names cannot hang layout.
//
// ANSI escape sequences are consumed whole and reported as zero width. Colour is
// applied while composing, so measurement and truncation both run over strings
// that already contain escapes; counting those bytes as visible would make every
// coloured element measure far wider than it draws.
uint32_t decode(const std::string& s, size_t& i, int& width) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    size_t extra = 0;
    uint32_t cp = c;

    if (c == 0x1B) {
        size_t j = i + 1;
        if (j < s.size() && s[j] == '[') {
            // CSI: parameter and intermediate bytes, then a final byte 0x40-0x7E.
            ++j;
            while (j < s.size() &&
                   static_cast<unsigned char>(s[j]) >= 0x20 &&
                   static_cast<unsigned char>(s[j]) <= 0x3F) {
                ++j;
            }
            if (j < s.size()) ++j;  // the final byte
        } else if (j < s.size()) {
            ++j;  // two-character escape
        }
        i = j;
        width = 0;
        return 0;
    }

    if (c < 0x80) {
        extra = 0;
    } else if ((c & 0xE0) == 0xC0) {
        extra = 1;
        cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        extra = 2;
        cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        extra = 3;
        cp = c & 0x07;
    } else {
        ++i;
        width = 1;
        return 0xFFFD;
    }
    if (i + extra >= s.size()) {
        ++i;
        width = 1;
        return 0xFFFD;
    }
    for (size_t k = 1; k <= extra; ++k) {
        const unsigned char cc = static_cast<unsigned char>(s[i + k]);
        if ((cc & 0xC0) != 0x80) {
            ++i;
            width = 1;
            return 0xFFFD;
        }
        cp = (cp << 6) | (cc & 0x3F);
    }
    i += extra + 1;
    width = is_wide(cp) ? 2 : 1;
    return cp;
}

}  // namespace

size_t display_width(const std::string& s) {
    size_t i = 0, total = 0;
    while (i < s.size()) {
        int w = 1;
        decode(s, i, w);
        total += static_cast<size_t>(w);
    }
    return total;
}

std::string truncate_to(const std::string& s, size_t max_cols) {
    if (max_cols == 0) return "";
    if (display_width(s) <= max_cols) return s;

    // Reserve one column for the ellipsis.
    const size_t budget = max_cols > 1 ? max_cols - 1 : 1;
    size_t i = 0, used = 0, cut = 0;
    while (i < s.size()) {
        int w = 1;
        const size_t start = i;
        decode(s, i, w);
        if (used + static_cast<size_t>(w) > budget) {
            cut = start;
            break;
        }
        used += static_cast<size_t>(w);
        cut = i;
    }
    std::string out = s.substr(0, cut) + "…";
    // Cutting can drop the reset that closed a colour span, which would bleed
    // that colour across the rest of the terminal.
    if (s.find('\x1b') != std::string::npos) out += ansi::kReset;
    return out;
}

std::string fit(const std::string& s, size_t cols) {
    std::string t = truncate_to(s, cols);
    const size_t w = display_width(t);
    if (w < cols) t.append(cols - w, ' ');
    return t;
}

}  // namespace btfind
