#include "ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace btfind {
namespace {

// The usable RSSI span. Below -100 dBm a device is barely detectable; above
// -35 dBm it is essentially in your hand, and further gain tells you nothing.
constexpr double kRssiFloor = -100.0;
constexpr double kRssiCeil = -35.0;

// 256-colour ramp from "cold" to "hot", indexed by proximity fraction.
int heat_color(double frac) {
    if (frac >= 0.85) return 46;   // bright green
    if (frac >= 0.70) return 82;
    if (frac >= 0.55) return 154;
    if (frac >= 0.40) return 226;  // yellow
    if (frac >= 0.28) return 214;  // orange
    if (frac >= 0.15) return 208;
    return 203;                    // red
}

std::string colored(const std::string& s, int color, bool enable) {
    if (!enable) return s;
    return ansi::fg256(color) + s + ansi::kReset;
}

const char* trend_glyph(Trend t) {
    switch (t) {
        case Trend::Warmer: return "▲";
        case Trend::Colder: return "▼";
        case Trend::Steady: return "●";
    }
    return " ";
}

const char* trend_word(Trend t) {
    switch (t) {
        case Trend::Warmer: return "WARMER";
        case Trend::Colder: return "COLDER";
        case Trend::Steady: return "steady";
    }
    return "";
}

int trend_color(Trend t) {
    switch (t) {
        case Trend::Warmer: return 46;
        case Trend::Colder: return 203;
        case Trend::Steady: return 245;
    }
    return 250;
}

// Eighth-block characters for sub-cell resolution in bars and sparklines.
const char* kBlocks[9] = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};

}  // namespace

double rssi_fraction(double rssi) {
    return std::clamp((rssi - kRssiFloor) / (kRssiCeil - kRssiFloor), 0.0, 1.0);
}

std::string bar(double frac, int width, bool color, const char* fill_glyph) {
    if (width <= 0) return "";
    frac = std::clamp(frac, 0.0, 1.0);
    const int filled = static_cast<int>(std::lround(frac * width));
    const char* glyph = fill_glyph != nullptr ? fill_glyph : "█";

    std::string filled_part;
    for (int i = 0; i < filled; ++i) filled_part += glyph;
    std::string empty_part;
    for (int i = filled; i < width; ++i) empty_part += "·";

    if (!color) return filled_part + empty_part;
    return ansi::fg256(heat_color(frac)) + filled_part + ansi::fg256(238) +
           empty_part + ansi::kReset;
}

