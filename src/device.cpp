#include "device.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace btfind {

uint64_t now_ms() {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000ull +
           static_cast<uint64_t>(ts.tv_nsec) / 1000000ull;
}

std::string format_addr(const Addr& a) {
    char buf[18];
    std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1],
                  a[2], a[3], a[4], a[5]);
    return std::string(buf);
}

// --- RssiHistory ------------------------------------------------------------

void RssiHistory::push(int8_t rssi, uint64_t t_ms) {
    buf_[head_] = Sample{rssi, t_ms};
    head_ = (head_ + 1) % kHistoryLen;
    if (count_ < kHistoryLen) ++count_;
}

void RssiHistory::clear() {
    head_ = 0;
    count_ = 0;
}

// Once the ring has wrapped, the oldest sample sits at head_; before that, at 0.
int8_t RssiHistory::rssi_at(size_t i) const {
    if (i >= count_) return -127;
    const size_t start = (count_ == kHistoryLen) ? head_ : 0;
    return buf_[(start + i) % kHistoryLen].rssi;
}

uint64_t RssiHistory::time_at(size_t i) const {
    if (i >= count_) return 0;
    const size_t start = (count_ == kHistoryLen) ? head_ : 0;
    return buf_[(start + i) % kHistoryLen].t_ms;
}

// --- Device -----------------------------------------------------------------

Trend Device::trend() const {
    if (!ema_primed) return Trend::Steady;
    const double d = ema_fast - ema_slow;
    if (d > kTrendThresholdDb) return Trend::Warmer;
    if (d < -kTrendThresholdDb) return Trend::Colder;
    return Trend::Steady;
}

double Device::tx_1m(double fallback) const {
    if (tx_1m_override) return *tx_1m_override;
    if (tx_1m_advertised) return static_cast<double>(*tx_1m_advertised);
    return fallback;
}

double Device::distance_m(double default_tx_1m, double path_loss) const {
    return distance_estimate(smoothed(), tx_1m(default_tx_1m), path_loss);
}

std::string Device::label() const {
    if (!name.empty()) return name;
    return format_addr(addr);
}

double distance_estimate(double rssi, double tx_1m, double path_loss) {
    if (path_loss <= 0.0) path_loss = kDefaultPathLoss;
    const double d = std::pow(10.0, (tx_1m - rssi) / (10.0 * path_loss));
    // Clamp to something a human might believe. BLE does not reach 1 km, and a
    // stronger-than-reference reading does not mean zero distance.
    return std::clamp(d, 0.05, 500.0);
}

// --- DeviceTable ------------------------------------------------------------

size_t DeviceTable::AddrHash::operator()(const Addr& a) const noexcept {
    // Six bytes fit in a word; FNV-1a over them is plenty for a table this size.
    uint64_t h = 1469598103934665603ull;
    for (uint8_t b : a) {
        h ^= b;
        h *= 1099511628211ull;
    }
    return static_cast<size_t>(h);
}

Device& DeviceTable::observe(const AdvEvent& ev, const AdInfo& info) {
    auto [it, inserted] = devices_.try_emplace(ev.addr);
    Device& d = it->second;

    if (inserted) {
        d.addr = ev.addr;
        d.addr_type = ev.addr_type;
        d.first_seen = ev.mono_time;
        d.best_rssi = ev.rssi;
    }

    // Scan responses carry the name while the advert itself often does not, so
    // never let a later nameless packet erase what we already learned.
    if (!info.name.empty() && (d.name.empty() || (info.name_is_complete && !d.name_is_complete))) {
        d.name = info.name;
        d.name_is_complete = info.name_is_complete;
    }
    if (info.company_id && !d.company_id) d.company_id = info.company_id;
    if (info.tx_power) d.tx_power_ad = info.tx_power;
    if (info.beacon_tx_1m) d.tx_1m_advertised = info.beacon_tx_1m;
    if (d.kind.empty()) d.kind = guess_kind(info);

    // Time-aware EMA: advertising rates differ by an order of magnitude between
    // devices, so a per-sample alpha would smooth a 10 Hz beacon far harder than
    // a 1 Hz one. Deriving alpha from elapsed time gives both the same response
    // in seconds, which is the unit the user actually perceives.
    const double rssi = ev.rssi;
    if (!d.ema_primed) {
        d.ema_fast = d.ema_slow = rssi;
        d.ema_primed = true;
    } else {
        const double dt = static_cast<double>(ev.mono_time - d.last_seen);
        const double a_fast = 1.0 - std::exp(-dt / kTauFastMs);
        const double a_slow = 1.0 - std::exp(-dt / kTauSlowMs);
        d.ema_fast += a_fast * (rssi - d.ema_fast);
        d.ema_slow += a_slow * (rssi - d.ema_slow);
    }

    d.last_rssi = ev.rssi;
    if (ev.rssi > d.best_rssi) d.best_rssi = ev.rssi;
    d.last_seen = ev.mono_time;
    ++d.packets;
    d.history.push(ev.rssi, ev.mono_time);

    return d;
}

