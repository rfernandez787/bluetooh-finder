#pragma once

#include <string>

#include "device.h"
#include "term.h"

namespace btfind {

struct Config {
    double default_tx_1m = kDefaultTx1m;
    double path_loss = kDefaultPathLoss;
    int min_rssi = -110;
    uint64_t stale_ms = 30000;   // dim beyond this
    uint64_t prune_ms = 120000;  // forget beyond this
    bool bell = false;
};

enum class Mode { List, Hunt, Help };

class Ui {
public:
    Ui(DeviceTable& table, Config& cfg) : table_(table), cfg_(cfg) {}

    // Returns false when the user asked to quit.
    bool handle_key(const KeyEvent& ev, Terminal& term);

    // Composes a complete frame for the current terminal size.
    std::string render(const Terminal& term, const std::string& source,
                       bool scanning_ok, const std::string& notice);

    // Rings the bell if hunt mode is due one. Called every loop iteration.
    void tick_bell(Terminal& term);

    Mode mode() const { return mode_; }
    bool paused() const { return paused_; }

    // Milliseconds until the next thing that needs a redraw or a bell.
    int next_wakeup_ms() const;

private:
    void render_list(std::string& out, const Terminal& term, int top_row,
                     int rows);
    void render_hunt(std::string& out, const Terminal& term, int top_row,
                     int rows);
    void render_help(std::string& out, const Terminal& term, int top_row,
                     int rows);
    std::string footer(const Terminal& term, const std::string& notice) const;

    void move_selection(int delta);
    const Device* selected_device() const;

    DeviceTable& table_;
    Config& cfg_;

    Mode mode_ = Mode::List;
    Mode help_return_mode_ = Mode::List;
    SortMode sort_ = SortMode::Rssi;
    bool paused_ = false;
    bool named_only_ = false;
    bool filter_input_ = false;
    std::string filter_;

    // Selection is held as an address, not an index: the list reorders itself
    // constantly under signal sort, and an index would slide onto a different
    // device every time two neighbours swapped.
    Addr selected_addr_{};
    bool has_selection_ = false;
    int scroll_ = 0;

    // The address being hunted is captured on entry so the view survives the
    // device dropping out of the filtered list.
    Addr hunt_addr_{};

    uint64_t next_bell_ms_ = 0;

    // Snapshot of the last rendered list, so key handling and drawing agree on
    // what "the row below" means.
    mutable std::vector<const Device*> last_view_;
};

// --- helpers, exposed for the self-test -------------------------------------

// Maps an RSSI to 0.0 (hopeless) .. 1.0 (right here).
double rssi_fraction(double rssi);

// A bar of `width` cells filled to `frac`, using block characters.
std::string bar(double frac, int width, bool color, const char* fill_glyph = nullptr);

// Sparkline over the history, most recent on the right.
std::string sparkline(const RssiHistory& h, int width);

// Human distance: "~1.2 m", "~14 m", "~120 m".
std::string format_distance(double metres);

// Human age: "0.4s", "12s", "3m", "1h".
std::string format_age(uint64_t ms);

// Bell interval in ms for a given smoothed RSSI: fast when close, slow when far.
int bell_interval_ms(double rssi);

}  // namespace btfind