std::string sparkline(const RssiHistory& h, int width) {
    if (width <= 0 || h.empty()) return std::string(std::max(0, width), ' ');

    const size_t n = h.size();
    const size_t take = std::min(n, static_cast<size_t>(width));
    const size_t start = n - take;

    // Scale to the window's own range rather than the absolute RSSI range: over a
    // few metres of walking the absolute swing is small, and a fixed scale would
    // flatten the trace into a straight line exactly when it matters most.
    int lo = 127, hi = -128;
    for (size_t i = start; i < n; ++i) {
        const int v = h.rssi_at(i);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    if (hi - lo < 6) {  // keep a quiet signal from looking like wild noise
        const int mid = (hi + lo) / 2;
        lo = mid - 3;
        hi = mid + 3;
    }

    std::string out;
    // Left-pad so a short history stays right-aligned and the newest sample is
    // always in the same place.
    for (size_t i = take; i < static_cast<size_t>(width); ++i) out += " ";
    for (size_t i = start; i < n; ++i) {
        const double f = static_cast<double>(h.rssi_at(i) - lo) / (hi - lo);
        int idx = static_cast<int>(std::lround(std::clamp(f, 0.0, 1.0) * 8.0));
        if (idx < 1) idx = 1;  // a sample present is never a blank cell
        out += kBlocks[idx];
    }
    return out;
}

std::string format_distance(double m) {
    char buf[16];
    if (m < 10.0) {
        std::snprintf(buf, sizeof(buf), "~%.1fm", m);
    } else if (m < 100.0) {
        std::snprintf(buf, sizeof(buf), "~%.0fm", m);
    } else {
        std::snprintf(buf, sizeof(buf), "~%.0fm+", m);
    }
    return buf;
}

std::string format_age(uint64_t ms) {
    char buf[16];
    if (ms < 10000) {
        std::snprintf(buf, sizeof(buf), "%.1fs", ms / 1000.0);
    } else if (ms < 60000) {
        std::snprintf(buf, sizeof(buf), "%llus",
                      static_cast<unsigned long long>(ms / 1000));
    } else if (ms < 3600000) {
        std::snprintf(buf, sizeof(buf), "%llum",
                      static_cast<unsigned long long>(ms / 60000));
    } else {
        std::snprintf(buf, sizeof(buf), "%lluh",
                      static_cast<unsigned long long>(ms / 3600000));
    }
    return buf;
}

int bell_interval_ms(double rssi) {
    // Geiger-counter pacing: the click rate, not its pitch, carries the signal.
    const double f = rssi_fraction(rssi);
    return static_cast<int>(std::lround(1200.0 - f * (1200.0 - 90.0)));
}

// --- key handling -----------------------------------------------------------

const Device* Ui::selected_device() const {
    if (!has_selection_) return nullptr;
    return table_.find(selected_addr_);
}

void Ui::move_selection(int delta) {
    if (last_view_.empty()) {
        has_selection_ = false;
        return;
    }
    int idx = 0;
    if (has_selection_) {
        for (size_t i = 0; i < last_view_.size(); ++i) {
            if (last_view_[i]->addr == selected_addr_) {
                idx = static_cast<int>(i);
                break;
            }
        }
    }
    idx = std::clamp(idx + delta, 0, static_cast<int>(last_view_.size()) - 1);
    selected_addr_ = last_view_[idx]->addr;
    has_selection_ = true;
}

bool Ui::handle_key(const KeyEvent& ev, Terminal& term) {
    (void)term;

    // While typing a filter, nearly every key is text. Only the editing keys and
    // Enter/Escape are interpreted.
    if (filter_input_) {
        switch (ev.key) {
            case Key::Enter:
            case Key::Escape:
                filter_input_ = false;
                if (ev.key == Key::Escape) filter_.clear();
                return true;
            case Key::Backspace:
                if (!filter_.empty()) filter_.pop_back();
                return true;
            case Key::Char:
                if (ev.ch >= 0x20 && ev.ch < 0x7F && filter_.size() < 32) {
                    filter_.push_back(ev.ch);
                }
                return true;
            default:
                return true;
        }
    }

    if (mode_ == Mode::Help) {
        // Return where the user came from: asking for help mid-hunt should not
        // cost them the hunt.
        if (ev.key != Key::None) mode_ = help_return_mode_;
        return true;
    }

    if (mode_ == Mode::Hunt) {
        switch (ev.key) {
            case Key::Escape:
                mode_ = Mode::List;
                return true;
            case Key::Char:
                switch (ev.ch) {
                    case 'q':
                        mode_ = Mode::List;
                        return true;
                    case 'b':
                        cfg_.bell = !cfg_.bell;
                        next_bell_ms_ = 0;
                        return true;
                    case 'c': {
                        // Calibrate: declare the present reading to be 1 m. The
                        // distance estimate is only ever as good as this number,
                        // so letting the user set it beats any default.
                        if (Device* d = table_.find(hunt_addr_)) {
                            d->tx_1m_override = d->smoothed();
                        }
                        return true;
                    }
                    case 'C':
                        if (Device* d = table_.find(hunt_addr_)) {
                            d->tx_1m_override.reset();
                        }
                        return true;
                    case 'p':
                        paused_ = !paused_;
                        return true;
                    case '?':
                        help_return_mode_ = Mode::Hunt;
                        mode_ = Mode::Help;
                        return true;
                    default:
                        return true;
                }
            default:
                return true;
        }
    }

    // List mode.
    switch (ev.key) {
        case Key::Up: move_selection(-1); return true;
        case Key::Down: move_selection(1); return true;
        case Key::PageUp: move_selection(-10); return true;
        case Key::PageDown: move_selection(10); return true;
        case Key::Home: move_selection(-1000000); return true;
        case Key::End: move_selection(1000000); return true;
        case Key::Enter:
            if (const Device* d = selected_device()) {
                hunt_addr_ = d->addr;
                mode_ = Mode::Hunt;
                next_bell_ms_ = 0;
            }
            return true;
        case Key::Escape:
            if (!filter_.empty()) {
                filter_.clear();
                return true;
            }
            return false;
        case Key::Char:
            switch (ev.ch) {
                case 'q': return false;
                case 'j': move_selection(1); return true;
                case 'k': move_selection(-1); return true;
                case 'g': move_selection(-1000000); return true;
                case 'G': move_selection(1000000); return true;
                case 's': sort_ = next_sort_mode(sort_); return true;
                case 'p': paused_ = !paused_; return true;
                case 'n': named_only_ = !named_only_; return true;
                case 'b': cfg_.bell = !cfg_.bell; return true;
                case '/': filter_input_ = true; return true;
                case '?':
                    help_return_mode_ = Mode::List;
                    mode_ = Mode::Help;
                    return true;
                case 'r':
                    table_.clear();
                    has_selection_ = false;
                    return true;
                case 'f':
                    // Cycle a coarse RSSI floor: all -> near -> very near.
                    if (cfg_.min_rssi <= -110) {
                        cfg_.min_rssi = -80;
                    } else if (cfg_.min_rssi == -80) {
                        cfg_.min_rssi = -65;
                    } else {
                        cfg_.min_rssi = -110;
                    }
                    return true;
                default: return true;
            }
        default:
            return true;
    }
}

void Ui::tick_bell(Terminal& term) {
    if (!cfg_.bell || mode_ != Mode::Hunt) return;
    const Device* d = table_.find(hunt_addr_);
    if (d == nullptr) return;
    const uint64_t now = now_ms();
    // Go quiet rather than click at a stale reading — silence is the honest
    // signal that contact has been lost.
    if (now - d->last_seen > 5000) return;
    if (now < next_bell_ms_) return;
    term.bell();
    next_bell_ms_ = now + static_cast<uint64_t>(bell_interval_ms(d->smoothed()));
}

int Ui::next_wakeup_ms() const {
    constexpr int kFrameMs = 80;  // ~12 fps is plenty for a moving bar
    if (!cfg_.bell || mode_ != Mode::Hunt) return kFrameMs;
    const uint64_t now = now_ms();
    if (next_bell_ms_ <= now) return 0;
    return std::min<int>(kFrameMs, static_cast<int>(next_bell_ms_ - now));
}

// --- rendering --------------------------------------------------------------

std::string Ui::footer(const Terminal& term, const std::string& notice) const {
    const bool c = term.color();
    std::string s;
    if (filter_input_) {
        s = " /" + filter_ + "_";
        return c ? (ansi::fg256(226) + s + ansi::kReset) : s;
    }
    if (!notice.empty()) {
        return c ? (ansi::fg256(214) + " " + notice + ansi::kReset) : " " + notice;
    }
    const char* keys =
        mode_ == Mode::Hunt
            ? " esc back · c calibrate · b beep · p pause · ? help"
            : " ↑↓ select · ⏎ hunt · / filter · s sort · f floor · n named · "
              "p pause · b beep · r reset · ? help · q quit";
    std::string t = keys;
    return c ? (ansi::fg256(242) + t + ansi::kReset) : t;
}

std::string Ui::render(const Terminal& term, const std::string& source,
                       bool scanning_ok, const std::string& notice) {
    const int w = term.width();
    const int h = term.height();
    const bool c = term.color();

    std::string out;
    out.reserve(static_cast<size_t>(w) * h * 2);
    out += ansi::kHome;

    // Header line.
    {
        std::string state = !scanning_ok ? "STOPPED" : (paused_ ? "PAUSED" : "scanning");
        int state_color = !scanning_ok ? 203 : (paused_ ? 214 : 46);

        char info[192];
        std::snprintf(info, sizeof(info), " · %zu devices · sort:%s%s%s",
                      table_.size(), sort_mode_name(sort_),
                      named_only_ ? " · named only" : "",
                      cfg_.min_rssi > -110 ? " · floor" : "");

        std::string line;
        line += c ? std::string(ansi::kBold) + ansi::fg256(51) : "";
        line += " btfind";
        line += c ? ansi::kReset : "";
        line += c ? ansi::fg256(250) : "";
        line += "  " + source + " · ";
        line += c ? ansi::kReset : "";
        line += colored(state, state_color, c);
        line += c ? ansi::fg256(244) : "";
        line += info;
        line += c ? ansi::kReset : "";

        const size_t pw = display_width(line);
        if (pw < static_cast<size_t>(w)) {
            line.append(w - pw, ' ');
        } else if (pw > static_cast<size_t>(w)) {
            line = truncate_to(line, w);
        }
        out += line;
        out += ansi::kClearLine;
        out += "\r\n";
    }

    // Separator.
    {
        std::string rule;
        for (int i = 0; i < w; ++i) rule += "─";
        out += c ? ansi::fg256(238) + rule + ansi::kReset : rule;
        out += "\r\n";
    }

    const int body_top = 3;
    const int body_rows = std::max(1, h - 3);  // header, rule, footer

    switch (mode_) {
        case Mode::List: render_list(out, term, body_top, body_rows); break;
        case Mode::Hunt: render_hunt(out, term, body_top, body_rows); break;
        case Mode::Help: render_help(out, term, body_top, body_rows); break;
    }

    // Footer, pinned to the last row. Truncated hard: the bottom line is the one
    // place where an overlong string would scroll the whole screen.
    out += ansi::move_to(h, 1);
    out += ansi::kClearLine;
    out += truncate_to(footer(term, notice), w);
    return out;
}

void Ui::render_list(std::string& out, const Terminal& term, int top_row, int rows) {
    const int w = term.width();
    const bool c = term.color();
    const uint64_t now = now_ms();

    last_view_ = table_.view(sort_, cfg_.min_rssi, named_only_, filter_, now);

    // Keep a selection alive across sorting and filtering.
    if (!last_view_.empty()) {
        bool found = false;
        if (has_selection_) {
            for (const Device* d : last_view_) {
                if (d->addr == selected_addr_) {
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            selected_addr_ = last_view_.front()->addr;
            has_selection_ = true;
        }
    } else {
        has_selection_ = false;
    }

    // Responsive columns: drop the least useful first as the window narrows.
    const bool show_addr = w >= 74;
    const bool show_dist = w >= 56;
    const bool show_age = w >= 46;
    const int bar_w = w >= 90 ? 16 : (w >= 68 ? 12 : 8);

    // Every column below is accounted for exactly, so header and cells line up:
    // marker(2) bar gap rssi(4) gap [dist(8)] name gap [addr(18)] [age(6)]
    int fixed = 2 + bar_w + 1 + 4 + 1 + 1;
    if (show_dist) fixed += 8;
    if (show_addr) fixed += 18;
    if (show_age) fixed += 6;
    const int name_w = std::max(6, w - fixed);

    int row = top_row;
    const int list_rows = rows - 1;  // one row for the column header

    // Column header.
    {
        std::string hdr = "  ";
        hdr += fit("SIGNAL", bar_w) + " ";
        hdr += fit("RSSI", 4) + " ";
        if (show_dist) hdr += fit("DIST", 8);
        hdr += fit("NAME", name_w) + " ";
        if (show_addr) hdr += fit("ADDRESS", 18);
        if (show_age) hdr += fit("AGE", 6);
        out += ansi::move_to(row++, 1);
        out += c ? ansi::fg256(240) + truncate_to(hdr, w) + ansi::kReset
                 : truncate_to(hdr, w);
        out += ansi::kClearLine;
    }

    if (last_view_.empty()) {
        out += ansi::move_to(row++, 1);
        out += ansi::kClearLine;
        const char* msg = filter_.empty() && cfg_.min_rssi <= -110
                              ? "  Listening… no advertisements yet."
                              : "  Nothing matches the current filters. "
                                "(f cycles the floor, / clears with Esc)";
        out += c ? ansi::fg256(244) + std::string(msg) + ansi::kReset : msg;
        while (row < top_row + rows) {
            out += ansi::move_to(row++, 1);
            out += ansi::kClearLine;
        }
        return;
    }

    // Scroll so the selection stays on screen.
    int sel_idx = 0;
    for (size_t i = 0; i < last_view_.size(); ++i) {
        if (last_view_[i]->addr == selected_addr_) {
            sel_idx = static_cast<int>(i);
            break;
        }
    }
    if (sel_idx < scroll_) scroll_ = sel_idx;
    if (sel_idx >= scroll_ + list_rows) scroll_ = sel_idx - list_rows + 1;
    scroll_ = std::clamp(scroll_, 0,
                         std::max(0, static_cast<int>(last_view_.size()) - list_rows));

    for (int i = 0; i < list_rows; ++i) {
        const int idx = scroll_ + i;
        out += ansi::move_to(row + i, 1);
        out += ansi::kClearLine;
        if (idx >= static_cast<int>(last_view_.size())) continue;

        const Device& d = *last_view_[idx];
        const bool sel = (idx == sel_idx);
        const bool stale = d.age_ms(now) > cfg_.stale_ms;
        const double rssi = d.smoothed();
        const double frac = rssi_fraction(rssi);

        std::string line;
        line += sel ? (c ? ansi::fg256(51) + "▸ " + ansi::kReset : "> ") : "  ";
        line += bar(frac, bar_w, c && !stale);
        line += " ";

        char rssi_buf[8];
        std::snprintf(rssi_buf, sizeof(rssi_buf), "%4d", static_cast<int>(std::lround(rssi)));
        line += colored(rssi_buf, heat_color(frac), c && !stale);
        line += " ";

        if (show_dist) {
            line += fit(format_distance(d.distance_m(cfg_.default_tx_1m, cfg_.path_loss)), 8);
        }

        std::string name = d.name.empty() ? "(unnamed)" : d.name;
        if (!d.kind.empty() && display_width(name) + display_width(d.kind) + 3 <=
                                   static_cast<size_t>(name_w)) {
            name += " [" + d.kind + "]";
        }
        std::string name_cell = fit(name, name_w);
        if (c) {
            if (d.name.empty()) {
                name_cell = ansi::fg256(244) + name_cell + ansi::kReset;
            } else if (sel) {
                name_cell = std::string(ansi::kBold) + name_cell + ansi::kReset;
            }
        }
        line += name_cell + " ";

        if (show_addr) {
            std::string a = fit(format_addr(d.addr), 18);
            line += c ? ansi::fg256(242) + a + ansi::kReset : a;
        }
        if (show_age) {
            std::string age = fit(format_age(d.age_ms(now)), 6);
            line += c ? (stale ? ansi::fg256(203) : ansi::fg256(242)) + age + ansi::kReset
                      : age;
        }

        if (stale && c) line = std::string(ansi::kDim) + line + ansi::kReset;
        out += truncate_to(line, w);
    }
}

void Ui::render_hunt(std::string& out, const Terminal& term, int top_row, int rows) {
    const int w = term.width();
    const bool c = term.color();
    const uint64_t now = now_ms();
    const Device* d = table_.find(hunt_addr_);

    int row = top_row;
    auto line = [&](const std::string& s) {
        if (row >= top_row + rows) return;
        out += ansi::move_to(row++, 1);
        out += ansi::kClearLine;
        out += truncate_to(s, w);
    };

    if (d == nullptr) {
        line("");
        line(c ? ansi::fg256(203) + "  Signal lost — device aged out of the table." +
                     ansi::kReset
               : "  Signal lost — device aged out of the table.");
        line("");
        line("  " + format_addr(hunt_addr_));
        line("");
        line(c ? ansi::fg256(244) + "  Esc returns to the list." + ansi::kReset
               : "  Esc returns to the list.");
        while (row < top_row + rows) line("");
        return;
    }

    const uint64_t age = d->age_ms(now);
    const bool live = age < 5000;
    const double rssi = d->smoothed();
    const double frac = rssi_fraction(rssi);
    const Trend tr = d->trend();

    // Title.
    {
        // The address already has its own line below; repeating it as the title
        // would just print it twice.
        std::string t = "  " + (d->name.empty() ? "(unnamed)" : d->name);
        if (!d->kind.empty()) t += "  [" + d->kind + "]";
        line(c ? std::string(ansi::kBold) + ansi::fg256(51) + t + ansi::kReset : t);
        std::string sub = "  " + format_addr(d->addr) + " · " +
                          (d->addr_type == AddrType::Public ? "public" : "random") +
                          " · " + std::to_string(d->packets) +
                          (d->packets == 1 ? " advert" : " adverts");
        if (d->tx_1m_override) sub += " · calibrated";
        line(c ? ansi::fg256(242) + sub + ansi::kReset : sub);
    }
    line("");

    // The proximity meter: the single most important thing on the screen, so it
    // gets the full width and three stacked rows of the same bar.
    const int meter_w = std::max(10, w - 8);
    const std::string meter = bar(frac, meter_w, c);
    for (int i = 0; i < 3; ++i) line("    " + meter);
    line("");

    // Readout.
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "    %d dBm", static_cast<int>(std::lround(rssi)));
        std::string s = buf;
        if (c) s = ansi::kBold + ansi::fg256(heat_color(frac)) + s + ansi::kReset;

        std::string dist = "   " + format_distance(
                                       d->distance_m(cfg_.default_tx_1m, cfg_.path_loss));
        s += c ? ansi::fg256(250) + dist + ansi::kReset : dist;

        char tbuf[64];
        std::snprintf(tbuf, sizeof(tbuf), "   %s %s  %+.1f dB", trend_glyph(tr),
                      trend_word(tr), d->trend_db());
        s += c ? std::string(ansi::kBold) + ansi::fg256(trend_color(tr)) + tbuf +
                     ansi::kReset
               : tbuf;
        line(s);
    }

    if (!live) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "    no advert for %s — reading is stale",
                      format_age(age).c_str());
        line(c ? ansi::fg256(203) + std::string(buf) + ansi::kReset : buf);
    } else {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "    last %s · best %d dBm · now %d dBm",
                      format_age(age).c_str(), d->best_rssi, d->last_rssi);
        line(c ? ansi::fg256(242) + std::string(buf) + ansi::kReset : buf);
    }
    line("");

    // History trace.
    {
        const int spark_w = std::max(10, w - 12);
        std::string s = "    " + sparkline(d->history, spark_w);
        line(c ? ansi::fg256(heat_color(frac)) + s + ansi::kReset : s);
        const size_t span_samples = d->history.size();
        std::string caption = "    last " + std::to_string(span_samples) +
                              " adverts, autoscaled";
        line(c ? ansi::fg256(240) + caption + ansi::kReset : caption);
    }
    line("");

    // How to actually use it. Worth the space: the technique is not obvious, and
    // the estimate is easy to over-trust.
    {
        const char* tips[] = {
            "    Sweep slowly. Turn your body — you are the antenna, and shielding",
            "    yourself from the device drops it 10 dB. Watch the trend, not the",
            "    number: distance is a rough model, and walls make it lie.",
        };
        for (const char* t : tips) line(c ? ansi::fg256(240) + std::string(t) + ansi::kReset : t);
    }

    while (row < top_row + rows) line("");
}