size_t DeviceTable::prune(uint64_t now, uint64_t max_age_ms) {
    size_t removed = 0;
    for (auto it = devices_.begin(); it != devices_.end();) {
        if (now - it->second.last_seen > max_age_ms) {
            it = devices_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

Device* DeviceTable::find(const Addr& a) {
    auto it = devices_.find(a);
    return it == devices_.end() ? nullptr : &it->second;
}

const Device* DeviceTable::find(const Addr& a) const {
    auto it = devices_.find(a);
    return it == devices_.end() ? nullptr : &it->second;
}

namespace {

bool contains_ci(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(haystack.begin(), haystack.end(), needle.begin(),
                          needle.end(), [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          });
    return it != haystack.end();
}

}  // namespace

std::vector<const Device*> DeviceTable::view(SortMode mode, int min_rssi,
                                             bool named_only,
                                             const std::string& text_filter,
                                             uint64_t now) const {
    (void)now;
    std::vector<const Device*> out;
    out.reserve(devices_.size());

    for (const auto& [addr, d] : devices_) {
        if (d.smoothed() < min_rssi) continue;
        if (named_only && d.name.empty()) continue;
        if (!text_filter.empty() && !contains_ci(d.name, text_filter) &&
            !contains_ci(format_addr(d.addr), text_filter)) {
            continue;
        }
        out.push_back(&d);
    }

    // Every comparator falls through to address order. Without that tiebreak,
    // devices with equal keys swap places on each redraw and the list shimmers.
    auto by_addr = [](const Device* a, const Device* b) { return a->addr < b->addr; };

    switch (mode) {
        case SortMode::Rssi:
            std::sort(out.begin(), out.end(), [&](const Device* a, const Device* b) {
                if (a->smoothed() != b->smoothed()) return a->smoothed() > b->smoothed();
                return by_addr(a, b);
            });
            break;
        case SortMode::Name:
            std::sort(out.begin(), out.end(), [&](const Device* a, const Device* b) {
                // Named devices first; anonymous ones are rarely what you hunt.
                const bool an = a->name.empty(), bn = b->name.empty();
                if (an != bn) return bn;
                if (a->name != b->name) return a->name < b->name;
                return by_addr(a, b);
            });
            break;
        case SortMode::Age:
            std::sort(out.begin(), out.end(), [&](const Device* a, const Device* b) {
                if (a->last_seen != b->last_seen) return a->last_seen > b->last_seen;
                return by_addr(a, b);
            });
            break;
        case SortMode::FirstSeen:
            std::sort(out.begin(), out.end(), [&](const Device* a, const Device* b) {
                if (a->first_seen != b->first_seen) return a->first_seen < b->first_seen;
                return by_addr(a, b);
            });
            break;
    }
    return out;
}

const char* sort_mode_name(SortMode m) {
    switch (m) {
        case SortMode::Rssi: return "signal";
        case SortMode::Name: return "name";
        case SortMode::Age: return "recent";
        case SortMode::FirstSeen: return "found";
    }
    return "?";
}

SortMode next_sort_mode(SortMode m) {
    switch (m) {
        case SortMode::Rssi: return SortMode::Name;
        case SortMode::Name: return SortMode::Age;
        case SortMode::Age: return SortMode::FirstSeen;
        case SortMode::FirstSeen: return SortMode::Rssi;
    }
    return SortMode::Rssi;
}

}  // namespace btfind
