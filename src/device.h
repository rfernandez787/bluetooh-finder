// Per-device state and the table that holds it.
//
// Raw BLE RSSI is far too noisy to walk around by: consecutive adverts from a
// stationary device routinely differ by 10 dB thanks to antenna orientation,
// multipath and channel hopping across the three advertising channels. Everything
// the UI shows is therefore smoothed, and the "am I getting closer" signal comes
// from comparing a fast and a slow average rather than from any single sample.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "ad_parse.h"
#include "scanner.h"

namespace btfind {

// Time constants for the two exponential averages, in milliseconds. The gap
// between them sets how twitchy the WARMER/COLDER readout feels.
constexpr double kTauFastMs = 800.0;
constexpr double kTauSlowMs = 4000.0;

// dB of fast-minus-slow separation before we call it a real move.
constexpr double kTrendThresholdDb = 1.2;

constexpr size_t kHistoryLen = 120;

// Default reference power: RSSI a typical BLE device shows at 1 m. Only used when
// the advert carries no calibrated figure of its own.
constexpr double kDefaultTx1m = -59.0;
constexpr double kDefaultPathLoss = 2.0;

enum class Trend { Colder, Steady, Warmer };

// Fixed-capacity RSSI ring, oldest-first when read out.
class RssiHistory {
public:
    void push(int8_t rssi, uint64_t t_ms);
    void clear();

    size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }

    // i in [0, size()); 0 is the oldest retained sample.
    int8_t rssi_at(size_t i) const;
    uint64_t time_at(size_t i) const;

private:
    struct Sample {
        int8_t rssi;
        uint64_t t_ms;
    };
    std::array<Sample, kHistoryLen> buf_{};
    size_t head_ = 0;   // next write position
    size_t count_ = 0;
};

struct Device {
    Addr addr{};
    AddrType addr_type = AddrType::Public;
    std::string name;
    bool name_is_complete = false;
    std::string kind;
    std::optional<uint16_t> company_id;

    int8_t last_rssi = -127;
    int8_t best_rssi = -127;
    double ema_fast = 0.0;
    double ema_slow = 0.0;
    bool ema_primed = false;

    uint64_t first_seen = 0;
    uint64_t last_seen = 0;
    uint64_t packets = 0;

    // 1 m reference power: from calibration, then the advert, then the default.
    std::optional<double> tx_1m_override;   // set by the calibrate key
    std::optional<int8_t> tx_1m_advertised; // iBeacon/Eddystone calibrated power
    std::optional<int8_t> tx_power_ad;      // AD 0x0A, radiated power (not 1 m)

    RssiHistory history;

    // Smoothed RSSI, which is what the whole UI displays.
    double smoothed() const { return ema_primed ? ema_fast : last_rssi; }

    Trend trend() const;

    // Signed dB of movement; positive means approaching.
    double trend_db() const { return ema_primed ? ema_fast - ema_slow : 0.0; }

    double tx_1m(double fallback) const;

    // Rough metres from the log-distance path-loss model. See distance_estimate().
    double distance_m(double default_tx_1m, double path_loss) const;

    uint64_t age_ms(uint64_t now) const { return now - last_seen; }

    // Display name, falling back to the address when nothing was advertised.
    std::string label() const;
};

// d = 10 ^ ((tx_1m - rssi) / (10 * n))
//
// This is the log-distance path-loss model. It assumes free space and an
// isotropic antenna, so indoors — through a sofa, in a pocket — it is wrong by a
// factor of several. It is exposed as a coarse hint only; the trend is the part
// worth trusting.
double distance_estimate(double rssi, double tx_1m, double path_loss);

enum class SortMode { Rssi, Name, Age, FirstSeen };
const char* sort_mode_name(SortMode m);
SortMode next_sort_mode(SortMode m);

class DeviceTable {
public:
    // Folds one advertisement in. Returns the affected device.
    Device& observe(const AdvEvent& ev, const AdInfo& info);

    // Drops devices unseen for longer than `max_age_ms`.
    // Returns how many were removed.
    size_t prune(uint64_t now, uint64_t max_age_ms);

    Device* find(const Addr& a);
    const Device* find(const Addr& a) const;

    size_t size() const { return devices_.size(); }
    void clear() { devices_.clear(); }

    // Devices passing the filters, ordered by `mode`. Ties break on address so
    // the list never flickers between equal-RSSI neighbours.
    std::vector<const Device*> view(SortMode mode, int min_rssi, bool named_only,
                                    const std::string& text_filter,
                                    uint64_t now) const;

private:
    struct AddrHash {
        size_t operator()(const Addr& a) const noexcept;
    };
    std::unordered_map<Addr, Device, AddrHash> devices_;
};

}  // namespace btfind