void Ui::render_help(std::string& out, const Terminal& term, int top_row, int rows) {
    const int w = term.width();
    const bool c = term.color();
    int row = top_row;
    auto line = [&](const std::string& s) {
        if (row >= top_row + rows) return;
        out += ansi::move_to(row++, 1);
        out += ansi::kClearLine;
        out += truncate_to(s, w);
    };

    auto head = [&](const char* s) {
        line(c ? std::string(ansi::kBold) + ansi::fg256(51) + "  " + s + ansi::kReset
               : std::string("  ") + s);
    };
    auto item = [&](const char* k, const char* desc) {
        std::string s = "    " + fit(k, 12) + desc;
        line(c ? ansi::fg256(250) + s + ansi::kReset : s);
    };

    line("");
    head("List");
    item("↑ ↓ j k", "move the selection");
    item("⏎", "hunt the selected device");
    item("/", "filter by name or address (Esc clears)");
    item("s", "cycle sort: signal, name, recent, found");
    item("f", "cycle the RSSI floor: all, -80, -65");
    item("n", "show only devices that advertise a name");
    item("p", "pause updates · r  forget everything");
    item("q", "quit");
    line("");
    head("Hunt");
    item("c", "calibrate: call the current reading 1 metre");
    item("C", "clear that calibration");
    item("b", "toggle the geiger beep (rate rises as you close in)");
    item("Esc", "back to the list");
    line("");
    head("Reading the signal");
    {
        const char* notes[] = {
            "    RSSI is noisy: 10 dB of swing while standing still is normal, so",
            "    the display is smoothed and the WARMER/COLDER call comes from a",
            "    fast average pulling away from a slow one.",
            "",
            "    The distance figure is a free-space model. Indoors, through a",
            "    pocket or a wall, treat it as an order of magnitude at best —",
            "    calibrate with c when you are one metre away to sharpen it.",
        };
        for (const char* n : notes)
            line(c ? ansi::fg256(244) + std::string(n) + ansi::kReset : n);
    }
    line("");
    line(c ? ansi::fg256(240) + std::string("    press any key to go back") + ansi::kReset
           : "    press any key to go back");

    while (row < top_row + rows) line("");
}

}  // namespace btfind
